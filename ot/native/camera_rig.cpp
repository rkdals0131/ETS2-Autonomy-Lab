#include "camera_rig.hpp"
#include "../include/geometry.hpp"
#include <cmath>
#include <stdexcept>
#include <cstdio>

namespace ot {
namespace {
struct SubmissionCopy {
    alignas(16) std::array<uint8_t,0x540> bytes{};
    CameraRig* owner=nullptr;
};
thread_local SubmissionCopy submission;
thread_local CameraRig* selection_owner=nullptr;
thread_local std::shared_ptr<const void> selection_config;
thread_local uint32_t selection_mask=0;
thread_local uintptr_t graph_camera=0;
using Q=std::array<double,4>;
using V=std::array<double,3>;
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
bool cabin_pose(uintptr_t interior,Placement& pose) noexcept {
    uintptr_t vehicle{},camera{};
    // The engine has already composed body interpolation and cabin suspension
    // into this parent, before applying the driver's head pose. Model-local
    // positions use neutral chassis axes/origin, as do native mirror locators.
    return read_memory(interior+0x118,vehicle) && vehicle &&
        read_memory(vehicle+0x1098,camera) && camera && read_memory(camera+0x4d4,pose);
}
}

json CameraRig::configure(const json& request) {
    if(!request.value("enabled",true)) {clear();return status();}
    auto config=std::make_shared<Configuration>();
    config->ego_full_model=request.value("ego_full_model",false);
    config->private_outputs=request.value("private_outputs",false);
    config->capture_warmup=request.value("capture_warmup",false);
    last_render_mask_=0;
    for(const auto& item:request.at("views")) {
        const int slot=item.at("slot").get<int>();
        if(slot<0 || slot>=9 || config->views[slot].enabled)
            throw std::runtime_error("Camera rig slots must be unique and between 0 and 8");
        auto& view=config->views[slot];
        view.source_slot=item.value("source_slot",slot);
        if(view.source_slot>=9) throw std::runtime_error("Camera source slot must be between 0 and 8");
        const auto basis=item.value("basis",std::string("chassis"));
        if(basis!="world" && basis!="chassis" && basis!="cabin")
            throw std::runtime_error("Camera basis must be world, chassis or cabin");
        view.basis=basis=="world"?Basis::world:basis=="cabin"?Basis::cabin:Basis::chassis;
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
        if(item.contains("base_resolution")) {
            const auto size=item.at("base_resolution").get<std::array<int,2>>();
            if(size[0]<1 || size[1]<1 || size[0]>16384 || size[1]>16384)
                throw std::runtime_error("Camera resolution exceeds the D3D11 texture dimension range");
            view.resolution={static_cast<uint32_t>(size[0]),static_cast<uint32_t>(size[1])};
        }
        view.enabled=true;config->mask|=1u<<slot;
    }
    if(!config->mask) throw std::runtime_error("Camera rig needs at least one view");
    configuration_.store(std::move(config));
    return status();
}
json CameraRig::status() {
    auto config=configuration_.load();json views=json::array();
    for(size_t slot=0;slot<9;++slot) {
        json view={{"slot",slot},{"applied",applied_[slot].load()}};
        if(config && config->views[slot].enabled) {
            const auto& v=config->views[slot];
            view.update({{"basis",v.basis==Basis::world?"world":v.basis==Basis::cabin?"cabin":"chassis"},{"position",v.position},
                {"quaternion_wxyz",v.rotation},{"hfov_deg",v.hfov},{"vfov_deg",v.vfov},
                {"base_resolution",v.resolution}});
        }
        views.push_back(std::move(view));
    }
    return {{"enabled",config!=nullptr},{"selected_mask",config?config->mask:0},
        {"ego_full_model",config && config->ego_full_model},{"ego_parts_applied",ego_parts_applied_.load()},
        {"private_outputs",config && config->private_outputs},{"private_ready",private_ready_.load()},
        {"capture_warmup",config && config->capture_warmup},
        {"in_flight",in_flight()},{"pending_graphs",graphs_.load()},{"unavailable",unavailable_.load()},{"views",views},
        {"source","private submission copy; persistent mirror fields unchanged"}};
}
void CameraRig::select(safetyhook::Context& context,uint32_t capture_mask,uint32_t warmup_mask,uint64_t present_id) noexcept {
    if(auto config=configuration_.load()) {
        // This hook runs after native camera updates. Register-only array
        // redirection leaves the engine's owning arrays and HUD aliases intact.
        if(config->private_outputs) {
            if(in_flight()) {context.r12&=~uint64_t(config->mask);last_render_mask_=0;return;}
            if(config->capture_warmup && !(capture_mask|warmup_mask)) {
                context.r12&=~uint64_t(config->mask);last_render_mask_=0;return;
            }
            if(!prepare_private(context.r14,*config)) {++unavailable_;return;}
            prepared_configuration_.store(config);
            context.r14=reinterpret_cast<uintptr_t>(&camera_array_);
            // After an idle interval the engine needs one complete preparation
            // render. Only a following consecutive frame is eligible to capture.
            capture_mask=config->capture_warmup?capture_mask|warmup_mask:config->mask;
            selection_mask=config->mask&capture_mask;
            last_render_frame_=present_id;last_render_mask_=selection_mask;
            if(selection_mask) {selection_owner=this;selection_config=config;++selections_;}
        }
        // Unowned mirrors retain the engine's choice.
        context.r12=(context.r12&~uint64_t(config->mask))|(config->mask&capture_mask);
    }
}
bool CameraRig::prepare_private(uintptr_t cameras,const Configuration& config) noexcept {
    uintptr_t controller{};Array camera_array{},drawable_array{};
    const auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if(!read_memory(cameras,camera_array) || camera_array.size!=9 ||
       !read_memory(base+0x36AE6D8,controller) || !controller ||
       !read_memory(controller+0xA8,drawable_array) || drawable_array.size!=9) return false;
    std::array<uintptr_t,9> sources{},drawables{};
    if(!copy_memory(camera_array.data,sources.data(),sizeof(sources)) ||
       !copy_memory(drawable_array.data,drawables.data(),sizeof(drawables))) return false;
    const auto original_sources=sources,original_drawables=drawables;
    for(unsigned i=0;i<9;++i) if(config.views[i].enabled) {
        const auto source=config.views[i].source_slot;auto& view=private_views_[i];
        if(!original_sources[source] || !original_drawables[source] ||
           !copy_memory(original_sources[source],view.camera.data(),view.camera.size()) ||
           !copy_memory(original_drawables[source],view.drawable.data(),view.drawable.size())) return false;
        // The descriptor's transient graph image is named independently of
        // native mirror textures. An invalid alias means graph output only.
        std::snprintf(view.name.data(),view.name.size(),"ot/sensor%u",i);
        const auto name=reinterpret_cast<uintptr_t>(view.name.data());
        const uint32_t length=static_cast<uint32_t>(std::strlen(view.name.data()));
        const uint16_t invalid=0xffff;
        const uint32_t mode=0;
        std::memcpy(view.drawable.data()+0x38,&name,8);
        std::memcpy(view.drawable.data()+0x40,&length,4);
        std::memcpy(view.drawable.data()+0x148,&invalid,2);
        std::memcpy(view.drawable.data()+0x160,&invalid,2);
        std::memcpy(view.camera.data()+0x518,&mode,4);
        sources[i]=reinterpret_cast<uintptr_t>(view.camera.data());
        drawables[i]=reinterpret_cast<uintptr_t>(view.drawable.data());
    }
    camera_pointers_=sources;drawable_pointers_=drawables;
    camera_array_=camera_array;camera_array_.data=reinterpret_cast<uintptr_t>(camera_pointers_.data());
    drawable_array_=drawable_array;drawable_array_.data=reinterpret_cast<uintptr_t>(drawable_pointers_.data());
    interior_=cameras-0x13A8;private_ready_=true;
    return true;
}
void CameraRig::submission_drawables(safetyhook::Context& context) noexcept {
    if(context.r14==reinterpret_cast<uintptr_t>(&camera_array_)) context.rcx=reinterpret_cast<uintptr_t>(&drawable_array_);
}
void CameraRig::graph_drawables(safetyhook::Context& context) noexcept {
    graph_camera=0;uintptr_t record{},camera{};
    if(!private_ready_ || !read_memory(context.rsp+0x40,record) || !read_memory(record+8,camera)) return;
    std::lock_guard lock(requests_mutex_);
    if(requests_.contains(camera)) {
        graph_camera=camera;
        context.rbx=reinterpret_cast<uintptr_t>(&drawable_array_);
    }
}
void CameraRig::graph_cameras(safetyhook::Context& context) noexcept {
    if(graph_camera)
        context.rcx=reinterpret_cast<uintptr_t>(&camera_array_);
}
void CameraRig::graph_end() noexcept {
    if(!graph_camera) return;
    std::lock_guard lock(requests_mutex_);
    if(requests_.erase(graph_camera)) --graphs_;
    graph_camera=0;
}
void CameraRig::begin(safetyhook::Context& context) noexcept {
    auto config=selection_owner==this?std::static_pointer_cast<const Configuration>(selection_config):configuration_.load();
    const auto slot=static_cast<uint32_t>(context.rbp);
    if(!config || slot>=9 || !config->views[slot].enabled) return;
    if(config->private_outputs && selection_owner!=this) return;
    if(submission.owner) {++unavailable_;return;}
    const auto& view=config->views[slot];V position=view.position;Q rotation=view.rotation;
    if(view.basis!=Basis::world) {
        Placement body{};
        const auto interior=context.r14==reinterpret_cast<uintptr_t>(&camera_array_)?interior_.load():context.r14-0x13a8;
        if(!(view.basis==Basis::cabin?cabin_pose(interior,body):chassis_pose(interior,body))) {++unavailable_;return;}
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
void CameraRig::end(safetyhook::Context& context) noexcept {
    if(submission.owner==this) {submission.owner=nullptr;--in_flight_;}
    if(selection_owner==this && (selection_mask&(1u<<context.rbp))) {
        {std::lock_guard lock(requests_mutex_);if(requests_.insert(context.rdi).second) ++graphs_;}
        selection_mask&=~(1u<<context.rbp);
        if(!selection_mask) {selection_owner=nullptr;selection_config.reset();--selections_;}
    }
}
void CameraRig::dimensions(safetyhook::Context& context) noexcept {
    uintptr_t caller{};
    if(!read_memory(context.rsp,caller) ||
       caller!=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))+0x4d46b8) return;
    const auto slot=static_cast<uint32_t>(context.rdi);
    const auto config=configuration_.load();
    if(!config || slot>=9 || !config->views[slot].enabled) return;
    const auto& size=config->views[slot].resolution;
    if(size[0]) {context.rdx=size[0];context.r8=size[1];}
}
void CameraRig::ego_parts(safetyhook::Context& context) noexcept {
    uintptr_t caller{};
    static const auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if(!read_memory(context.rsp+0x48,caller) || (caller!=base+0x646DC9 && caller!=base+0x646E57)) return;
    const auto config=configuration_.load();
    if(!config || !config->ego_full_model) return;
    uintptr_t mask{},controller{},actor{},vehicle{};
    constexpr std::array<uint64_t,9> masks={0x400,0x800,0x1000,0x2000,0x4000,0x8000,0x10000,0x0800000000000000,0x1000000000000000};
    uint64_t sensor_mask=0;for(unsigned i=0;i<9;++i) if(config->mask&(1u<<i)) sensor_mask|=masks[i];
    // A3CADE follows the cached-model "all parts" test. Limit the override to
    // the two body model calls made by the player's 646C00 submission, and to
    // camera slots owned by this rig. Other models and normal mirrors keep
    // their native per-mirror subsets. No model/camera memory is changed.
    if(!read_memory(context.r8+0x18,mask) || !(mask&sensor_mask) ||
       !read_memory(base+0x36AE6D8,controller) || !controller ||
       !read_memory(controller+0x31B0,actor) || !actor ||
       !read_memory(actor+0x18,vehicle) || context.rsi!=vehicle) return;
    context.rflags&=~uintptr_t{0x40}; // ZF=0: use the engine's full-list path.
    ++ego_parts_applied_;
}
}
