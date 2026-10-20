#pragma once
// Kinematic bicycle reference of a multi-axle truck. Front: the steering axle. Rear: the
// grounded non-steering axles, a grounded lift axle weighted kLiftAxleWeight. The weight
// is measured (docs/29, 2026-10-20): on the Scania S 6x2 with the tag axle down, the yaw
// response matched an effective wheelbase of 3.68 m against 3.805 m to the middle of the
// drive and tag axles, i.e. the rear point sits 40 % of the way from drive to tag axle.
// With the tag axle raised the yaw response matched the drive axle (3.158 m).
#include <cmath>
#include <cstdint>
#include <map>
#include <stdexcept>

struct AxleWheel {double x;bool steerable,liftable;};   // x forward of base_link, m

struct KinematicAxles {
    double front_x,rear_x;   // forward of base_link, m
    double wheelbase() const {return front_x-rear_x;}
};

constexpr double kLiftAxleWeight=.675;

// grounded: wheel index -> touches the ground. A liftable wheel without a state counts
// as raised; a fixed axle counts as grounded.
inline KinematicAxles kinematic_axles(const std::map<uint32_t,AxleWheel>& wheels,const std::map<uint32_t,bool>& grounded) {
    double front=-INFINITY;
    std::map<long long,std::pair<double,double>> axles;   // mm key -> (x, weight)
    for(const auto& [index,w]:wheels) {
        if(w.steerable && !w.liftable) {front=std::max(front,w.x);continue;}
        if(w.steerable) continue;
        const auto state=grounded.find(index);
        const bool down=w.liftable?(state!=grounded.end() && state->second):true;
        if(!down) continue;
        auto& axle=axles[std::llround(w.x*1000)];
        axle.first=w.x;axle.second=std::max(axle.second,w.liftable?kLiftAxleWeight:1.0);
    }
    double sum=0,weight=0;
    for(const auto& [key,axle]:axles) {sum+=axle.first*axle.second;weight+=axle.second;}
    if(!std::isfinite(front) || weight<=0) throw std::runtime_error("Truck axle geometry is unavailable");
    return {front,sum/weight};
}
