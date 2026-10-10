#include "motion_sensors.hpp"
#include <iostream>
#include <numbers>
#include <string>

using namespace bridge;
constexpr double pi=std::numbers::pi;
static void check(bool value,const char* what) {if(!value) throw std::runtime_error(std::string("Motion sensor contract failed: ")+what);}
static bool near(double a,double b,double tolerance) {return std::abs(a-b)<=tolerance;}

struct Wheel {uint32_t index;double omega_radps,steering_rad;bool ground=true;};
// SDK channels carry rotations and rotations/s; the relay converts to rad.
static json state(uint64_t us,const std::vector<Wheel>& wheels,bool paused=false,uint64_t generation=7) {
    json sdk=json::object();
    for(const auto& w:wheels) {
        const auto suffix="["+std::to_string(w.index)+"]";
        sdk["truck.wheel.angular_velocity"+suffix]={{"available",true},{"value",w.omega_radps/(2*pi)}};
        sdk["truck.wheel.steering"+suffix]={{"available",true},{"value",w.steering_rad/(2*pi)}};
        sdk["truck.wheel.on_ground"+suffix]={{"available",true},{"value",w.ground}};
    }
    return {{"truck_generation",generation},{"paused_simulation_time_us",us},{"paused",paused},{"sdk",sdk}};
}
// SCS model space: x right, y up, z backward. Identity orientation faces -z.
static json with_vehicle(json s,V position_m,V linear_model_mps,V angular_model_rps) {
    s["sdk"]["truck.local.velocity.linear"]={{"available",true},{"value",linear_model_mps}};
    s["sdk"]["truck.local.velocity.angular"]={{"available",true},{"value",angular_model_rps}};
    s["engine"]={{"vehicle",{{"available",true},{"mass_center_local_m",V{0,0,0}},
        {"pose_physics",{{"quaternion_wxyz",Q{1,0,0,0}},{"position_m",position_m}}}}}};
    return s;
}
// base_link: x forward, y left. Front axle 3.8 m ahead, track 2.08 m, radius 0.5 m.
static VehicleProfile truck() {
    return {json::object(),json::object(),V{0,0,0},
        {{0,V{3.8,1.04,0},.5,true},{1,V{3.8,-1.04,0},.5,true},{2,V{0,1.04,0},.5,true},{3,V{0,-1.04,0},.5,true}},7};
}
// Rolling without slip for base speed v and yaw rate w; front wheels steer to Ackermann angles.
static std::vector<Wheel> rolling(const VehicleProfile& profile,double v,double w) {
    std::vector<Wheel> result;
    for(const auto& g:profile.wheels) {
        const double along=v-w*g.position[1],across=w*g.position[0];
        const double delta=g.position[0]>0?std::atan2(across,along):0;
        result.push_back({g.index,(along*std::cos(delta)+across*std::sin(delta))/g.radius,delta});
    }
    return result;
}

int main() {
    const auto profile=truck();
    {
        MotionSensors sensors(profile,json::object());
        auto first=sensors.update(state(1'000'000,rolling(profile,10,0)));
        check(first.odometry && near(first.odometry->speed,10,1e-9) && near(first.odometry->yaw_rate,0,1e-9),"straight speed fit");
        auto second=sensors.update(state(1'100'000,rolling(profile,10,0)));
        check(near(second.odometry->x,1,1e-9) && near(second.odometry->y,0,1e-12),"straight integration");
        check(!first.imu && !first.gnss,"no IMU or GNSS without engine pose");
    }
    {
        MotionSensors sensors(profile,json::object());const double v=10,w=.2;
        auto wheels=rolling(profile,v,w);wheels[3].omega_radps=1e6;wheels[3].ground=false;
        auto first=sensors.update(state(1'000'000,wheels));
        check(near(first.odometry->speed,v,1e-9) && near(first.odometry->yaw_rate,w,1e-9),"Ackermann turn fit ignores airborne wheels");
        check(near(first.wheels.steering[0],wheels[0].steering_rad,1e-12) && first.wheels.steering[0]>0,"left wheel steering is positive rad");
        auto second=sensors.update(state(1'100'000,wheels));const double theta=w*.1;
        check(near(second.odometry->yaw,theta,1e-12),"turn yaw integration");
        check(near(second.odometry->x,v/w*std::sin(theta),1e-9) && near(second.odometry->y,v/w*(1-std::cos(theta)),1e-9),"turn follows exact arc");
        auto paused=sensors.update(state(1'200'000,wheels,true));check(!paused.odometry,"paused frame has no odometry");
        auto resumed=sensors.update(state(5'000'000,wheels));check(near(resumed.odometry->x,second.odometry->x,1e-12),"pause gap is not integrated");
    }
    {
        MotionSensors sensors(profile,json::object());
        sensors.update(with_vehicle(state(1'000'000,{}),V{0,0,0},V{0,0,0},V{0,0,0}));
        auto still=sensors.update(with_vehicle(state(1'010'000,{}),V{0,0,0},V{0,0,0},V{0,0,0}));
        check(still.imu && near(still.imu->force[0],0,1e-6) && near(still.imu->force[1],0,1e-6) && near(still.imu->force[2],9.81,1e-5),"stationary IMU reads +g up");
        auto turning=sensors.update(with_vehicle(state(1'020'000,{}),V{0,0,0},V{0,0,0},V{0,.1,0}));
        check(near(turning.imu->gyro[2],.2*pi,1e-9),"left yaw is positive z rad/s");
    }
    {
        const V reference{37.5,127,50};MotionSensors sensors(profile,{{"gnss_reference_lla",reference}});
        auto first=sensors.update(with_vehicle(state(1'000'000,{}),V{0,0,0},V{0,0,0},V{0,0,0}));
        check(first.gnss && near(first.gnss->lla[0],37.5,1e-9) && near(first.gnss->lla[1],127,1e-9) && near(first.gnss->lla[2],51,1e-6),"first fix is reference plus 1 m mount");
        auto same=sensors.update(with_vehicle(state(1'050'000,{}),V{50,0,0},V{0,0,0},V{0,0,0}));
        check(same.gnss->stamp_us==1'000'000,"GNSS holds the 10 Hz sample");
        auto east=sensors.update(with_vehicle(state(1'100'000,{}),V{100,0,0},V{0,0,0},V{0,0,0}));
        const double a=6378137.,e2=6.6943799901413165e-3,lat=37.5*pi/180,n=a/std::sqrt(1-e2*std::sin(lat)*std::sin(lat));
        check(east.gnss->stamp_us==1'100'000 && near(east.gnss->lla[0],37.5,1e-7),"east move keeps latitude");
        check(near(east.gnss->lla[1],127+std::atan2(100,(n+51)*std::cos(lat))*180/pi,1e-9),"model +x is east");
    }
    {
        MotionSensors sensors(profile,json::object());bool rejected=false;
        try {sensors.update(state(1'000'000,{},false,8));} catch(const std::runtime_error&) {rejected=true;}
        check(rejected,"truck generation change is rejected");
    }
    std::cout<<"Wheel odometry, IMU and GNSS contracts passed\n";
}
