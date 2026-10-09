#include "ot.hpp"
#include <array>
#include <cmath>
#include <set>

namespace ot {
namespace {
using Vec = std::array<double,3>;
using Quat = std::array<double,4>; // wxyz
struct PxPose { float x,y,z,w,px,py,pz; };
struct CellPosition { float x,y,z; int16_t cx,cz; };
static_assert(sizeof(PxPose)==28 && sizeof(CellPosition)==16);
Vec add(Vec a,Vec b) {return {a[0]+b[0],a[1]+b[1],a[2]+b[2]};}
Vec negate(Vec a) {return {-a[0],-a[1],-a[2]};}
Vec cross(Vec a,Vec b) {return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
Quat conjugate(Quat q) {return {q[0],-q[1],-q[2],-q[3]};}
Quat multiply(Quat a,Quat b) {
    return {a[0]*b[0]-a[1]*b[1]-a[2]*b[2]-a[3]*b[3],
            a[0]*b[1]+a[1]*b[0]+a[2]*b[3]-a[3]*b[2],
            a[0]*b[2]-a[1]*b[3]+a[2]*b[0]+a[3]*b[1],
            a[0]*b[3]+a[1]*b[2]-a[2]*b[1]+a[3]*b[0]};
}
Vec rotate(Quat q,Vec v) {
    Vec u{q[1],q[2],q[3]},t=cross(u,v),uxt=cross(u,t);
    return {v[0]+2*(q[0]*t[0]+uxt[0]),v[1]+2*(q[0]*t[1]+uxt[1]),v[2]+2*(q[0]*t[2]+uxt[2])};
}
bool decode(const PxPose& p,Quat& q,Vec& position) {
    q={p.w,p.x,p.y,p.z};position={p.px,p.py,p.pz};
    double norm=0;
    for(auto x:q) {if(!std::isfinite(x)) return false;norm+=x*x;}
    for(auto x:position) if(!std::isfinite(x)) return false;
    if(norm==0) return false;
    for(auto& x:q) x/=std::sqrt(norm);
    return true;
}
}

json read_world_traffic(const json& schema) {
    struct Array {uintptr_t vtable,data;uint64_t size,capacity;};
    struct Placement {float x,y,z;int16_t cx,cz;float w,qx,qy,qz;};
    const auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto& layout=schema.at("render_vehicle");const auto& physics=schema.at("traffic_physics");
    json result={{"available",false},{"phase","sdk_frame_end"},{"vehicles",json::array()}};
    uintptr_t manager{};
    if(!read_memory(base+layout.at("traffic_pointer_rva").get<uintptr_t>(),manager) || !manager) {
        result["error"]="Traffic manager unavailable";return result;
    }
    std::set<uintptr_t> actors;
    for(const auto* name:{"spawned_array_1_offset","spawned_array_2_offset"}) {
        Array list{};
        if(!read_memory(manager+layout.at(name).get<uintptr_t>(),list) || list.size>list.capacity || list.size>4096 || (list.size && !list.data)) {
            result["error"]="Traffic actor array unavailable";return result;
        }
        for(uint64_t i=0;i<list.size;++i) {
            uintptr_t actor{};
            if(!read_memory(list.data+i*16,actor)) {result["error"]="Traffic actor entry unavailable";return result;}
            if(actor) actors.insert(actor);
        }
    }
    for(const auto actor:actors) {
        Placement p{};std::array<float,6> bounds{};uintptr_t body{};float speed{};
        if(!read_memory(actor+layout.at("actor_placement_offset").get<uintptr_t>(),p) ||
           !read_memory(actor+layout.at("actor_aabb_offset").get<uintptr_t>(),bounds) ||
           !read_memory(actor+physics.at("actor_physics_offset").get<uintptr_t>(),body) || !body ||
           !read_memory(body+physics.at("speed_offset").get<uintptr_t>(),speed)) {
            result["error"]="Traffic actor changed during observation";return result;
        }
        Quat q{p.w,p.qx,p.qy,p.qz};double norm=0;
        for(auto v:q) {if(!std::isfinite(v)) {result["error"]="Invalid traffic pose";return result;}norm+=v*v;}
        for(auto v:bounds) if(!std::isfinite(v)) {result["error"]="Invalid traffic bounds";return result;}
        if(!norm || !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) || !std::isfinite(speed)) {
            result["error"]="Invalid traffic observation";return result;
        }
        for(auto& v:q) v/=std::sqrt(norm);
        const auto center=rotate(q,{(bounds[0]+bounds[3])/2,(bounds[1]+bounds[4])/2,(bounds[2]+bounds[5])/2});
        const auto forward=rotate(q,{0,0,-1});
        result["vehicles"].push_back({{"id",actor},{"x",p.x+512.0*p.cx+center[0]},
            {"y",-(p.z+512.0*p.cz+center[2])},{"yaw",std::atan2(-forward[2],forward[0])},
            {"speed_mps",speed},{"length_m",bounds[5]-bounds[2]},{"width_m",bounds[3]-bounds[0]}});
    }
    result["available"]=true;return result;
}

json read_vehicle_physics(uintptr_t actor,const json& schema) {
    const auto& s=schema.at("vehicle_physics");
    const auto offset=[&](const char* name) {return s.at(name).get<uintptr_t>();};
    auto missing=[](const char* reason) {return json{{"available",false},{"reason",reason}};};
    uintptr_t vehicle{},body{},backend{},member{},vtable{};
    if(!read_memory(actor+offset("actor_vehicle_offset"),vehicle) || !vehicle ||
       !read_memory(vehicle+offset("vehicle_body_offset"),body) || !body ||
       !read_memory(body+offset("body_backend_offset"),backend) || !backend ||
       !read_memory(backend,vtable)) return missing("physics_body_unavailable");
    if(vtable!=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))+offset("backend_vtable_rva"))
        return missing("physics_backend_type_not_supported");
    uint32_t flags{}; uintptr_t local_address=backend+offset("backend_mass_pose_offset");
    if(!read_memory(backend+offset("backend_flags_offset"),flags)) return missing("physics_flags_unavailable");
    if(flags&s.at("buffered_mass_flag").get<uint32_t>()) {
        uintptr_t buffer{};
        if(!read_memory(backend+offset("backend_buffer_offset"),buffer) || !buffer) return missing("physics_buffer_unavailable");
        local_address=buffer+offset("buffer_mass_pose_offset");
    }
    PxPose local{},world{};std::array<float,3> shift{};CellPosition origin{};
    if(!read_memory(local_address,local) || !read_memory(backend+offset("backend_world_pose_offset"),world) ||
       !read_memory(vehicle+offset("vehicle_origin_shift_offset"),shift) ||
       !read_memory(body+offset("body_member_offset"),member)) return missing("physics_pose_unavailable");
    if(member && !read_memory(member+offset("member_origin_offset"),origin)) return missing("physics_origin_unavailable");
    Quat q_local{},q_world{};Vec p_local{},p_world{};
    if(!decode(local,q_local,p_local) || !decode(world,q_world,p_world)) return missing("invalid_physics_transform");
    for(float x:shift) if(!std::isfinite(x)) return missing("invalid_vehicle_origin_shift");
    if(!std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(origin.z)) return missing("invalid_physics_origin");
    // PhysX: actor_world = mass_world * inverse(mass_local).
    // SCS: vehicle_world = actor_world * Translation(-origin_shift).
    Quat rotation=multiply(q_world,conjugate(q_local));
    Vec position=add(p_world,rotate(rotation,negate(p_local)));
    position=add(position,{origin.x+512.0*origin.cx,origin.y,origin.z+512.0*origin.cz});
    position=add(position,rotate(rotation,{-shift[0],-shift[1],-shift[2]}));
    return {{"available",true},{"pose_physics",{{"position_m",position},{"quaternion_wxyz",rotation},{"coordinate_space","world"}}},
        {"mass_center_local_m",add(p_local,{shift[0],shift[1],shift[2]})},
        {"origin_shift",shift},{"phase","sdk_frame_end"},{"source","physx_body_with_scs_origin_shift"}};
}
}
