#pragma once
#include "ot.hpp"
#include "gpu_capture.hpp"
#include "capture_stream.hpp"
#include "pass_commands.hpp"
#include "camera_rig.hpp"
#include <array>
#include <safetyhook.hpp>

namespace ot {
class RenderProbe {
public:
    RenderProbe()=default;
    void enable(bool vehicle_metadata=false,const std::string& mode="observe",bool frame_timing=false,bool draw_metadata=true);
    void disable() noexcept;
    // On failure the caller must retain this object and its module reference.
    // Code may still be returning through the hook; destruction would be unsafe.
    bool close() noexcept;
    void sdk_frame(std::shared_ptr<const json> state) noexcept { sdk_frame_.store(state->at("frame_id").get<uint64_t>()); sdk_state_.store(std::move(state)); }
    json status();
    json frames(uint64_t after_id=0);
    json capture(const std::string& action,const CaptureOptions& options={});
    json capture_views(const std::string& action,Transport* publisher=nullptr,bool metadata=true,const CaptureOptions& options={});
    json camera_rig(const json& request);
    json stream(const json& request,Transport& publisher,const CaptureOptions& options={});
    ~RenderProbe();
private:
    struct Record {
        uint64_t sequence{},sdk_frame_hint{},render_frame{};
        DWORD thread{};
        uint32_t count{};
        uintptr_t context{},depth{};
        std::array<uintptr_t,8> targets{};
        uint16_t compiled_id{};
        uintptr_t token{};
        RenderPassPtr pass;
    };
    static void callback(safetyhook::Context& context) noexcept;
    static void present_callback(safetyhook::Context& context) noexcept;
    static void compile_begin_callback(safetyhook::Context& context) noexcept;
    static void compile_end_callback(safetyhook::Context& context) noexcept;
    static void draw_batch_callback(safetyhook::Context& context) noexcept;
    static void rig_select_callback(safetyhook::Context& context) noexcept;
    static void rig_begin_callback(safetyhook::Context& context) noexcept;
    static void rig_end_callback(safetyhook::Context& context) noexcept;
    static void rig_dimensions_callback(safetyhook::Context& context) noexcept;
    static void rig_ego_parts_callback(safetyhook::Context& context) noexcept;
    static void rig_drawables_callback(safetyhook::Context& context) noexcept;
    static void rig_graph_drawables_callback(safetyhook::Context& context) noexcept;
    static void rig_graph_cameras_callback(safetyhook::Context& context) noexcept;
    static void rig_graph_end_callback(safetyhook::Context& context) noexcept;
    struct Timing {
        std::atomic<uint64_t> count{0},total{0},maximum{0};
        // Bucket 0 includes 0 and 1 tick; bucket k contains [2^k, 2^(k+1)).
        std::array<std::atomic<uint64_t>,64> buckets{};
        void add(uint64_t ticks) noexcept;
        json snapshot() const;
    };
    Timing bind_timing_,present_timing_,compile_begin_timing_,compile_end_timing_;
    Timing draw_batch_timing_;
    void present(HRESULT result) noexcept;
    void observe(const safetyhook::Context& context) noexcept;
    bool compile_frame() const noexcept;
    bool quiescent() noexcept;
    bool disable_locked(bool clear_rig);
    std::mutex control_;
    SRWLOCK records_lock_=SRWLOCK_INIT;
    std::array<Record,128> records_{};
    uint64_t records_written_=0;
    std::atomic<uint64_t> calls_{0},missed_{0},sdk_frame_{0};
    std::atomic<bool> accepting_{false};
    std::atomic<bool> observing_{false};
    std::atomic<bool> vehicle_metadata_{false};
    safetyhook::MidHook hook_;
    safetyhook::MidHook present_hook_;
    safetyhook::MidHook compile_begin_hook_,compile_end_hook_;
    safetyhook::MidHook draw_batch_hook_;
    safetyhook::MidHook rig_select_hook_,rig_begin_hook_,rig_end_hook_,rig_dimensions_hook_;
    safetyhook::MidHook rig_ego_parts_hook_;
    safetyhook::MidHook rig_drawables_hook_,rig_graph_drawables_hook_,rig_graph_cameras_hook_,rig_graph_end_hook_;
    CameraRig rig_;
    std::atomic<std::shared_ptr<CaptureStream>> stream_;
    std::array<safetyhook::MidHook*,14> hookset() noexcept {
        return {&hook_,&present_hook_,&compile_begin_hook_,&compile_end_hook_,&draw_batch_hook_,
            &rig_select_hook_,&rig_begin_hook_,&rig_end_hook_,&rig_dimensions_hook_,&rig_ego_parts_hook_,
            &rig_drawables_hook_,&rig_graph_drawables_hook_,&rig_graph_cameras_hook_,&rig_graph_end_hook_};
    }
    PassCommands pass_commands_;
    std::atomic<std::shared_ptr<const json>> sdk_state_;
    struct PresentRecord { uint64_t id,qpc,sdk_frame;HRESULT result;DWORD thread;bool game_foreground; };
    SRWLOCK frames_lock_=SRWLOCK_INIT;
    std::array<PresentRecord,600> frames_{};
    uint64_t frames_written_=0;
    std::atomic<uint64_t> presents_{0},missed_frames_{0};
    std::atomic<bool> frame_boundary_seen_{false};
    const uint64_t observation_session_=qpc_now();
    HMODULE module_reference_{};
    uintptr_t module_begin_{},module_end_{};
    std::string last_error_;
    std::string mode_="off";
    GpuCapture gpu_{"mirror5"},gpu0_{"mirror0"},gpu1_{"mirror1"},gpu2_{"mirror2"},gpu3_{"mirror3"},gpu4_{"mirror4"};
    GpuCapture gpu6_{"mirror6"},gpu7_{"mirror7"},gpu8_{"mirror8"};
    uint64_t bundle_frame_=0;
    uint32_t bundle_mask_=0x27;
    json published_bundle_;
    std::array<GpuCapture*,9> cameras() noexcept {return {&gpu0_,&gpu1_,&gpu2_,&gpu3_,&gpu4_,&gpu_,&gpu6_,&gpu7_,&gpu8_};}
    std::vector<GpuCapture*> capture_cameras() {
        std::vector<GpuCapture*> selected;const auto all=cameras();
        for(size_t i=0;i<all.size();++i) if(bundle_mask_&(1u<<i)) selected.push_back(all[i]);
        return selected;
    }
};
}
