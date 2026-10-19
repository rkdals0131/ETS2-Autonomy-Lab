#pragma once
// Outer lateral loop: lane polyline -> front wheel angle target (rad) and its rate.
//
// curvature = path_curvature(s + v*preview) - k_y*offset - k_psi*sin(heading_error)
// k_y = 2/d^2, k_psi = 2/d, d = lookahead_m + lookahead_time_s*v
// wheel_angle = atan(wheelbase*curvature)
//
// The feedback is pure pursuit linearized about the lane: for a lane point d
// ahead, 2*lateral/d^2 = -2*offset/d^2 - 2*heading_error/d. In path distance the
// offset obeys e'' + (2/d) e' + (2/d^2) e = 0, damping 0.71 and natural length
// d/sqrt(2), so the baseline lookahead keeps its meaning. Pure pursuit reaches
// the lane curvature only through the chord to the lookahead point, which turns
// in early at a curve entry and lags the curvature ramp; here the curvature is
// an explicit feedforward, optionally previewed and differentiated
// along the lane for the inner loop's rate feedforward, and the feedback acts
// only on the errors. The pure pursuit curvature is still returned for the ACC
// curve speed limit so the speed behavior stays the baseline's.
#include "path_following.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>

struct LateralController {
    struct Settings {
        double lookahead_m=6,lookahead_time_s=.7;
        // Feedforward preview, s. The offline sim (game delay one frame, rate
        // feedforward in the inner loop) has its smallest error at 0 and grows
        // either side; kept to absorb a longer measured delay in the game.
        double curvature_preview_s=0;
        // Centered averaging span of the lane curvature: 4 m passes the map
        // lane's sampling noise into the steering, 8 m+ rounds curve entries.
        double curvature_window_m=6;
    };
    struct Output {
        double wheel_angle,wheel_rate;        // rad, rad/s
        double curvature,path_curvature;      // commanded, feedforward (1/m)
        double pursuit_curvature;             // baseline pure pursuit, for the curve speed limit
        double offset,heading_error,distance; // signed left-positive, rad, unsigned
        double remaining;
    };
    static Output update(LanePath& lane,double x,double y,double yaw,double speed,double wheelbase,const Settings& settings) {
        const auto p=lane.track(x,y);
        const auto v=std::max(0.0,speed),d=settings.lookahead_m+v*settings.lookahead_time_s;
        const auto w=settings.curvature_window_m,s=p.s+v*settings.curvature_preview_s;
        const auto heading_error=std::remainder(yaw-lane.heading(p.s),2*std::numbers::pi);
        const auto path_curvature=lane.curvature(s,w);
        const auto curvature=path_curvature-2*p.offset/(d*d)-2*std::sin(heading_error)/d;
        const auto curvature_rate=v*(lane.curvature(s+w/2,w)-lane.curvature(s-w/2,w))/w;
        const auto k=wheelbase*curvature;
        return {std::atan(k),wheelbase*curvature_rate/(1+k*k),curvature,path_curvature,
                lane.pursuit(x,y,yaw,d),p.offset,heading_error,p.d,lane.remaining()};
    }
};
