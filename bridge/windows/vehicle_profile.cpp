#include "vehicle_profile.hpp"
#include <map>
#include <set>

namespace bridge {
VehicleProfile resolve_vehicle(json rig,const json& truck,const json& selected) {
    auto layout=ot::resolve_mount_layout(std::move(rig),truck,selected);
    rig=std::move(layout.rig);
    const auto& origin=layout.origin;
    const auto& wheels=layout.reference_wheels;
    std::map<std::pair<std::string,int>,json> attributes;
    for(const auto& a:truck.at("attributes")) attributes[{a.at("name"),a.at("index").is_null()?-1:a.at("index").get<int>()}]=a.at("value");
    const bool mounted_sensors=!selected.empty();
    std::set<std::string> camera_ids;
    for(const auto& v:rig.at("views")) {
        const auto name=v.at("camera_id").get<std::string>();
        if((name!="C_FN" && name!="C_FW" && name!="C_RL" && name!="C_RR") || !camera_ids.insert(name).second)
            throw std::runtime_error("ROS camera_id must be unique and one of C_FN, C_FW, C_RL, C_RR");
    }
    rig["enabled"]=!rig.at("views").empty();rig["cmd"]="camera_rig";
    VehicleProfile result{rig,{{"truck_id",attributes.at({"id",-1})},{"base_origin_model_m",origin},
        {"base_link_wheels",wheels},{"mount_profile",mounted_sensors?rig.value("vehicle_configuration",json(nullptr)):json(nullptr)},
        {"wheels",json::array()}},origin,{},truck.at("truck_generation").get<uint64_t>()};
    for(const auto& [index,wheel]:layout.wheels) {
        const auto position=mul(transpose(base_to_model),sub(wheel.position,origin));
        result.wheels.push_back({static_cast<uint32_t>(index),position,wheel.radius,wheel.simulated});
        json info={{"index",index},{"position_base_m",position},{"radius_m",wheel.radius}};
        for(const auto* field:{"steerable","simulated","powered","liftable"}) {
            const auto found=attributes.find({std::string("wheel.")+field,index});
            info[field]=found==attributes.end()?json(nullptr):found->second;
        }
        result.configuration["wheels"].push_back(std::move(info));
    }
    return result;
}
json lidar_patterns(const json& rig,const json& profile) {
    std::map<std::string,json> views;
    for(const auto& view:rig.at("views")) views["mirror"+std::to_string(view.at("slot").get<int>())]=view;
    json patterns=json::object();
    for(const auto& sensor:profile.at("sensors")) {
        const auto axis=sensor.at("axis_camera").get<std::string>();
        bool complete=views.contains(axis);
        for(const auto& name:sensor.at("sources")) complete&=views.contains(name.get<std::string>());
        if(!complete) continue;
        const auto& a=views.at(axis);const auto axis_rotation=from_quat(a.at("quaternion_wxyz").get<Q>());
        for(const auto& name:sensor.at("sources")) {
            const auto& source=views.at(name.get<std::string>());
            const auto delta=sub(source.at("position").get<V>(),a.at("position").get<V>());
            if(source.at("basis")!=a.at("basis") || std::hypot(delta[0],delta[1],delta[2])>1e-4)
                throw std::runtime_error("LiDAR source mounts must share an optical origin and parent");
            auto spec=sensor;
            spec["camera_from_sensor"]=mul(mul(transpose(from_quat(source.at("quaternion_wxyz").get<Q>())),axis_rotation),base_to_model);
            patterns[name.get<std::string>()]=std::move(spec);
        }
    }
    return patterns;
}
}
