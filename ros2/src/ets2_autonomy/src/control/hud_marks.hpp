#pragma once
// HUD marks (hud_node.cpp): what the decal and Foxglove show for the assistance state, as
// strokes. One element per thing the driver needs to know (docs/24 §0):
// - LCC steering: a thin ribbon on the path it steers (from drive_speed's /ets2/drive/plan).
// - ACC without a lead: two short brackets at the lane edges at the gap it keeps ("on, this
//   far"); with a lead: one bar under its rear bumper. The brackets slide to the lead and
//   close into the bar (and open again) over a quarter second.
// - Colour only for the deceleration: neutral, amber while ACC brakes noticeably, red when it
//   brakes hard; the marks fade while the driver's throttle overrides ACC.
// - A turn signal lane change waiting for a gap: a dashed line on the target lane.
// Off: nothing.
#include "hud_path.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

namespace hud {
struct Stroke {
    std::string kind;              // lcc_path, acc, lane_change: the marker namespace road/<kind>
    Points points;
    double width_m=.2,screen_px=0; // width on the road; minimum width on screen (pixels at 1080 lines)
    std::array<float,4> rgba{1,1,1,1};
};
struct Inputs {
    bool lcc_steering=false,acc=false,driver_throttle=false;
    uint8_t mark=0;                 // AssistState.acc_mark: 0 none, 1 gap, 2 lead
    double mark_x=0,mark_y=0,mark_heading=0,lane_width=0;
    bool emergency=false;
    double accel_target=0;          // m/s^2
    int change_request=0;           // -1 left, 1 right: a lane change waiting for a gap
    Points plan;                    // LCC's path, world
    double ego_x=0,ego_y=0,ego_yaw=0;
};
inline constexpr std::array<float,3> neutral{.92f,.92f,.95f},amber{1,.62f,.15f},red{1,.25f,.15f};

class Composer {
public:
    // dt: seconds since the last call.
    std::vector<Stroke> update(const Inputs& in,double dt) {
        std::vector<Stroke> strokes;
        if(in.lcc_steering && in.plan.size()>=2) strokes.push_back({"lcc_path",in.plan,.2,3,rgba(neutral,.5f,in)});
        if(in.acc && in.mark) acc_marks(in,dt,strokes);
        else {shown_.reset();join_=0;}
        if(in.change_request && in.plan.size()>=2 && in.lane_width>0) waiting(in,strokes);
        return strokes;
    }
    double join() const {return join_;}
private:
    static constexpr double join_s=.25,follow_s=.08,dash_m=3;
    std::optional<std::array<double,3>> shown_; // mark in the truck frame: ahead, left, heading
    double join_=0;
    static std::array<float,4> rgba(std::array<float,3> c,float a,const Inputs& in) {
        return {c[0],c[1],c[2],in.driver_throttle?a*.45f:a};
    }
    void acc_marks(const Inputs& in,double dt,std::vector<Stroke>& strokes) {
        // The mark in the truck frame, eased there (no lag once it stops moving relative to the truck).
        const double c=std::cos(in.ego_yaw),s=std::sin(in.ego_yaw),dx=in.mark_x-in.ego_x,dy=in.mark_y-in.ego_y;
        const std::array<double,3> target{c*dx+s*dy,-s*dx+c*dy,std::remainder(in.mark_heading-in.ego_yaw,2*std::numbers::pi)};
        if(!shown_) shown_=target;
        else {
            const auto k=std::clamp(dt/follow_s,0.0,1.0);
            for(int i=0;i<3;++i) (*shown_)[i]+=k*(i==2?std::remainder(target[i]-(*shown_)[i],2*std::numbers::pi):target[i]-(*shown_)[i]);
        }
        join_=std::clamp(join_+(in.mark==2?1:-1)*dt/join_s,0.0,1.0);
        const auto [ahead,left,heading]=*shown_;
        const double x=in.ego_x+c*ahead-s*left,y=in.ego_y+s*ahead+c*left,h=in.ego_yaw+heading;
        const double half=in.lane_width>0?std::min(in.lane_width/2-.25,bar_half_m):bar_half_m;
        const double gap=(1-join_)*(half-.6);   // half the opening in the middle
        const auto colour=in.emergency || in.accel_target<-2.5?red:in.accel_target<-1?amber:neutral;
        const float alpha=float(.35+.4*join_);
        const double width=.35+.05*join_,px=6+2*join_;
        const auto across=[&](double from,double to) {
            return Points{{x-std::sin(h)*from,y+std::cos(h)*from},{x-std::sin(h)*to,y+std::cos(h)*to}};
        };
        if(gap<.05) strokes.push_back({"acc",across(-half,half),width,px,rgba(colour,alpha,in)});
        else {
            strokes.push_back({"acc",across(-half,-gap),width,px,rgba(colour,alpha,in)});
            strokes.push_back({"acc",across(gap,half),width,px,rgba(colour,alpha,in)});
        }
    }
    static void waiting(const Inputs& in,std::vector<Stroke>& strokes) {
        // Dashes 8-26 m along the plan, one lane width over (left positive in ROS).
        const double offset=-in.change_request*in.lane_width;
        Points dash;double s=0;
        for(size_t i=1;i<in.plan.size() && strokes.size()<8;++i) {
            const auto [ax,ay]=in.plan[i-1];const auto [bx,by]=in.plan[i];
            const double l=std::hypot(bx-ax,by-ay);
            if(!(l>1e-6)) continue;
            const double nx=-(by-ay)/l,ny=(bx-ax)/l;
            for(double t=0;t<l;t+=.5) {
                const double at=s+t;
                const bool on=at>=8 && at<=26 && std::fmod(at-8,2*dash_m)<dash_m;
                if(on) dash.push_back({ax+(bx-ax)*t/l+nx*offset,ay+(by-ay)*t/l+ny*offset});
                else if(dash.size()>=2) {strokes.push_back({"lane_change",dash,.2,3,rgba(amber,.5f,in)});dash.clear();}
                else dash.clear();
            }
            s+=l;
        }
        if(dash.size()>=2 && strokes.size()<8) strokes.push_back({"lane_change",dash,.2,3,rgba(amber,.5f,in)});
    }
};
}
