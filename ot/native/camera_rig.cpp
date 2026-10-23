#include "camera_rig.hpp"
#include "proxy_passes.hpp"
#include "../include/geometry.hpp"
#include <bit>
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
       !read_memory(base+0x36AF7C8,controller) || !controller ||
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
    // 1.61.1.9 moved the vehicle fields from 0x690 up by 0x28 (0x1098 before).
    return read_memory(interior+0x118,vehicle) && vehicle &&
        read_memory(vehicle+0x10C0,camera) && camera && read_memory(camera+0x4d4,pose);
}
}

json CameraRig::configure(const json& request) {
    if(!request.value("enabled",true)) {clear();return status();}
    auto config=std::make_shared<Configuration>();
    config->ego_full_model=request.value("ego_full_model",false);
    config->private_outputs=request.value("private_outputs",false);
    config->capture_warmup=request.value("capture_warmup",false);
    config->native_mirrors=request.value("native_mirrors",true);
    // docs/28 §11.23: with snowymoon AA on, sensor views in a frame where the
    // proxy records no frame camera cost ~0.5 s in its final pass. Its AA
    // setting and frame counters are read only for the pinned build (proxy_gate.hpp).
    const auto proxy=proxy::current_environment();
    if(proxy.base && proxy.in_game_directory && proxy.sha256==proxy::Layout::sha256) {
        config->proxy_aa=reinterpret_cast<const uint32_t*>(proxy.base+proxy::Layout::aa_copy);
        config->proxy_frame=reinterpret_cast<const uint32_t*>(proxy.base+proxy::Layout::frame_source);
        config->proxy_record=reinterpret_cast<const uint32_t*>(proxy.base+proxy::Layout::record_frame);
    }
    config->pause_without_mirrors=request.value("pause_without_mirrors",config->proxy_aa!=nullptr);
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
        json view={{"slot",slot},{"applied",applied_[slot].load()},{"native_applied",native_applied_[slot].load()}};
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
        {"native_mirrors",!config || config->native_mirrors},{"native_suppressed",native_suppressed_.load()},
        {"pause_without_mirrors",config && config->pause_without_mirrors},{"mirrorless_skips",mirrorless_skips_.load()},{"recordless_skips",recordless_skips_.load()},
        {"free_engine_slots",free_slots_.load()},{"slot_map",slot_map()},
        {"in_flight",in_flight()},{"pending_graphs",graphs_.load()},{"unavailable",unavailable_.load()},{"views",views},
        {"source","private submission copy; persistent mirror fields unchanged"}};
}
namespace {
constexpr uint64_t no_views=0xFFFFFFFFFULL;
uint64_t pack(const std::array<uint8_t,9>& map) noexcept {
    uint64_t packed=0;
    for(unsigned slot=0;slot<9;++slot) packed|=uint64_t(map[slot]&0xF)<<(4*slot);
    return packed;
}
}
std::array<uint8_t,9> CameraRig::slot_map() const noexcept {
    std::array<uint8_t,9> map{};
    for(unsigned slot=0;slot<9;++slot) map[slot]=static_cast<uint8_t>(view_of(slot));
    return map;
}
void CameraRig::select(safetyhook::Context& context,uint32_t capture_mask,uint32_t warmup_mask,uint64_t present_id) noexcept {
    auto config=configuration_.load();
    if(!config) return;
    if(!config->private_outputs) {
        // Native outputs: the views are engine slots. Clearing a game mirror
        // bit matches the engine's own off-screen skip; its texture keeps the
        // last image.
        const auto native=uint64_t(0x1FF&~config->mask);
        if(!config->native_mirrors && (context.r12&native)) {++native_suppressed_;context.r12&=~native;}
        for(auto bits=context.r12&native;bits;bits&=bits-1) ++native_applied_[std::countr_zero(bits)];
        context.r12=(context.r12&~uint64_t(config->mask))|(config->mask&capture_mask);
        std::array<uint8_t,9> identity{};
        for(unsigned slot=0;slot<9;++slot) identity[slot]=static_cast<uint8_t>(slot);
        current_map_.store(pack(identity));
        return;
    }
    current_map_.store(no_views);
    EngineSlots engine;
    if(!engine_slots(context.r14,engine)) {++unavailable_;last_render_mask_=0;return;}
    // Game mirrors keep the engine's choice; free slots render rig views only.
    const auto free=uint64_t(engine.free);
    free_slots_.store(engine.free);
    const auto native=context.r12&~free;
    context.r12&=~free;
    if(config->pause_without_mirrors && (!config->proxy_aa || *config->proxy_aa)) {
        if(!native) {++mirrorless_skips_;last_render_mask_=0;return;}
        // docs/28 §11.23: with the driver's head out of the cab the proxy
        // records no frame camera (no camera passes its |m[11]| > 2 test);
        // frames that still carry sensor views then cost ~0.5 s in its final
        // pass. Sensor views rest until 30 frames after the last such frame.
        // The proxy's frame number moves at Present on the render thread, so
        // compare with the previous selection: the frame moved on and the
        // record did not (2026-10-24: comparing the two values directly
        // misread most driving frames as record-less).
        if(config->proxy_record) {
            const auto record=*config->proxy_record,frame=*config->proxy_frame;
            if(record==seen_record_ && frame!=seen_frame_) recordless_at_=present_id;
            seen_record_=record;seen_frame_=frame;
        }
        if(present_id<recordless_at_.load()+30) {++recordless_skips_;last_render_mask_=0;return;}
    }
    if(!config->native_mirrors && native) {++native_suppressed_;context.r12&=~native;}
    for(auto bits=context.r12;bits;bits&=bits-1) ++native_applied_[std::countr_zero(bits)];
    if(!free) {++unavailable_;last_render_mask_=0;return;}
    if(in_flight()) {last_render_mask_=0;return;}
    // After an idle interval the engine needs one complete preparation render
    // of a view; only a following consecutive frame is eligible to capture.
    // A capture is the group rendered in the previous frame, on the same slots.
    // Groups: the views in slot order, as many at a time as there are free
    // slots. Taking turns needs the warm-up order even when it is off.
    std::array<unsigned,9> free_list{};unsigned free_count=0;
    for(auto bits=engine.free;bits;bits&=bits-1) free_list[free_count++]=std::countr_zero(bits);
    const auto views=config->mask;
    const auto capture=capture_mask&views;
    const bool warm=config->capture_warmup || static_cast<unsigned>(std::popcount(views))>free_count;
    const auto wanted=capture?capture:warm?warmup_mask&views:views;
    if(!wanted) {last_render_mask_=0;return;}
    // A stream capture or warm-up names one group (the lowest waiting); the
    // rig without a stream renders every view, so its groups rotate.
    uint32_t groups=0;unsigned index=0;
    for(auto bits=views;bits;bits&=bits-1,++index) if(wanted>>std::countr_zero(bits)&1) groups|=1u<<(index/free_count);
    unsigned group=std::countr_zero(groups);
    if(capture_mask==UINT32_MAX) for(auto turn=present_id%std::popcount(groups);turn--;) group=std::countr_zero(groups&=groups-1);
    std::array<uint8_t,9> map;map.fill(9);
    uint32_t render=0,physical=0;index=0;
    for(auto bits=views;bits;bits&=bits-1,++index) {
        const auto view=std::countr_zero(bits);
        if(index/free_count!=group || !(wanted>>view&1)) continue;
        const auto slot=free_list[index%free_count];
        map[slot]=static_cast<uint8_t>(view);render|=1u<<view;physical|=1u<<slot;
    }
    if(!prepare_private(engine,*config,map)) {++unavailable_;last_render_mask_=0;return;}
    prepared_configuration_.store(config);
    interior_=context.r14-0x13A8;
    // Register-only array redirection leaves the engine's owning arrays and
    // HUD aliases intact.
    context.r14=reinterpret_cast<uintptr_t>(&camera_array_);
    context.r12|=physical;
    selection_mask=physical;
    last_render_frame_=present_id;last_render_mask_=render;
    selection_owner=this;selection_config=config;++selections_;
    current_map_.store(pack(map));
}
bool CameraRig::engine_slots(uintptr_t cameras,EngineSlots& slots) noexcept {
    uintptr_t controller{};
    const auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if(!read_memory(cameras,slots.cameras) || slots.cameras.size!=9 ||
       !read_memory(base+0x36AF7C8,controller) || !controller ||
       !read_memory(controller+0xA8,slots.drawables) || slots.drawables.size!=9 ||
       !copy_memory(slots.cameras.data,slots.camera.data(),sizeof(slots.camera)) ||
       !copy_memory(slots.drawables.data,slots.drawable.data(),sizeof(slots.drawable))) return false;
    // A game mirror of the current truck has a drawable (2026-10-23 Scania S:
    // slots 0-6, six mirrors and the UI mirror).
    slots.free=0;
    for(unsigned slot=0;slot<9;++slot) if(!slots.drawable[slot]) slots.free|=1u<<slot;
    return true;
}
bool CameraRig::prepare_private(const EngineSlots& engine,const Configuration& config,const std::array<uint8_t,9>& map) noexcept {
    auto sources=engine.camera,drawables=engine.drawable;
    const auto& original_sources=engine.camera;const auto& original_drawables=engine.drawable;
    for(unsigned slot=0;slot<9;++slot) if(map[slot]<9) {
        const unsigned i=map[slot];
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
        sources[slot]=reinterpret_cast<uintptr_t>(view.camera.data());
        drawables[slot]=reinterpret_cast<uintptr_t>(view.drawable.data());
    }
    camera_pointers_=sources;drawable_pointers_=drawables;
    camera_array_=engine.cameras;camera_array_.data=reinterpret_cast<uintptr_t>(camera_pointers_.data());
    drawable_array_=engine.drawables;drawable_array_.data=reinterpret_cast<uintptr_t>(drawable_pointers_.data());
    private_ready_=true;
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
    // rbp is the engine slot; private views map it to the rig view.
    const auto engine_slot=static_cast<uint32_t>(context.rbp);
    if(!config || engine_slot>=9) return;
    if(config->private_outputs && selection_owner!=this) return;
    const auto slot=config->private_outputs?view_of(engine_slot):engine_slot;
    if(slot>=9 || !config->views[slot].enabled) return;
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
       convert!=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))+0x8807a0 ||
       !copy_memory(context.rbx,submission.bytes.data(),submission.bytes.size())) {++unavailable_;return;}
    const auto cx=std::floor(position[0]/512.0),cz=std::floor(position[2]/512.0);
    if(cx<-32768 || cx>32767 || cz<-32768 || cz>32767) {++unavailable_;return;}
    // 0x8807A0 composes the native quaternion with a Z half-turn, then the
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
       caller!=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))+0x4d47d8) return;
    const auto config=configuration_.load();
    if(!config) return;
    const auto engine_slot=static_cast<uint32_t>(context.rdi);
    const auto slot=config->private_outputs?view_of(engine_slot):engine_slot;
    if(slot>=9 || !config->views[slot].enabled) return;
    const auto& size=config->views[slot].resolution;
    if(size[0]) {context.rdx=size[0];context.r8=size[1];}
}
void CameraRig::ego_parts(safetyhook::Context& context) noexcept {
    uintptr_t caller{};
    static const auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if(!read_memory(context.rsp+0x48,caller) || (caller!=base+0x647129 && caller!=base+0x6471B7)) return;
    const auto config=configuration_.load();
    if(!config || !config->ego_full_model) return;
    uintptr_t mask{},controller{},actor{},vehicle{};
    constexpr std::array<uint64_t,9> masks={0x400,0x800,0x1000,0x2000,0x4000,0x8000,0x10000,0x0800000000000000,0x1000000000000000};
    // Model part masks are per engine slot: the free slots private views use.
    const auto slots=config->private_outputs?free_slots_.load():config->mask;
    uint64_t sensor_mask=0;for(unsigned i=0;i<9;++i) if(slots&(1u<<i)) sensor_mask|=masks[i];
    // A3CADE follows the cached-model "all parts" test. Limit the override to
    // the two body model calls made by the player's 646C00 submission, and to
    // camera slots owned by this rig. Other models and normal mirrors keep
    // their native per-mirror subsets. No model/camera memory is changed.
    if(!read_memory(context.r8+0x18,mask) || !(mask&sensor_mask) ||
       !read_memory(base+0x36AF7C8,controller) || !controller ||
       !read_memory(controller+0x31B0,actor) || !actor ||
       !read_memory(actor+0x18,vehicle) || context.rsi!=vehicle) return;
    context.rflags&=~uintptr_t{0x40}; // ZF=0: use the engine's full-list path.
    ++ego_parts_applied_;
}
}
