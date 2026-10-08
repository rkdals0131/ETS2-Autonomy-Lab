#include "camera_rig.hpp"
#include <cmath>
#include <stdexcept>

namespace ot {
namespace {
struct Placement {float x,y,z;int16_t cx,cz;float w,qx,qy,qz;};
static_assert(sizeof(Placement)==32);
struct SubmissionCopy {
    alignas(16) std::array<uint8_t,0x540> bytes{};
    CameraRig* owner=nullptr;
};
thread_local SubmissionCopy submission;
using Q=std::array<double,4>;
using V=std::array<double,3>;
Q multiply(Q a,Q b) {
    return {a[0]*b[0]-a[1]*b[1]-a[2]*b[2]-a[3]*b[3],
        a[0]*b[1]+a[1]*b[0]+a[2]*b[3]-a[3]*b[2],
        a[0]*b[2]-a[1]*b[3]+a[2]*b[0]+a[3]*b[1],
        a[0]*b[3]+a[1]*b[2]-a[2]*b[1]+a[3]*b[0]};
}
V rotate(Q q,V p) {
    auto v=multiply(multiply(q,{0,p[0],p[1],p[2]}),{q[0],-q[1],-q[2],-q[3]});
    return {v[1],v[2],v[3]};
}
bool chassis_pose(uintptr_t interior,Placement& pose) noexcept {
    uintptr_t vehicle{},state{},controller{},manager{},buffers{},placements{};
    uint32_t mode{},flags{};uint64_t handle{},count{};
    const auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if(!read_memory(interior+0x118,vehicle) || !vehicle ||
       !read_memory(vehicle+0x38,state) || !state || !read_memory(state,mode)) return false;
    if(mode==2 || mode==3) return read_memory(state+0x10,pose);
    if(mode!=1 || !read_memory(state+8,handle) || (handle>>56)==0xff ||
       !read_memory(base+0x36AE6D8,controller) || !controller ||
       !read_memory(controller+0x3260,manager) || !manager ||
       !read_memory(manager+8,buffers) || !buffers ||
       !read_memory(buffers+8,placements) || !placements ||
       !read_memory(buffers+0x10,count)) return false;
    const auto index=handle&0xfffffffffffffULL;
    if(index>=count || !read_memory(placements+index*0x24+0x20,flags) ||
       (flags&0x20) || (((handle>>52)^flags)&15)) return false;
    return read_memory(placements+index*0x24,pose);
}
}

json CameraRig::configure(const json& request) {
    if(!request.value("enabled",true)) {clear();return status();}
    auto config=std::make_shared<Configuration>();
    for(const auto& item:request.at("views")) {
        const int slot=item.at("slot").get<int>();
        if(slot<0 || slot>=6 || config->views[slot].enabled)
            throw std::runtime_error("Camera rig slots must be unique and between 0 and 5");
        auto& view=config->views[slot];
        const auto basis=item.value("basis",std::string("chassis"));
        if(basis!="world" && basis!="chassis") throw std::runtime_error("Camera basis must be world or chassis");
        view.chassis=basis=="chassis";
        view.position=item.at("position").get<V>();
        view.rotation=item.at("quaternion_wxyz").get<Q>();
        double norm=0;
        for(auto value:view.rotation) {if(!std::isfinite(value)) throw std::runtime_error("Invalid camera quaternion");norm+=value*value;}
        if(!std::isfinite(norm) || norm==0) throw std::runtime_error("Invalid camera quaternion");
        for(auto& value:view.rotation) value/=std::sqrt(norm);
        for(auto value:view.position) if(!std::isfinite(value) || std::abs(value)>16000000)
            throw std::runtime_error("Camera position exceeds the engine cell coordinate range");
        view.hfov=item.at("hfov_deg").get<float>();view.vfov=item.at("vfov_deg").get<float>();
        if(!(view.hfov>0 && view.hfov<179 && view.vfov>0 && view.vfov<179))
            throw std::runtime_error("Camera FOV must be between 0 and 179 degrees");
        view.enabled=true;config->mask|=1u<<slot;
    }
    if(!config->mask) throw std::runtime_error("Camera rig needs at least one view");
    configuration_.store(std::move(config));
    return status();
}
json CameraRig::status() {
    auto config=configuration_.load();json views=json::array();
    for(size_t slot=0;slot<6;++slot) {
        json view={{"slot",slot},{"applied",applied_[slot].load()}};
        if(config && config->views[slot].enabled) {
            const auto& v=config->views[slot];
            view.update({{"basis",v.chassis?"chassis":"world"},{"position",v.position},
                {"quaternion_wxyz",v.rotation},{"hfov_deg",v.hfov},{"vfov_deg",v.vfov}});
        }
        views.push_back(std::move(view));
    }
    return {{"enabled",config!=nullptr},{"selected_mask",config?config->mask:0},
        {"in_flight",in_flight_.load()},{"unavailable",unavailable_.load()},{"views",views},
        {"source","private submission copy; persistent mirror fields unchanged"}};
}
void CameraRig::select(safetyhook::Context& context) noexcept {
    if(auto config=configuration_.load()) context.r12|=config->mask;
}
void CameraRig::begin(safetyhook::Context& context) noexcept {
    auto config=configuration_.load();const auto slot=static_cast<uint32_t>(context.rbp);
    if(!config || slot>=6 || !config->views[slot].enabled) return;
    if(submission.owner) {++unavailable_;return;}
    const auto& view=config->views[slot];V position=view.position;Q rotation=view.rotation;
    if(view.chassis) {
        Placement body{};
        if(!chassis_pose(context.r14-0x13a8,body)) {++unavailable_;return;}
        Q q{body.w,body.qx,body.qy,body.qz};
        const auto relative=rotate(q,position);
        position={body.cx*512.0+body.x+relative[0],body.y+relative[1],body.cz*512.0+body.z+relative[2]};
        rotation=multiply(q,rotation);
    }
    uintptr_t vtable{},convert{};
    if(!read_memory(context.rbx,vtable) || !read_memory(vtable+0x88,convert) ||
       convert!=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))+0x87fd70 ||
       !copy_memory(context.rbx,submission.bytes.data(),submission.bytes.size())) {++unavailable_;return;}
    const auto cx=std::floor(position[0]/512.0),cz=std::floor(position[2]/512.0);
    if(cx<-32768 || cx>32767 || cz<-32768 || cz>32767) {++unavailable_;return;}
    // 0x87FD70 composes the native quaternion with a Z half-turn, then the
    // camera component builds its inverse view. Cancel that half-turn here:
    // +X right, +Y up, -Z forward, without a reflected/upside-down sensor view.
    rotation=multiply(rotation,{0,0,0,1});
    Placement pose{static_cast<float>(position[0]-cx*512),static_cast<float>(position[1]),
        static_cast<float>(position[2]-cz*512),static_cast<int16_t>(cx),static_cast<int16_t>(cz),
        static_cast<float>(rotation[0]),static_cast<float>(rotation[1]),static_cast<float>(rotation[2]),static_cast<float>(rotation[3])};
    std::memcpy(submission.bytes.data()+0x38,&view.hfov,4);
    std::memcpy(submission.bytes.data()+0x3c,&view.vfov,4);
    std::memcpy(submission.bytes.data()+0x40,&pose,sizeof(pose));
    submission.owner=this;++in_flight_;
    context.rbx=reinterpret_cast<uintptr_t>(submission.bytes.data());
    ++applied_[slot];
}
void CameraRig::end() noexcept {
    if(submission.owner==this) {submission.owner=nullptr;--in_flight_;}
}
}
