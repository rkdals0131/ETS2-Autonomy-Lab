#pragma once
// Soft (re)engagement and lane change of LCC: a reference from the truck's lateral offset e0
// and heading error h0 to the lane centre, a quintic in lane distance over a length L
// (r(0)=e0, r'(0)=tan h0, r''(0)=0, r(L)=r'(L)=r''(L)=0: minimum jerk, no step of lateral
// acceleration at either end). The lateral controller steers on the errors against it and
// gets its curvature as feedforward, so engaging off-centre or at an angle (after the
// driver's lane change, or a stick release mid-lane) is a merge instead of a correction
// step, and a lane change is the same merge from one lane width off. L keeps the offset
// term's peak lateral acceleration v^2*|r''| at a_max (5.77|e0|/L^2) and the heading term's
// at 2*a_max (3.94|tan h0|/L: turning the heading back needs lateral travel v^2 h0^2/(2a)
// whatever the path, so a long path only carries the truck further off), and is at least
// t_min of driving.
#include <algorithm>
#include <cmath>

struct LaneMerge {
    double s0=0,length=0,e0=0,m0=0;
    static LaneMerge start(double s,double offset,double heading_error,double speed,double a_max=.5,double t_min=2) {
        const auto v=std::max(std::abs(speed),5.0),m=std::tan(std::clamp(heading_error,-1.0,1.0));
        const auto b=3.94*std::abs(m)*v*v/2,c=5.78*std::abs(offset)*v*v;
        return {s,std::max({20.0,v*t_min,(b+std::sqrt(b*b+4*a_max*c))/(2*a_max)}),offset,m};
    }
    bool done(double s) const {return s-s0>=length;}
    struct Reference {double offset,heading,curvature;};
    Reference at(double s) const {
        if(done(s)) return {0,0,0};
        const auto u=std::max(0.0,(s-s0)/length),L=length,u2=u*u,u3=u2*u;
        const auto r=e0*(1-10*u3+15*u3*u-6*u3*u2)+L*m0*(u-6*u3+8*u3*u-3*u3*u2);
        const auto dr=(e0*(-30*u2+60*u3-30*u3*u)+L*m0*(1-18*u2+32*u3-15*u3*u))/L;
        const auto ddr=(e0*(-60*u+180*u2-120*u3)+L*m0*(-36*u+96*u2-60*u3))/(L*L);
        return {r,std::atan(dr),ddr/std::pow(1+dr*dr,1.5)};
    }
};
