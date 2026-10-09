#pragma once
#include <array>
#include <cstdint>

namespace ot {
using Vec3 = std::array<double,3>;
using Quaternion = std::array<double,4>; // w,x,y,z
struct Placement {float x,y,z;int16_t cx,cz;float w,qx,qy,qz;};
static_assert(sizeof(Placement)==32);
inline Vec3 world_position(const std::array<float,3>& local,const std::array<int16_t,2>& cells,double scale=512) {
    return {local[0]+scale*cells[0],local[1],local[2]+scale*cells[1]};
}
inline Vec3 world_position(const Placement& p) {return world_position({p.x,p.y,p.z},{p.cx,p.cz});}
inline Quaternion conjugate(Quaternion q) {return {q[0],-q[1],-q[2],-q[3]};}
inline Quaternion multiply(Quaternion a,Quaternion b) {
    return {a[0]*b[0]-a[1]*b[1]-a[2]*b[2]-a[3]*b[3],
        a[0]*b[1]+a[1]*b[0]+a[2]*b[3]-a[3]*b[2],
        a[0]*b[2]-a[1]*b[3]+a[2]*b[0]+a[3]*b[1],
        a[0]*b[3]+a[1]*b[2]-a[2]*b[1]+a[3]*b[0]};
}
inline Vec3 rotate(Quaternion q,Vec3 p) {
    const auto v=multiply(multiply(q,{0,p[0],p[1],p[2]}),conjugate(q));
    return {v[1],v[2],v[3]};
}
}
