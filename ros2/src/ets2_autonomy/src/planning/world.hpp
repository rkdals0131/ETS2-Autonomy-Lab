#pragma once
// The planner's view of the world (docs: plan stage 2). Filled from ground truth today
// (planner_node.cpp's GT adapter); perception fills the same struct later, so the behavior
// tree and the maneuver state machine never see where it came from. No ROS types here.
#include "control/path_following.hpp"
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace planning {

struct Object {uint64_t id;double x,y,yaw,speed,length,width,z=NAN,height=NAN;};

// A route event on the followed lane (map_lane_provider --route) at provider arc length s:
// the walker could not reach the route's exit from this lane; lanes_ok are the entry lanes
// that can, ranked from the left, lane the current one.
struct RouteEvent {double s;std::vector<int> lanes_ok;int lane;};

struct World {
    double t=0;                       // s, steady
    // Ego (ROS world) and the assistance drive_speed runs.
    double x=0,y=0,z=0,yaw=0,speed=0,grade=0;
    bool acc=false,lcc=false;
    double set_speed=0;
    bool lead=false;double lead_gap=0,lead_speed=0;uint64_t lead_id=0;
    // The driver's turn signal switch: -1 left, 1 right, 0 off (both on: hazards, 0).
    int signal=0;
    // drive_speed's lane change: requested side (waiting for a gap), side in progress.
    int change_request=0,change_active=0;
    std::vector<Object> objects;
    // The followed lane and the same-direction lanes beside it (provider files), and the
    // truck's arc length on the provider's path (the events' s).
    std::optional<LanePath> lane;std::optional<LanePath> left,right;
    double path_s=0;
    std::vector<RouteEvent> route;
    const std::optional<LanePath>& side(int s) const {return s<0?left:right;}
};

// Time to collision with the lead (s), infinite when not closing.
inline double lead_ttc(const World& w) {
    if(!w.lead) return INFINITY;
    const double closing=w.speed-w.lead_speed;
    return closing>.1?w.lead_gap/closing:INFINITY;
}

// Why a lane change to the target lane cannot start now ("" when the gap is free): a vehicle
// in that lane too close ahead (5 m + 0.5 s + 3 s of closing speed), behind (5 m + 3 s of its
// closing speed) or beside. The same rule as drive_speed's gap check (gt_drive_node.cpp), so
// the shadow planner and the executor agree; front/rear: bumper offsets from the projection.
inline std::string gap_problem(const World& w,const LanePath& target,double front=6,double rear=15) {
    const auto me=target.project(w.x,w.y);
    for(const auto& a:w.objects) {
        const auto p=target.project(a.x,a.y,std::max(0.0,me.s-200),me.s+200);
        if(!(p.d<=(target.width+a.width)/2)) continue;
        const auto& A=target.points[p.segment];const auto& B=target.points[p.segment+1];
        if(std::cos(a.yaw-std::atan2(B.y-A.y,B.x-A.x))<=0) continue;
        // Another level (an overpass): not in this lane.
        if(std::isfinite(a.z) && std::isfinite(a.height) && std::abs(a.z-a.height/2-(w.z+std::tan(w.grade)*(p.s-me.s)))>4) continue;
        const double ahead=p.s-a.length/2-(me.s+front),behind=(me.s-rear)-(p.s+a.length/2),v=std::max(0.0,w.speed);
        if(ahead>=0) {if(ahead<5+.5*v+3*std::max(0.0,v-a.speed)) return "vehicle ahead";}
        else if(behind>=0) {if(behind<5+3*std::max(0.0,a.speed-v)) return "vehicle behind";}
        else return "vehicle beside";
    }
    return "";
}

// The side to move to for the nearest route event within range: towards the nearest lane
// that reaches the route's exit (lanes ranked from the left), 0 when none or already there.
inline int route_side(const World& w,double range=1500) {
    for(const auto& e:w.route) {
        const double ahead=e.s-w.path_s;
        if(ahead<0 || ahead>range || e.lanes_ok.empty()) continue;
        int best=e.lanes_ok.front();
        for(int k:e.lanes_ok) if(std::abs(k-e.lane)<std::abs(best-e.lane)) best=k;
        return best>e.lane?1:best<e.lane?-1:0;
    }
    return 0;
}

// A lead slower than the set speed (by margin) within range, continuously for hold seconds:
// worth passing. Feed it every tick.
struct SlowLead {
    double margin_mps=10/3.6,range_m=100,hold_s=8;
    double since=NAN;
    bool update(const World& w) {
        const bool slow=w.acc && w.lead && w.lead_gap<range_m && w.lead_speed<w.set_speed-margin_mps;
        if(!slow) {since=NAN;return false;}
        if(std::isnan(since)) since=w.t;
        return w.t-since>=hold_s;
    }
};

} // namespace planning
