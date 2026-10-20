#pragma once
// Road grade for the ACC's mass estimator, from the GT pose: rise over the horizontal distance
// of the last ~20 m driven. The SDK pitch also carries the suspension's pitch, which biased the
// estimated mass 3-4 % low on the recordings (apps/control/matlab/acc/README.md).
#include <array>
#include <cmath>
#include <deque>
#include <limits>

struct RoadGrade {
    double window_m=20;
    double grade=std::numeric_limits<double>::quiet_NaN(); // rad, uphill positive; NaN until 10 m
    // x, y horizontal and z up (ROS world). Points closer than 0.5 m to the last are skipped,
    // so the grade holds at standstill.
    double update(double x,double y,double z) {
        if(!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return grade;
        if(!trail_.empty() && horizontal(trail_.back(),{x,y,z})<.5) return grade;
        trail_.push_back({x,y,z});
        while(trail_.size()>2 && horizontal(trail_[1],trail_.back())>=window_m) trail_.pop_front();
        const auto run=horizontal(trail_.front(),trail_.back());
        if(run>=window_m/2) grade=std::atan2(trail_.back()[2]-trail_.front()[2],run);
        return grade;
    }
    void reset() {trail_.clear();grade=std::numeric_limits<double>::quiet_NaN();}
private:
    static double horizontal(const std::array<double,3>& a,const std::array<double,3>& b) {return std::hypot(b[0]-a[0],b[1]-a[1]);}
    std::deque<std::array<double,3>> trail_;
};
