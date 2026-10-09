#pragma once
#include "geometry.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>
#include <vector>

namespace ot {
using MountJson=nlohmann::json;
class MountMismatch:public std::runtime_error {using std::runtime_error::runtime_error;};
struct MountWheel {Vec3 position;double radius;bool simulated;};
struct MountLayout {MountJson rig;Vec3 origin{};std::vector<int> reference_wheels;std::map<int,MountWheel> wheels;};
inline MountLayout resolve_mount_layout(MountJson rig,const MountJson& truck,const MountJson& selected) {
    std::map<std::pair<std::string,int>,MountJson> attributes;
    for(const auto& a:truck.at("attributes")) attributes[{a.at("name"),a.at("index").is_null()?-1:a.at("index").get<int>()}]=a.at("value");
    std::map<int,MountWheel> geometry;
    for(const auto& [key,value]:attributes) if(key.first=="wheel.position") {
        const auto index=key.second;
        const auto p=value.get<Vec3>();const auto radius=attributes.at({"wheel.radius",index}).get<double>();
        if(!std::isfinite(radius) || radius<=0) throw std::runtime_error("Invalid SDK wheel geometry");
        for(auto coordinate:p) if(!std::isfinite(coordinate)) throw std::runtime_error("Invalid SDK wheel geometry");
        const bool simulated=!attributes.contains({"wheel.simulated",index}) || attributes.at({"wheel.simulated",index}).get<bool>();
        geometry.emplace(index,MountWheel{p,radius,simulated});
    }
    const bool mounted=!selected.empty();
    if(mounted && rig.contains("truck_id") && attributes.at({"id",-1})!=rig.at("truck_id"))
        throw MountMismatch("Truck differs from calibrated sensor mount: current "+attributes.at({"id",-1}).get<std::string>()+"; preset "+rig.at("truck_id").get<std::string>());
    auto wheels=mounted?rig.value("base_link_wheels",std::vector<int>{}):std::vector<int>{};
    if(wheels.empty()) for(const auto& [key,value]:attributes)
        if(key.first=="wheel.powered" && value.get<bool>() && (!attributes.contains({"wheel.simulated",key.second}) || attributes.at({"wheel.simulated",key.second}).get<bool>())) wheels.push_back(key.second);
    if(wheels.empty()) throw std::runtime_error("No SDK wheel reference for base_link");
    const double axle=geometry.at(wheels.front()).position[2];
    Vec3 origin{};
    for(int index:wheels) {
        auto p=geometry.at(index).position;const auto radius=geometry.at(index).radius;
        if(std::abs(p[2]-axle)>.001) throw std::runtime_error("Select one base_link axle in the mounting preset");
        p[1]-=radius;for(int i=0;i<3;++i) origin[i]+=p[i]/wheels.size();
    }
    MountJson views=MountJson::array();
    for(auto view:rig.at("views")) if(std::find(selected.begin(),selected.end(),view.at("slot"))!=selected.end()) {
        if(view.contains("position_base_link")) {
            const auto p=view.at("position_base_link").get<Vec3>();
            view["position"]={origin[0]-p[1],origin[1]+p[2],origin[2]-p[0]};view.erase("position_base_link");
            const auto basis=view.value("basis",std::string("chassis"));
            if(basis!="chassis" && basis!="cabin") throw std::runtime_error("position_base_link requires chassis or cabin attachment");
            view["basis"]=basis;
        }
        views.push_back(std::move(view));
    }
    rig["views"]=std::move(views);rig["base_origin"]=origin;
    rig["mount_calibration"]={{"sdk_truck_id",attributes.at({"id",-1})},{"reference_wheel_indices",wheels},
        {"base_link_origin_in_chassis",origin},{"method","SDK nominal wheel centers minus radii; fixed body reference, not measured road contact"},
        {"base_link_axes","x forward, y left, z up"}};
    return {std::move(rig),origin,std::move(wheels),std::move(geometry)};
}
}
