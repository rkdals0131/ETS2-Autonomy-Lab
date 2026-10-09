#pragma once
#include <nlohmann/json.hpp>
#include <array>
#include <algorithm>
#include <cmath>
#include <numbers>

namespace bridge {
using json=nlohmann::json;
using V=std::array<double,3>;
using M=std::array<double,9>;
using Q=std::array<double,4>;
inline M transpose(M a) {return {a[0],a[3],a[6],a[1],a[4],a[7],a[2],a[5],a[8]};}
inline V mul(M a,V b) {V r{};for(int i=0;i<3;++i) for(int j=0;j<3;++j) r[i]+=a[i*3+j]*b[j];return r;}
inline M mul(M a,M b) {M r{};for(int i=0;i<3;++i) for(int j=0;j<3;++j) for(int k=0;k<3;++k) r[3*i+j]+=a[3*i+k]*b[3*k+j];return r;}
inline V add(V a,V b) {return {a[0]+b[0],a[1]+b[1],a[2]+b[2]};}
inline V sub(V a,V b) {return {a[0]-b[0],a[1]-b[1],a[2]-b[2]};}
inline M matrix(const json& a) {return {a.at(0),a.at(1),a.at(2),a.at(4),a.at(5),a.at(6),a.at(8),a.at(9),a.at(10)};}
inline M from_quat(Q q) {
    const auto w=q[0],x=q[1],y=q[2],z=q[3];
    return {1-2*(y*y+z*z),2*(x*y-w*z),2*(x*z+w*y),2*(x*y+w*z),1-2*(x*x+z*z),2*(y*z-w*x),2*(x*z-w*y),2*(y*z+w*x),1-2*(x*x+y*y)};
}
inline Q quaternion(M a) { // ROS x,y,z,w, matrix maps child vectors into parent.
    Q q{};const auto trace=a[0]+a[4]+a[8];
    if(trace>0) {const auto s=std::sqrt(trace+1)*2;q={(a[7]-a[5])/s,(a[2]-a[6])/s,(a[3]-a[1])/s,s/4};}
    else {
        int i=0;if(a[4]>a[0]) i=1;if(a[8]>a[i*3+i]) i=2;
        const int j=(i+1)%3,k=(i+2)%3;const auto s=std::sqrt(1+a[i*3+i]-a[j*3+j]-a[k*3+k])*2;
        q[i]=s/4;q[j]=(a[j*3+i]+a[i*3+j])/s;q[k]=(a[k*3+i]+a[i*3+k])/s;q[3]=(a[k*3+j]-a[j*3+k])/s;
    }
    double norm=0;for(double v:q) norm+=v*v;
    for(auto& v:q) v/=std::sqrt(norm);
    return q;
}
inline V scale(V a,double s) {for(auto& v:a) v*=s;return a;}
inline V cross(V a,V b) {return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
inline bool finite(V a) {return std::all_of(a.begin(),a.end(),[](double x){return std::isfinite(x);});}
inline const M enu{1,0,0,0,0,-1,0,1,0},optical{1,0,0,0,-1,0,0,0,-1},base_to_model{0,-1,0,0,0,1,-1,0,0};
inline V world_position(V origin,M rotation,V local) {return mul(enu,add(origin,mul(rotation,local)));}
struct BasePose {V position;M rotation;};
inline BasePose world_base_pose(V origin,M rotation,V base) {
    return {world_position(origin,rotation,base),mul(mul(enu,rotation),base_to_model)};
}
}
