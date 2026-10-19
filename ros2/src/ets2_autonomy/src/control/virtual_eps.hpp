#pragma once
// Inner steering loop for the profile's relative steering (c_relatsteer=1).
// Measured plant (standstill, 2026-10-20): d(applied)/dt = rate_per_unit*u with
// rate_per_unit 0.65 /s for |u| 0.02..1, both signs, no deadzone, about one game
// frame of delay, and no self-centering at u=0 (centering is a parameter for the
// speed dependence that is not measured yet). On an integrator a P loop already
// has zero steady-state error and a rate feedforward removes the ramp lag, so
// there is no integral term.
//
// kp=10: loop crossover 0.65*kp = 6.5 rad/s (time constant 0.15 s). The loop
// delay is one game frame plus up to one 50/60 Hz sample (about 35 ms), 13 deg
// at crossover, leaving a phase margin near 75 deg; the per-cycle discrete gain
// 0.65*kp*0.02 = 0.13 is far below the 1 limit. Margin covers the speed
// dependence of the rate that is not yet measured.
#include <algorithm>
#include <cmath>
#include <limits>

struct VirtualEps {
    struct Settings {
        double rate_per_unit=.65; // applied steering units per second at u=1
        double kp=10;             // 1/s per unit of applied-steering error
        double rate_limit=0;      // target slew limit, units/s; 0 disables
        double centering=0;       // 1/s pull toward center, d(applied)/dt -= centering*applied; 0 at standstill
    };
    // Rate-limited target; NaN until the first command after construction or reset.
    double target=std::numeric_limits<double>::quiet_NaN();
    void reset() {target=std::numeric_limits<double>::quiet_NaN();}
    // desired and applied are normalized steering [-1,1], desired_rate per second.
    double command(double desired,double desired_rate,double applied,double dt,const Settings& settings) {
        dt=std::clamp(std::isfinite(dt)?dt:0.0,0.0,.1);
        double rate=desired_rate;
        if(settings.rate_limit>0) {
            if(!std::isfinite(target)) target=applied;
            const auto step=std::clamp(desired-target,-settings.rate_limit*dt,settings.rate_limit*dt);
            if(target+step!=desired) rate=dt>0?step/dt:0;
            target+=step;
            rate=std::clamp(rate,-settings.rate_limit,settings.rate_limit);
        } else target=desired;
        // Inverse of d(applied)/dt = rate_per_unit*u - centering*applied, plus P on the error.
        return std::clamp((rate+settings.centering*applied)/settings.rate_per_unit+settings.kp*(target-applied),-1.0,1.0);
    }
};
