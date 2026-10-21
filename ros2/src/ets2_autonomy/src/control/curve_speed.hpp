#pragma once
// Curve speed for ACC while LCC follows the lane: the highest speed from which the truck can
// slow to sqrt(a_lat/|kappa|) at every point ahead with the ACC's gentle set-speed ramp. The
// ACC lowers a reduced set speed on a ramp of at most 0.5 m/s^2 (acc README, set speed), so
// planning with a smaller deceleration keeps it on time; the curvature is the lane's mean
// over window_m. Before 2026-10-21 the limit used the curvature 6 + 0.7 v m ahead only, and the
// truck entered an R130 curve at 87 km/h (4.2 m/s^2).
#include "path_following.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

inline double curve_speed(const LanePath& lane,double from_s,double lateral_acceleration,double deceleration,
                          double horizon_m=700,double step_m=5,double window_m=10) {
    auto limit=std::numeric_limits<double>::infinity();
    const auto end=std::min(from_s+horizon_m,lane.points.back().s);
    for(double s=from_s;s<=end;s+=step_m) {
        const auto k=std::abs(lane.curvature(s,window_m));
        if(!(k>1e-6)) continue;
        limit=std::min(limit,std::sqrt(lateral_acceleration/k+2*deceleration*(s-from_s)));
    }
    return limit;
}
