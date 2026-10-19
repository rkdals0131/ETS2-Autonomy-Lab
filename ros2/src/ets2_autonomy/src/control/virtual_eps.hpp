#pragma once
// Inner steering loop for the profile's relative steering (c_relatsteer=1).
// Measured plant (2026-10-20, Scania S 4x2 without trailer, test pad):
// d(applied)/dt = rate(v)*u - centering(v)*applied, |u| 0.02..1 both signs, no
// deadzone, about one game frame of delay. rate falls almost linearly with speed
// (0.652 - 0.0148*v[m/s]); the self-centering is 0 at standstill and saturates
// near 2.1 /s (fit R^2 0.98/0.96/0.92/0.87/0.86/0.92 at 20/40/60/80/88/97 km/h). The
// table is measured only up to 97 km/h for this truck; the end values are held beyond.
// Inverting the plant (rate feedforward + centering compensation) and a P term
// kp = bandwidth/rate(v) give d(error)/dt = -bandwidth*error at every speed, so
// there is no integral term.
//
// bandwidth 6.5 rad/s (time constant 0.15 s, kp 10 at standstill as before). The
// loop delay is one game frame plus up to one 50/60 Hz sample (about 35 ms), 13 deg
// at crossover, leaving a phase margin near 75 deg; the per-cycle discrete gain
// 6.5*0.02 = 0.13 is far below the 1 limit.
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

struct VirtualEps {
    struct Plant {double rate,centering;}; // units/s at u=1; 1/s pull toward center
    struct Settings {
        double bandwidth=6.5;  // rad/s, inner-loop error decay rate
        double rate_limit=0;   // target slew limit, units/s; 0 disables
    };
    // Measured plant at speed (m/s), piecewise linear between 0/20/40/60/80/88/97 km/h
    // (the last two are the mean speeds of the 90 and 100 km/h blocks, sysid-20261020-033402).
    static Plant plant(double speed) {
        static constexpr std::array<double,7> v{0,20/3.6,40/3.6,60/3.6,80/3.6,88.2/3.6,96.8/3.6};
        static constexpr std::array<Plant,7> p{{{.650,0},{.570,.939},{.489,1.624},{.401,1.995},{.320,2.108},{.284,2.084},{.261,2.093}}};
        speed=std::isfinite(speed)?std::abs(speed):0;
        if(speed>=v.back()) return p.back();
        std::size_t i=0;while(speed>v[i+1]) ++i;
        const auto f=(speed-v[i])/(v[i+1]-v[i]);
        return {p[i].rate+f*(p[i+1].rate-p[i].rate),p[i].centering+f*(p[i+1].centering-p[i].centering)};
    }
    // Rate-limited target; NaN until the first command after construction or reset.
    double target=std::numeric_limits<double>::quiet_NaN();
    void reset() {target=std::numeric_limits<double>::quiet_NaN();}
    // desired and applied are normalized steering [-1,1], desired_rate per second, speed m/s.
    double command(double desired,double desired_rate,double applied,double speed,double dt,const Settings& settings) {
        dt=std::clamp(std::isfinite(dt)?dt:0.0,0.0,.1);
        double rate=desired_rate;
        if(settings.rate_limit>0) {
            if(!std::isfinite(target)) target=applied;
            const auto step=std::clamp(desired-target,-settings.rate_limit*dt,settings.rate_limit*dt);
            if(target+step!=desired) rate=dt>0?step/dt:0;
            target+=step;
            rate=std::clamp(rate,-settings.rate_limit,settings.rate_limit);
        } else target=desired;
        // Inverse of the plant plus P on the error, scaled so the loop bandwidth is speed independent.
        const auto [a,c]=plant(speed);
        return std::clamp((rate+c*applied+settings.bandwidth*(target-applied))/a,-1.0,1.0);
    }
};
