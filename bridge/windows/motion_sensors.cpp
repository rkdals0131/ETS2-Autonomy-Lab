#include "motion_sensors.hpp"
#include "telemetry.hpp"

namespace bridge {
MotionSensors::MotionSensors(const VehicleProfile& vehicle,const json& settings):
    wheels_(vehicle.wheels),base_(vehicle.base),
    mount_(settings.value("imu_mount_base_m",V{0,0,1})),
    gnss_mount_(settings.value("gnss_mount_base_m",V{0,0,1})),
    reference_(settings.value("gnss_reference_lla",V{0,0,0})),generation_(vehicle.generation) {
    if(!finite(mount_) || !finite(gnss_mount_) || !finite(reference_) || std::abs(reference_[0])>=90 || std::abs(reference_[1])>180)
        throw std::runtime_error("Invalid motion sensor mounts or WGS84 reference");
}
json MotionSensors::configuration() const {
    return {{"core_version","0.23.0"},{"model","ideal"},{"angular_velocity_unit","rad/s"},
        {"imu_mount_base_m",mount_},{"gnss_mount_base_m",gnss_mount_},{"gnss_reference_lla",reference_},
        {"gnss_reference","virtual ENU at first observed base_link; WGS84 ellipsoid"},
        {"gnss_hz",10},{"motion_rate","one fresh SDK frame; no interpolation"},
        {"origin_lifetime","relay process; preserved across transport reconnect and capture stop"},
        {"wheel_odometry","rolling constraints, zero initial pose, no GT correction; lateral velocity and slip covariance unmodelled"}};
}
static V wgs84(V local,V reference) {
    constexpr double a=6378137.,e2=6.6943799901413165e-3;
    const auto lat=reference[0]*std::numbers::pi/180,lon=reference[1]*std::numbers::pi/180;
    const auto s=std::sin(lat),c=std::cos(lat),sl=std::sin(lon),cl=std::cos(lon),n=a/std::sqrt(1-e2*s*s);
    V ecef{(n+reference[2])*c*cl,(n+reference[2])*c*sl,(n*(1-e2)+reference[2])*s};
    ecef=add(ecef,mul(M{-sl,-s*cl,c*cl,cl,-s*sl,c*sl,0,c,s},local));
    const auto p=std::hypot(ecef[0],ecef[1]);double phi=std::atan2(ecef[2],p*(1-e2)),height=0;
    for(int i=0;i<8;++i) {const auto sp=std::sin(phi),np=a/std::sqrt(1-e2*sp*sp);height=p/std::cos(phi)-np;phi=std::atan2(ecef[2]+e2*np*sp,p);}
    height=p/std::cos(phi)-a/std::sqrt(1-e2*std::sin(phi)*std::sin(phi));
    return {phi*180/std::numbers::pi,std::atan2(ecef[1],ecef[0])*180/std::numbers::pi,height};
}
MotionSample MotionSensors::update(const json& state) {
    if(state.at("truck_generation").get<uint64_t>()!=generation_)
        throw std::runtime_error("Truck configuration changed; restart bridge to update sensor mounts and wheel geometry");
    MotionSample result;
    result.stamp_us=state.at("paused_simulation_time_us").get<uint64_t>();
    if(state.at("paused").get<bool>()) {previous_us_=previous_wheel_us_=0;return result;}
    const auto us=result.stamp_us;
    const auto dt=previous_us_ && us>previous_us_?(us-previous_us_)*1e-6:0;
    const auto wheel_dt=previous_wheel_us_ && us>previous_wheel_us_?(us-previous_wheel_us_)*1e-6:0;
    const auto& sdk=state.at("sdk");constexpr double tau=2*std::numbers::pi;
    auto& wheels=result.wheels;
    double aa=0,ab=0,bb=0,as=0,bs=0;
    for(const auto& wheel:wheels_) {
        const auto suffix="["+std::to_string(wheel.index)+"]";
        const auto velocity="truck.wheel.angular_velocity"+suffix,angle="truck.wheel.steering"+suffix,contact="truck.wheel.on_ground"+suffix;
        if(!sdk.contains(contact) || !sdk.at(contact).value("available",false)) continue;
        const auto omega=channel(state,velocity.c_str())*tau,delta=channel(state,angle.c_str())*tau;
        if(!std::isfinite(omega) || !std::isfinite(delta)) continue;
        const bool on_ground=sdk.at(contact).at("value").get<bool>();
        wheels.indices.push_back(wheel.index);wheels.angular.push_back(omega);wheels.steering.push_back(delta);
        wheels.radius.push_back(wheel.radius);wheels.ground.push_back(on_ground);
        if(on_ground && wheel.simulated) {
            const auto a=std::cos(delta),b=-wheel.position[1]*a+wheel.position[0]*std::sin(delta),speed=omega*wheel.radius;
            aa+=a*a;ab+=a*b;bb+=b*b;as+=a*speed;bs+=b*speed;
        }
    }
    const auto determinant=aa*bb-ab*ab;
    if(determinant>1e-10) {
        const auto speed=(as*bb-bs*ab)/determinant,rate=(bs*aa-as*ab)/determinant;
        if(wheel_dt>0) {
            const auto theta=rate*wheel_dt;
            const auto distance=std::abs(theta)>1e-8?speed*wheel_dt*std::sin(theta*.5)/(theta*.5):speed*wheel_dt;
            x_+=distance*std::cos(yaw_+theta*.5);y_+=distance*std::sin(yaw_+theta*.5);yaw_+=theta;
        }
        result.odometry=WheelOdometry{x_,y_,yaw_,speed,rate};previous_wheel_us_=us;
    } else previous_wheel_us_=0;

    if(!state.contains("engine") || !state.at("engine").contains("vehicle") || !state.at("engine").at("vehicle").value("available",false)) {
        previous_us_=0;return result;
    }
    const auto& vehicle=state.at("engine").at("vehicle");const auto& p=vehicle.at("pose_physics");
    const auto rotation=from_quat(p.at("quaternion_wxyz").get<Q>()),sensor_rotation=mul(rotation,base_to_model);
    const auto velocity=mul(sensor_rotation,vector_channel(state,"truck.local.velocity.linear"));
    const auto omega=mul(sensor_rotation,scale(vector_channel(state,"truck.local.velocity.angular"),tau));
    if(!finite(velocity) || !finite(omega)) {previous_us_=0;return result;}
    if(dt>0) {
        const auto r=mul(rotation,sub(add(base_,mul(base_to_model,mount_)),vehicle.at("mass_center_local_m").get<V>()));
        const auto acceleration=add(scale(sub(velocity,previous_velocity_),1/dt),
            add(cross(scale(sub(omega,previous_omega_),1/dt),r),cross(omega,cross(omega,r))));
        result.imu=ImuMeasurement{mul(transpose(sensor_rotation),sub(acceleration,V{0,-9.8100004196167,0})),mul(transpose(sensor_rotation),omega)};
    }
    previous_velocity_=velocity;previous_omega_=omega;previous_us_=us;
    const auto origin=p.at("position_m").get<V>();
    if(!anchored_) {anchor_=mul(enu,add(origin,mul(rotation,base_)));anchored_=true;}
    if(us>=next_gnss_us_) {
        const auto antenna=mul(enu,add(origin,mul(rotation,add(base_,mul(base_to_model,gnss_mount_)))));
        gnss_=GnssMeasurement{us,wgs84(sub(antenna,anchor_),reference_)};
        next_gnss_us_=(us/100000+1)*100000;
    }
    result.gnss=gnss_;
    return result;
}
}
