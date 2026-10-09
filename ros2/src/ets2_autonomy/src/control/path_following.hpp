#pragma once
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <vector>

struct LanePath {
    struct Point {double x,y,s;};
    struct Projection {double s,d;size_t segment;};
    std::vector<Point> points;
    double width=0,progress=0;
    explicit LanePath(const std::string& file) {
        std::ifstream input(file);nlohmann::json data;input>>data;
        if(data.at("frame_id")!="world") throw std::runtime_error("Lane path must use ROS world coordinates");
        width=data.at("lane_width_m").get<double>();
        if(!std::isfinite(width) || width<=0) throw std::runtime_error("Invalid lane width");
        for(const auto& p:data.at("points")) {
            const auto x=p.at(0).get<double>(),y=p.at(1).get<double>();
            if(!std::isfinite(x) || !std::isfinite(y)) throw std::runtime_error("Nonfinite lane point");
            const auto distance=points.empty()?0:std::hypot(x-points.back().x,y-points.back().y);
            if(!points.empty() && distance==0) continue;
            points.push_back({x,y,points.empty()?0:points.back().s+distance});
        }
        if(points.size()<2) throw std::runtime_error("Lane path needs two distinct points");
    }
    Projection project(double x,double y,double from=0) const {
        Projection best{0,std::numeric_limits<double>::infinity(),0};
        double nearest=std::numeric_limits<double>::infinity();
        for(size_t i=0;i+1<points.size();++i) {
            const auto& a=points[i];const auto& b=points[i+1];
            if(b.s<from) continue;
            const auto dx=b.x-a.x,dy=b.y-a.y,length=b.s-a.s;
            const auto t=std::clamp(((x-a.x)*dx+(y-a.y)*dy)/(length*length),0.0,1.0);
            const auto distance=std::hypot(x-a.x-t*dx,y-a.y-t*dy);
            if(distance<nearest) {
                nearest=distance;
                best={a.s+t*length,std::abs((x-a.x)*dy-(y-a.y)*dx)/length,i};
            }
        }
        return best;
    }
    Point at(double s) const {
        s=std::clamp(s,0.0,points.back().s);
        const auto upper=std::upper_bound(points.begin(),points.end(),s,[](double s,const Point& p){return s<p.s;});
        if(upper==points.end()) return points.back();
        const auto& a=*(upper-1);const auto& b=*upper;const auto t=(s-a.s)/(b.s-a.s);
        return {a.x+t*(b.x-a.x),a.y+t*(b.y-a.y),s};
    }
    struct Target {double curvature,remaining,error;};
    Target follow(double x,double y,double yaw,double lookahead) {
        const auto p=project(x,y,progress);progress=std::max(progress,p.s);
        const auto target=at(progress+lookahead);const auto dx=target.x-x,dy=target.y-y;
        const auto distance2=dx*dx+dy*dy;
        const auto lateral=-std::sin(yaw)*dx+std::cos(yaw)*dy;
        return {distance2>0?2*lateral/distance2:0,points.back().s-progress,p.d};
    }
};
