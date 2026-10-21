#pragma once
// Soft (re)engagement of LCC: a reference from the truck's lateral offset e0 and heading
// error h0 back to the lane centre, a cubic Hermite in lane distance over a length L
// (r(0)=e0, r'(0)=tan h0, r(L)=r'(L)=0). The lateral controller steers on the errors
// against it and gets its curvature as feedforward, so engaging off-centre or at an
// angle (after the driver's lane change, or a stick release mid-lane) is a merge instead
// of a correction step. L keeps the reference's peak lateral acceleration v^2*|r''| at
// a_max (|r''| <= 6|e0|/L^2 + 4|tan h0|/L) and is at least t_min of driving.
#include <algorithm>
#include <cmath>

struct LaneMerge {
    double s0=0,length=0,e0=0,m0=0;
    static LaneMerge start(double s,double offset,double heading_error,double speed,double a_max=.5,double t_min=2) {
        const auto v=std::max(std::abs(speed),5.0),m=std::tan(std::clamp(heading_error,-1.0,1.0));
        const auto b=4*std::abs(m)*v*v,c=6*std::abs(offset)*v*v;
        return {s,std::max({20.0,v*t_min,(b+std::sqrt(b*b+4*a_max*c))/(2*a_max)}),offset,m};
    }
    bool done(double s) const {return s-s0>=length;}
    struct Reference {double offset,heading,curvature;};
    Reference at(double s) const {
        if(done(s)) return {0,0,0};
        const auto u=std::max(0.0,(s-s0)/length),L=length;
        const auto r=e0*(2*u*u*u-3*u*u+1)+L*m0*(u*u*u-2*u*u+u);
        const auto dr=(e0*(6*u*u-6*u)+L*m0*(3*u*u-4*u+1))/L;
        const auto ddr=(e0*(12*u-6)+L*m0*(6*u-4))/(L*L);
        return {r,std::atan(dr),ddr/std::pow(1+dr*dr,1.5)};
    }
};
