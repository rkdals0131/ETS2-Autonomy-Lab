#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

// The vehicle the ACC follows. GT traffic gives each actor's position and speed but no
// acceleration, so the lead's acceleration is a first-order low-pass of its speed
// difference. A different lead (cut-in, lane change, lost lead) restarts the estimate
// at 0 instead of differentiating across two vehicles.
class LeadTracker {
public:
    struct Settings {double accel_time_constant_s=.5,accel_limit_mps2=8;};
    struct Lead {
        bool valid=false;uint64_t id=0;
        double gap_m=std::numeric_limits<double>::infinity(),speed_mps=0,accel_mps2=0;
    };
    // id empty: no lead this frame. t in seconds, monotonic.
    Lead update(std::optional<uint64_t> id,double gap_m,double speed_mps,double t,const Settings& settings) {
        if(!id || !std::isfinite(gap_m) || !std::isfinite(speed_mps)) {reset();return {};}
        if(!lead_.valid || lead_.id!=*id || !(t>time_)) {
            lead_={true,*id,gap_m,speed_mps,0};time_=t;return lead_;
        }
        const auto dt=t-time_;
        const auto raw=std::clamp((speed_mps-lead_.speed_mps)/dt,-settings.accel_limit_mps2,settings.accel_limit_mps2);
        const auto alpha=dt/(settings.accel_time_constant_s+dt);
        lead_.accel_mps2+=alpha*(raw-lead_.accel_mps2);
        lead_.gap_m=gap_m;lead_.speed_mps=speed_mps;time_=t;
        return lead_;
    }
    void reset() {lead_={};time_=0;}
private:
    Lead lead_;
    double time_=0;
};
