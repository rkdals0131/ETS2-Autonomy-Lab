#include "vehicle_profile.hpp"
#include <map>
#include <set>

namespace bridge {
VehicleProfile resolve_vehicle(json rig,const json& truck,const json& selected) {
    std::map<std::pair<std::string,int>,json> attributes;
    for(const auto& a:truck.at("attributes")) attributes[{a.at("name"),a.at("index").is_null()?-1:a.at("index").get<int>()}]=a.at("value");
    const bool mounted_sensors=!selected.empty();
    if(mounted_sensors && attributes.at({"id",-1})!=rig.at("truck_id")) throw std::runtime_error("Truck differs from calibrated sensor mount");
    auto wheels=mounted_sensors?rig.value("base_link_wheels",std::vector<int>{}):std::vector<int>{};
    if(wheels.empty()) for(const auto& [key,value]:attributes)
        if(key.first=="wheel.powered" && value.get<bool>() &&
           (!attributes.contains({"wheel.simulated",key.second}) || attributes.at({"wheel.simulated",key.second}).get<bool>())) wheels.push_back(key.second);
    if(wheels.empty()) throw std::runtime_error("No SDK wheel reference for base_link");
    const double axle=attributes.at({"wheel.position",wheels.front()}).at(2);
    for(int index:wheels) if(std::abs(attributes.at({"wheel.position",index}).at(2).get<double>()-axle)>.001)
        throw std::runtime_error("Select one base_link axle in the mounting preset");
    std::array<double,3> origin{};
    for(int index:wheels) {auto p=attributes.at({"wheel.position",index}).get<std::array<double,3>>();p[1]-=attributes.at({"wheel.radius",index}).get<double>();for(int i=0;i<3;++i) origin[i]+=p[i]/wheels.size();}
    json views=json::array();
    const std::map<int,std::string> legacy_names{{0,"C_FN"},{1,"C_FW"},{2,"C_RL"},{5,"C_RR"}};
    std::set<std::string> camera_ids;
    for(auto v:rig.at("views")) if(std::find(selected.begin(),selected.end(),v.at("slot"))!=selected.end()) {
        const auto name=v.contains("camera_id")?v.at("camera_id").get<std::string>():legacy_names.at(v.at("slot").get<int>());
        if((name!="C_FN" && name!="C_FW" && name!="C_RL" && name!="C_RR") || !camera_ids.insert(name).second)
            throw std::runtime_error("ROS camera_id must be unique and one of C_FN, C_FW, C_RL, C_RR");
        v["camera_id"]=name;
        const auto p=v.at("position_base_link").get<std::array<double,3>>();v["position"]={origin[0]-p[1],origin[1]+p[2],origin[2]-p[0]};v.erase("position_base_link");views.push_back(v);
    }
    rig["views"]=views;rig["base_origin"]=origin;rig["enabled"]=!views.empty();rig["cmd"]="camera_rig";
    VehicleProfile result{rig,{{"truck_id",attributes.at({"id",-1})},{"base_origin_model_m",origin},
        {"base_link_wheels",wheels},{"mount_profile",mounted_sensors?rig.value("vehicle_configuration",json(nullptr)):json(nullptr)},
        {"wheels",json::array()}},origin,{},truck.at("truck_generation").get<uint64_t>()};
    for(const auto& [key,value]:attributes) if(key.first=="wheel.position") {
        const auto index=key.second;
        const auto radius=attributes.at({"wheel.radius",index}).get<double>();
        const auto position=mul(transpose(base_to_model),sub(value.get<V>(),origin));
        if(!finite(position) || !std::isfinite(radius) || radius<=0) throw std::runtime_error("Invalid SDK wheel geometry");
        const bool simulated=!attributes.contains({"wheel.simulated",index}) || attributes.at({"wheel.simulated",index}).get<bool>();
        result.wheels.push_back({static_cast<uint32_t>(index),position,radius,simulated});
        json info={{"index",index},{"position_base_m",position},{"radius_m",radius}};
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
