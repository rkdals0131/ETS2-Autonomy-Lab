#include "ros_cdr.hpp"
#include "telemetry.hpp"

namespace bridge {
void add_sensor_configuration(Packet& packet,const json& configuration) {
    const auto text=configuration.dump();
    add_message(packet,"/ets2/sensors/config",cdr(text.size()+32,[&](Cdr& c){c<<text;}));
}
void append_motion_messages(Packet& packet,const MotionSample& sample,const Demand& demand,uint64_t& last_gnss_us) {
    const auto us=sample.stamp_us;
    const auto& w=sample.wheels;
    if(!w.indices.empty() && demand.contains("/ets2/wheels/state"))
        add_message(packet,"/ets2/wheels/state",cdr(256+w.indices.size()*32,[&](Cdr& c){header(c,us,"base_link");c<<w.indices<<w.angular<<w.steering<<w.radius<<w.ground;}));
    if(sample.odometry && demand.contains("/ets2/wheels/odometry")) {
        const auto& o=*sample.odometry;
        add_message(packet,"/ets2/wheels/odometry",cdr(1024,[&](Cdr& c){
            header(c,us,"wheel_odom");c<<std::string("base_link");pose(c,{o.x,o.y,0},{0,0,std::sin(o.yaw/2),std::cos(o.yaw/2)});
            for(int i=0;i<36;++i) c<<double{0};const V v{o.speed,0,0},angular{0,0,o.yaw_rate};
            c.serialize_array(v.data(),3);c.serialize_array(angular.data(),3);for(int i=0;i<36;++i) c<<double{0};}));
    }
    if(sample.imu && demand.contains("/ets2/imu/data_raw")) {
        const auto& imu=*sample.imu;
        add_message(packet,"/ets2/imu/data_raw",cdr(512,[&](Cdr& c){header(c,us,"imu_link");
            const Q q{0,0,0,1};c.serialize_array(q.data(),4);for(int i=0;i<9;++i) c<<double(i==0?-1:0);
            c.serialize_array(imu.gyro.data(),3);for(int i=0;i<9;++i) c<<double{0};
            c.serialize_array(imu.force.data(),3);for(int i=0;i<9;++i) c<<double{0};}));
    }
    if(sample.gnss && sample.gnss->stamp_us!=last_gnss_us && demand.contains("/ets2/gnss/fix")) {
        const auto& fix=*sample.gnss;
        add_message(packet,"/ets2/gnss/fix",cdr(256,[&](Cdr& c){header(c,fix.stamp_us,"gnss_link");c<<int8_t{0}<<uint16_t{1};
            c.serialize_array(fix.lla.data(),3);for(int i=0;i<9;++i) c<<double{0};c<<uint8_t{2};}));
        last_gnss_us=fix.stamp_us;
    }
}
Packet static_messages(const json& rig,const json& patterns,const std::string& session,const json& settings) {
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
    mounts.push_back({"imu_link",settings.value("imu_mount_base_m",V{0,0,1}),{0,0,0,1},"base_link"});
    mounts.push_back({"gnss_link",settings.value("gnss_mount_base_m",V{0,0,1}),{0,0,0,1},"base_link"});
    Packet packet{{{"session",session}}, {}};
    add_message(packet,"/tf_static",cdr(512+mounts.size()*256,[&](Cdr& c){
        c<<uint32_t(mounts.size());for(const auto& m:mounts) {header(c,0,m.parent);c<<m.name;pose(c,m.p,m.q);}
    }));return packet;
}
void add_diagnostics(Packet& packet,const json& values) {
    add_message(packet,"/diagnostics",cdr(8192,[&](Cdr& c){
        header(c,values.value("stamp_us",uint64_t{0}),"base_link");c<<uint32_t{1}<<uint8_t{0}
            <<std::string("ets2_bridge")<<std::string("Streaming")<<std::string("Windows relay / WSL Jazzy")<<uint32_t(values.size());
        for(const auto& item:values.items()) c<<item.key()<<item.value().dump();
    }));
}
Packet state_messages(const json& state,const std::string& session,const V& base) {
    const auto us=state.at("paused_simulation_time_us").get<uint64_t>();
    Packet packet{{{"session",session}}, {}};
    add_message(packet,"/clock",cdr(32,[&](Cdr& c){stamp(c,us);}));
    add_message(packet,"/ets2/vehicle/state",cdr(512,[&](Cdr& c){
        header(c,us,"base_link");c<<state.at("frame_id").get<uint64_t>()<<state.at("paused").get<bool>();
        for(const auto* key:{"render_time_us","simulation_time_us","paused_simulation_time_us"}) c<<state.at(key).get<uint64_t>();
        for(const auto* key:{"truck.speed","truck.engine.rpm","truck.input.steering","truck.input.throttle","truck.input.brake"}) c<<channel(state,key);
        for(const auto* key:{"truck.local.velocity.linear","truck.local.velocity.angular","truck.local.acceleration.linear"}) {
            auto value=vector_channel(state,key);
            if(std::string_view(key)=="truck.local.velocity.angular") for(auto& v:value) v*=2*std::numbers::pi;
            c.serialize_array(value.data(),3);
        }
    }));
    add_message(packet,"/ets2/vehicle/actuation",cdr(256,[&](Cdr& c){
        header(c,us,"base_link");c<<state.at("frame_id").get<uint64_t>();
        for(const auto* key:{"truck.input.steering","truck.input.throttle","truck.input.brake",
                            "truck.effective.steering","truck.effective.throttle","truck.effective.brake"}) c<<channel(state,key);
        for(const auto* key:{"truck.engine.gear","truck.displayed.gear"}) {
            const auto& sdk=state.at("sdk");const bool available=sdk.contains(key) && sdk.at(key).value("available",false);
            c<<available<<(available?sdk.at(key).at("value").get<int32_t>():int32_t{0});
        }
    }));
    if(state.contains("drive")) {
        const auto& drive=state.at("drive");packet.meta["drive_state"]=drive;
        add_message(packet,"/ets2/drive/state",cdr(2048,[&](Cdr& c){
            header(c,us,"base_link");c<<state.at("frame_id").get<uint64_t>();
            for(const auto* key:{"available","permitted","profile_supported","armed","active"}) c<<drive.at(key).get<bool>();
            c<<drive.at("reason").get<std::string>()<<drive.at("owner").get<std::string>();
            for(const auto* key:{"epoch","sequence","deadline_ms","command_window_ms"}) c<<drive.at(key).get<uint64_t>();
            for(const auto* key:{"steering","throttle","brake","manual_steering","manual_throttle","manual_brake"}) c<<drive.at(key).get<float>();
            for(const auto* key:{"truck.effective.steering","truck.effective.throttle","truck.effective.brake"}) c<<channel(state,key);
            c<<drive.at("error").get<std::string>();
        }));
    }
    if(state.contains("engine") && state["engine"].contains("vehicle") && state["engine"]["vehicle"].value("available",false)) {
        const auto& p=state["engine"]["vehicle"]["pose_physics"];
        const M rotation=from_quat(p.at("quaternion_wxyz").get<Q>());
        const auto world=mul(enu,add(p.at("position_m").get<V>(),mul(rotation,base)));
        const auto q=quaternion(mul(mul(enu,rotation),base_to_model));
        add_message(packet,"/ets2/ground_truth/ego/pose",cdr(256,[&](Cdr& c){header(c,us,"world");pose(c,world,q);}));
    }
    if(state.contains("engine") && state["engine"].contains("traffic")) {
        const auto& traffic=state["engine"]["traffic"];
        std::vector<uint64_t> ids;std::array<std::vector<double>,6> values;
        if(traffic.at("available").get<bool>()) for(const auto& actor:traffic.at("vehicles")) {
            ids.push_back(actor.at("id").get<uint64_t>());int i=0;
            for(const auto* key:{"x","y","yaw","speed_mps","length_m","width_m"}) values[i++].push_back(actor.at(key).get<double>());
        }
        add_message(packet,"/ets2/ground_truth/traffic",cdr(256+ids.size()*56,[&](Cdr& c){
            header(c,us,"world");c<<state.at("frame_id").get<uint64_t>()<<traffic.at("available").get<bool>()<<traffic.value("error",std::string{});
            c<<ids;for(const auto& axis:values) c<<axis;
        }));
    }
    return packet;
}
}
