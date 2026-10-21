#pragma once
// The provider's map around the truck (<path_file>.scene, map_lane_provider Scene) for
// Foxglove's scene/markings, scene/junctions and scene/signals: LINE_LIST point pairs per
// kind and the traffic lights, ROS world coordinates. The file covers a wide radius and is
// rewritten every few metres; the hud draws the part within a smaller radius of the truck
// each frame, so the view stays whole around the truck between rewrites.
#include <nlohmann/json.hpp>
#include <array>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace hud {
using Point3=std::array<double,3>;
struct Light {Point3 position;double height;std::optional<double> heading;};
struct Scene {std::vector<Point3> lane,centre,edge,junction;std::vector<Light> lights;double x=0,y=0,radius=0;bool valid=false;};

inline Scene read_scene(const nlohmann::json& data) {
    Scene out;
    if(data.at("frame_id")!="world") throw std::runtime_error("Scene must use ROS world coordinates");
    out.x=data.at("center").at(0).get<double>();out.y=data.at("center").at(1).get<double>();out.radius=data.at("radius_m").get<double>();
    const auto point=[](double x,double y,double z) {return Point3{x,y,z+.05};};
    for(const auto& line:data.at("lines")) {
        std::vector<Point3> q;
        for(const auto& p:line.at("points")) q.push_back({p.at(0).get<double>(),p.at(1).get<double>(),p.at(2).get<double>()});
        if(q.size()<2) continue;
        const std::string kind=line.at("kind");
        if(kind=="lane") {
            // 3 m dashes and gaps from the line's arc length on the road (s0), so they stay put as the file rolls.
            double s=line.value("s0",0.0);
            for(size_t i=0;i+1<q.size();++i) {
                const auto& a=q[i];const auto& b=q[i+1];
                const double l=std::hypot(b[0]-a[0],b[1]-a[1]);
                for(double u=0;u<l;) {
                    const double phase=std::fmod(s+u,6.0),step=std::max(1e-3,std::min(l-u,phase<3?3-phase:6-phase));
                    if(phase<3) for(const double v:{u,u+step}) out.lane.push_back(point(a[0]+(b[0]-a[0])*v/l,a[1]+(b[1]-a[1])*v/l,a[2]+(b[2]-a[2])*v/l));
                    u+=step;
                }
                s+=l;
            }
            continue;
        }
        // The divider between the directions is a double line 0.25 m apart.
        for(const double offset:kind=="centre"?std::vector<double>{-.125,.125}:std::vector<double>{0})
            for(size_t i=0;i+1<q.size();++i) for(const auto k:{i,i+1}) {
                const double dx=q[i+1][0]-q[i][0],dy=q[i+1][1]-q[i][1],l=std::max(1e-6,std::hypot(dx,dy));
                (kind=="edge"?out.edge:kind=="centre"?out.centre:out.junction).push_back(point(q[k][0]-offset*dy/l,q[k][1]+offset*dx/l,q[k][2]));
            }
    }
    for(const auto& light:data.at("signals")) {
        const auto& p=light.at("position");
        out.lights.push_back({{p.at(0).get<double>(),p.at(1).get<double>(),p.at(2).get<double>()},light.value("height_m",0.0),
                              light.contains("heading") && light["heading"].is_number()?std::optional<double>(light["heading"].get<double>()):std::nullopt});
    }
    out.valid=true;
    return out;
}

// The point pairs with an end within radius of (x, y), ground plane.
inline std::vector<Point3> within(const std::vector<Point3>& pairs,double x,double y,double radius) {
    std::vector<Point3> out;
    const double r2=radius*radius;
    const auto in=[&](const Point3& p) {const double dx=p[0]-x,dy=p[1]-y;return dx*dx+dy*dy<=r2;};
    for(size_t i=0;i+1<pairs.size();i+=2)
        if(in(pairs[i]) || in(pairs[i+1])) {out.push_back(pairs[i]);out.push_back(pairs[i+1]);}
    return out;
}
} // namespace hud
