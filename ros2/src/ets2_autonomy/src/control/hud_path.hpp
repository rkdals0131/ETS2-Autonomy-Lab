#pragma once
// HUD decal content for /ets2/overlay/path (world frame, ROS x/y): the assistance's path
// ahead as a thin ribbon. With LCC it is the lane centreline from the rear point; without,
// the arc the truck drives at its current yaw rate. A followed lead ends the ribbon with a
// cross bar under its rear bumper (the relay's mask paints distance to the polyline, so a
// bar drawn out and back reads as a T).
#include "path_following.hpp"
#include <array>
#include <cmath>
#include <vector>

namespace hud {
using Points=std::vector<std::array<double,2>>;
inline constexpr double step_m=2,bar_half_m=1.6;

inline Points lane(const LanePath& path,double from_s,double length) {
    Points points;
    const auto end=std::min(from_s+length,path.points.back().s);
    for(double s=from_s;s<end;s+=step_m) {const auto p=path.at(s);points.push_back({p.x,p.y});}
    if(end>from_s) {const auto p=path.at(end);points.push_back({p.x,p.y});}
    return points;
}
inline Points arc(double x,double y,double yaw,double curvature,double length) {
    Points points{{x,y}};
    for(double s=step_m;s<length+step_m/2;s+=step_m) {
        const auto t=std::min(s,length),h=yaw+curvature*t;
        if(std::abs(curvature)<1e-6) points.push_back({x+t*std::cos(yaw),y+t*std::sin(yaw)});
        else points.push_back({x+(std::sin(h)-std::sin(yaw))/curvature,y-(std::cos(h)-std::cos(yaw))/curvature});
    }
    return points;
}
// Cross bar at the last point, across the last segment; the polyline returns to the centre.
inline void bar(Points& points) {
    if(points.size()<2) return;
    const auto [x,y]=points.back();const auto [px,py]=points[points.size()-2];
    const auto n=std::hypot(x-px,y-py);
    if(!(n>1e-6)) return;
    const auto nx=-(y-py)/n*bar_half_m,ny=(x-px)/n*bar_half_m;
    points.push_back({x+nx,y+ny});points.push_back({x-nx,y-ny});points.push_back({x,y});
}
} // namespace hud
