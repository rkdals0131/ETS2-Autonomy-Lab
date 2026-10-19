#pragma once
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <numbers>
#include <vector>

struct LanePath {
    struct Point {double x,y,s;};
    // d: distance to the polyline; offset: the same distance signed, left of the path positive.
    struct Projection {double s,d;size_t segment;double offset;};
    std::vector<Point> points;
    std::vector<double> headings; // per segment, unwrapped
    double width=0,progress=0;
    bool tracked=false;
    // Ego search span past progress. The ego moves <1 m per 20 ms cycle; a
    // nearest point at the span end or off the lane falls back to the full scan.
    static constexpr double track_window_m=100;
    explicit LanePath(const std::string& file) {
        std::ifstream input(file);nlohmann::json data;input>>data;
        if(data.at("frame_id")!="world") throw std::runtime_error("Lane path must use ROS world coordinates");
        width=data.at("lane_width_m").get<double>();
        if(!std::isfinite(width) || width<=0) throw std::runtime_error("Invalid lane width");
        std::vector<std::array<double,2>> xy;
        for(const auto& p:data.at("points")) xy.push_back({p.at(0).get<double>(),p.at(1).get<double>()});
        load(xy);
    }
    LanePath(const std::vector<std::array<double,2>>& xy,double lane_width):width(lane_width) {
        if(!std::isfinite(width) || width<=0) throw std::runtime_error("Invalid lane width");
        load(xy);
    }
    Projection project(double x,double y,double from=0,double to=std::numeric_limits<double>::infinity()) const {
        Projection best{0,std::numeric_limits<double>::infinity(),0,0};
        // First segment whose end is at or past from, as the full scan's b.s<from skip.
        const auto first=std::lower_bound(points.begin(),points.end(),from,[](const Point& p,double s){return p.s<s;});
        for(size_t i=first==points.begin()?0:size_t(first-points.begin())-1;i+1<points.size() && points[i].s<=to;++i) {
            const auto& a=points[i];const auto& b=points[i+1];
            const auto dx=b.x-a.x,dy=b.y-a.y,length=b.s-a.s;
            const auto t=std::clamp(((x-a.x)*dx+(y-a.y)*dy)/(length*length),0.0,1.0);
            const auto distance=std::hypot(x-a.x-t*dx,y-a.y-t*dy);
            if(distance<best.d) {
                // Distance to the path itself, not to the segment's infinite line,
                // so points beyond either end are not reported as on-lane.
                best={a.s+t*length,distance,i,std::copysign(distance,dx*(y-a.y)-dy*(x-a.x))};
            }
        }
        return best;
    }
    // Ego projection: monotonic progress, searched in a window ahead of it.
    Projection track(double x,double y) {
        auto p=project(x,y,progress,tracked?progress+track_window_m:std::numeric_limits<double>::infinity());
        if(tracked && (p.d>width || p.s>=progress+track_window_m)) p=project(x,y,progress);
        tracked=true;progress=std::max(progress,p.s);
        return p;
    }
    Point at(double s) const {
        // Continue the last segment past the end. A lookahead clamped to the end
        // point would shrink toward the vehicle and saturate steering there.
        s=std::max(s,0.0);
        const auto upper=std::upper_bound(points.begin(),points.end(),s,[](double s,const Point& p){return s<p.s;});
        const auto last=upper==points.end()?points.end()-1:upper;
        const auto& a=*(last-1);const auto& b=*last;const auto t=(s-a.s)/(b.s-a.s);
        return {a.x+t*(b.x-a.x),a.y+t*(b.y-a.y),s};
    }
    // Tangent heading (unwrapped): segment headings at segment midpoints,
    // interpolated linearly, so an arc's polyline yields its exact tangent and
    // the 1.5 m vertex staircase does not ripple the curvature.
    double heading(double s) const {
        const auto mid=[&](size_t i){return (points[i].s+points[i+1].s)/2;};
        const auto n=headings.size();
        if(s<=mid(0)) return headings.front();
        if(s>=mid(n-1)) return headings.back();
        const auto upper=std::upper_bound(points.begin()+1,points.end()-1,s,[](double s,const Point& p){return s<p.s;});
        auto i=size_t(upper-points.begin())-1; // segment containing s
        if(s<mid(i)) --i;
        const auto t=(s-mid(i))/(mid(i+1)-mid(i));
        return headings[i]+t*(headings[i+1]-headings[i]);
    }
    // Mean curvature over [s-w/2, s+w/2]; zero once past the end.
    double curvature(double s,double w) const {return (heading(s+w/2)-heading(s-w/2))/w;}
    double remaining() const {return points.back().s-progress;}
    // Pure pursuit curvature toward the lane point lookahead past progress.
    double pursuit(double x,double y,double yaw,double lookahead) const {
        const auto target=at(progress+lookahead);const auto dx=target.x-x,dy=target.y-y;
        const auto distance2=dx*dx+dy*dy;
        const auto lateral=-std::sin(yaw)*dx+std::cos(yaw)*dy;
        return distance2>0?2*lateral/distance2:0;
    }
    struct Target {double curvature,remaining,error;};
    Target follow(double x,double y,double yaw,double lookahead) {
        const auto p=track(x,y);
        return {pursuit(x,y,yaw,lookahead),remaining(),p.d};
    }
private:
    void load(const std::vector<std::array<double,2>>& xy) {
        for(const auto& [x,y]:xy) {
            if(!std::isfinite(x) || !std::isfinite(y)) throw std::runtime_error("Nonfinite lane point");
            const auto distance=points.empty()?0:std::hypot(x-points.back().x,y-points.back().y);
            if(!points.empty() && distance==0) continue;
            points.push_back({x,y,points.empty()?0:points.back().s+distance});
        }
        if(points.size()<2) throw std::runtime_error("Lane path needs two distinct points");
        for(size_t i=0;i+1<points.size();++i) {
            const auto h=std::atan2(points[i+1].y-points[i].y,points[i+1].x-points[i].x);
            headings.push_back(headings.empty()?h:headings.back()+std::remainder(h-headings.back(),2*std::numbers::pi));
        }
    }
};
