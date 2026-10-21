#include "ros_cdr.hpp"
#include "telemetry.hpp"
#include <cmath>
#include <limits>
#include <numbers>

namespace bridge {
void add_sensor_configuration(Packet& packet,const json& configuration) {
    const auto text=configuration.dump();
    append_cdr(packet,"/ets2/sensors/config",text.size()+32,[&](Cdr& c){c<<text;});
}
void append_motion_messages(Packet& packet,const MotionSample& sample,const Demand& demand,uint64_t& last_gnss_us) {
    const auto us=sample.stamp_us;
    const auto& w=sample.wheels;
    if(!w.indices.empty() && demand.contains("/ets2/wheels/state"))
        append_cdr(packet,"/ets2/wheels/state",256+w.indices.size()*32,[&](Cdr& c){header(c,us,"base_link");c<<w.indices<<w.angular<<w.steering<<w.radius<<w.ground;});
    if(sample.odometry && demand.contains("/ets2/wheels/odometry")) {
        const auto& o=*sample.odometry;
        append_cdr(packet,"/ets2/wheels/odometry",1024,[&](Cdr& c){
            header(c,us,"wheel_odom");c<<std::string("base_link");pose(c,{o.x,o.y,0},{0,0,std::sin(o.yaw/2),std::cos(o.yaw/2)});
            for(int i=0;i<36;++i) c<<double{0};const V v{o.speed,0,0},angular{0,0,o.yaw_rate};
            c.serialize_array(v.data(),3);c.serialize_array(angular.data(),3);for(int i=0;i<36;++i) c<<double{0};});
    }
    if(sample.imu && demand.contains("/ets2/imu/data_raw")) {
        const auto& imu=*sample.imu;
        append_cdr(packet,"/ets2/imu/data_raw",512,[&](Cdr& c){header(c,us,"imu_link");
            const Q q{0,0,0,1};c.serialize_array(q.data(),4);for(int i=0;i<9;++i) c<<double(i==0?-1:0);
            c.serialize_array(imu.gyro.data(),3);for(int i=0;i<9;++i) c<<double{0};
            c.serialize_array(imu.force.data(),3);for(int i=0;i<9;++i) c<<double{0};});
    }
    if(sample.gnss && sample.gnss->stamp_us!=last_gnss_us && demand.contains("/ets2/gnss/fix")) {
        const auto& fix=*sample.gnss;
        append_cdr(packet,"/ets2/gnss/fix",256,[&](Cdr& c){header(c,fix.stamp_us,"gnss_link");c<<int8_t{0}<<uint16_t{1};
            c.serialize_array(fix.lla.data(),3);for(int i=0;i<9;++i) c<<double{0};c<<uint8_t{2};});
        last_gnss_us=fix.stamp_us;
    }
}
Packet static_messages(const json& rig,const json& patterns,const std::string& session,const json& sensors) {
    struct Mount {std::string name;V p;Q q;std::string parent="cabin";};std::vector<Mount> mounts;
    for(const auto& view:rig.at("views")) {
        const int slot=view.at("slot");
        if(view.at("basis")!="cabin") throw std::runtime_error("ROS mounting tree requires cabin mounts");
        const auto p=mul(transpose(base_to_model),view.at("position").get<V>());
        const auto r=mul(transpose(base_to_model),from_quat(view.at("quaternion_wxyz").get<Q>()));
        mounts.push_back({view.at("camera_id").get<std::string>()+"_optical",p,quaternion(mul(r,optical))});
        const auto source="mirror"+std::to_string(slot);
        if(patterns.contains(source) && patterns.at(source).at("axis_camera")==source)
            mounts.push_back({patterns.at(source).at("name"),p,quaternion(mul(r,base_to_model))});
    }
    mounts.push_back({"imu_link",sensors.at("imu_mount_base_m").get<V>(),{0,0,0,1},"base_link"});
    mounts.push_back({"gnss_link",sensors.at("gnss_mount_base_m").get<V>(),{0,0,0,1},"base_link"});
    Packet packet{{{"session",session}}, {}};
    append_cdr(packet,"/tf_static",512+mounts.size()*256,[&](Cdr& c){
        c<<uint32_t(mounts.size());for(const auto& m:mounts) {header(c,0,m.parent);c<<m.name;pose(c,m.p,m.q);}
    });return packet;
}
void add_diagnostics(Packet& packet,const json& values) {
    append_cdr(packet,"/diagnostics",8192,[&](Cdr& c){
        header(c,values.value("stamp_us",uint64_t{0}),"base_link");c<<uint32_t{1}<<uint8_t{0}
            <<std::string("ets2_relay")<<std::string("Streaming")<<std::string("Windows relay")<<uint32_t(values.size());
        for(const auto& item:values.items()) c<<item.key()<<item.value().dump();
    });
}
Packet state_messages(const json& state,const std::string& session,const V& base,bool gt_localization_tf) {
    const auto us=state.at("paused_simulation_time_us").get<uint64_t>();
    Packet packet{{{"session",session}}, {}};
    append_cdr(packet,"/clock",32,[&](Cdr& c){stamp(c,us);});
    append_cdr(packet,"/ets2/vehicle/state",512,[&](Cdr& c){
        header(c,us,"base_link");c<<state.at("frame_id").get<uint64_t>()<<state.at("paused").get<bool>();
        for(const auto* key:{"render_time_us","simulation_time_us","paused_simulation_time_us"}) c<<state.at(key).get<uint64_t>();
        for(const auto* key:{"truck.speed","truck.engine.rpm","truck.input.steering","truck.input.throttle","truck.input.brake"}) c<<channel(state,key);
        for(const auto* key:{"truck.local.velocity.linear","truck.local.velocity.angular","truck.local.acceleration.linear"}) {
            const auto value=std::string_view(key)=="truck.local.velocity.angular"?angular_vector_channel(state,key):vector_channel(state,key);
            c.serialize_array(value.data(),3);
        }
    });
    append_cdr(packet,"/ets2/vehicle/actuation",256,[&](Cdr& c){
        header(c,us,"base_link");c<<state.at("frame_id").get<uint64_t>();
        for(const auto* key:{"truck.input.steering","truck.input.throttle","truck.input.brake",
                            "truck.effective.steering","truck.effective.throttle","truck.effective.brake"}) c<<channel(state,key);
        for(const auto* key:{"truck.engine.gear","truck.displayed.gear"}) {
            const auto& sdk=state.at("sdk");const bool available=sdk.contains(key) && sdk.at(key).value("available",false);
            c<<available<<(available?sdk.at(key).at("value").get<int32_t>():int32_t{0});
        }
    });
    append_cdr(packet,"/ets2/vehicle/powertrain",1024,[&](Cdr& c){
        header(c,us,"base_link");c<<state.at("frame_id").get<uint64_t>();
        for(const auto* key:{"truck.engine.rpm","truck.effective.clutch","truck.cruise_control","truck.oil.temperature",
                            "truck.oil.pressure","truck.water.temperature","truck.battery.voltage","truck.fuel.amount",
                            "truck.fuel.consumption.average","truck.adblue","truck.brake.air.pressure","truck.brake.temperature"}) c<<channel(state,key);
        const auto& sdk=state.at("sdk");
        const auto flag=[&](const char* key) {return sdk.contains(key) && sdk.at(key).value("available",false) && sdk.at(key).at("value").get<bool>();};
        const bool retarder=sdk.contains("truck.brake.retarder") && sdk.at("truck.brake.retarder").value("available",false);
        c<<(retarder?sdk.at("truck.brake.retarder").at("value").get<uint32_t>():uint32_t{0});
        c<<flag("truck.brake.motor")<<flag("truck.brake.parking")<<flag("truck.lift_axle")<<flag("truck.lblinker")<<flag("truck.rblinker");
        const auto nan=std::numeric_limits<double>::quiet_NaN();
        const json* drivetrain=state.contains("engine") && state.at("engine").contains("drivetrain") ? &state.at("engine").at("drivetrain") : nullptr;
        const bool available=drivetrain && drivetrain->value("available",false);
        c<<available;
        for(const auto* key:{"engine_speed_radps","engine_torque_nm","output_shaft_speed_radps","output_torque_nm","gear_ratio","retarder_level","engine_brake"})
            c<<(available?drivetrain->at(key).get<double>():nan);
        c<<(available?drivetrain->at("wheel_brake_torque_nm").get<std::vector<double>>():std::vector<double>{});
        c<<(available?drivetrain->value("trailer_wheel_brake_torque_nm",std::vector<double>{}):std::vector<double>{});
    });
    if(state.contains("drive")) {
        const auto& drive=state.at("drive");
        append_cdr(packet,"/ets2/drive/state",2048,[&](Cdr& c){
            header(c,us,"base_link");c<<state.at("frame_id").get<uint64_t>();
            for(const auto* key:{"available","permitted","profile_supported","armed","active"}) c<<drive.at(key).get<bool>();
            c<<drive.at("reason").get<std::string>()<<drive.at("owner").get<std::string>();
            for(const auto* key:{"epoch","sequence","deadline_ms","command_window_ms"}) c<<drive.at(key).get<uint64_t>();
            for(const auto* key:{"steering","throttle","brake","manual_steering","manual_throttle","manual_brake"}) c<<drive.at(key).get<float>();
            for(const auto* key:{"truck.effective.steering","truck.effective.throttle","truck.effective.brake"}) c<<channel(state,key);
            c<<drive.at("error").get<std::string>();
            c<<static_cast<uint8_t>(drive.value("axes",drive.at("armed").get<bool>()?3u:0u))<<drive.value("independent_axes",false);
            c<<drive.value("auxiliary_brakes",false)<<static_cast<int8_t>(drive.value("retarder",-1))<<drive.value("engine_brake",false);
        });
    }
    if(state.contains("sdk")) {
        // Connected trailer's SDK placement (its origin, yaw only) for the LCC's hitch angle.
        // SDK heading is turns counterclockwise from SCS -Z; ROS world yaw = 2*pi*h + pi/2.
        const auto& sdk=state.at("sdk");
        const auto value=[&](const char* key)->const json* {
            return sdk.contains(key) && sdk.at(key).value("available",false) && !sdk.at(key).at("value").is_null()?&sdk.at(key).at("value"):nullptr;
        };
        const auto* connected=value("trailer.0.connected");const auto* placement=value("trailer.0.world.placement");
        if(connected && connected->is_boolean() && connected->get<bool>() && placement) {
            const auto position=placement->at("position_m").get<V>();
            const auto yaw=placement->at("euler_rotations").at(0).get<double>()*2*std::numbers::pi+std::numbers::pi/2;
            const V world{position[0],-position[2],position[1]};const Q q{0,0,std::sin(yaw/2),std::cos(yaw/2)};
            append_cdr(packet,"/ets2/ground_truth/trailer/pose",256,[&](Cdr& c){header(c,us,"world");pose(c,world,q);});
        }
    }
    if(state.contains("engine") && state["engine"].contains("vehicle") && state["engine"]["vehicle"].value("available",false)) {
        const auto& p=state["engine"]["vehicle"]["pose_physics"];
        const M rotation=from_quat(p.at("quaternion_wxyz").get<Q>());
        const auto ego=world_base_pose(p.at("position_m").get<V>(),rotation,base);const auto q=quaternion(ego.rotation);
        append_cdr(packet,"/ets2/ground_truth/ego/pose",256,[&](Cdr& c){header(c,us,"world");pose(c,ego.position,q);});
        // Ground truth stays visible for evaluation; base_link gets this parent only
        // when no stack localization publishes odom->base_link.
        append_cdr(packet,"/tf",512,[&](Cdr& c){
            c<<uint32_t(gt_localization_tf?2:1);header(c,us,"world");c<<std::string("gt/base_link");pose(c,ego.position,q);
            if(gt_localization_tf) {header(c,us,"world");c<<std::string("base_link");pose(c,ego.position,q);}
        });
    }
    if(state.contains("engine") && state["engine"].contains("traffic")) {
        const auto& traffic=state["engine"]["traffic"];
        std::vector<uint64_t> ids;std::array<std::vector<double>,8> values;
        if(traffic.at("available").get<bool>()) for(const auto& actor:traffic.at("vehicles")) {
            ids.push_back(actor.at("id").get<uint64_t>());int i=0;
            for(const auto* key:{"x","y","yaw","speed_mps","length_m","width_m"}) values[i++].push_back(actor.at(key).get<double>());
            // Heights from core 0.47.1 on.
            for(const auto* key:{"z","height_m"}) values[i++].push_back(actor.value(key,std::numeric_limits<double>::quiet_NaN()));
        }
        append_cdr(packet,"/ets2/ground_truth/traffic",256+ids.size()*72,[&](Cdr& c){
            header(c,us,"world");c<<state.at("frame_id").get<uint64_t>()<<traffic.at("available").get<bool>()<<traffic.value("error",std::string{});
            c<<ids;for(const auto& axis:values) c<<axis;
        });
    }
    return packet;
}
}
