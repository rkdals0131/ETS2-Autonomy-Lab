#pragma once
#include "ot.hpp"
#include "gpu_capture.hpp"
#include "capture_stream.hpp"
#include "pass_commands.hpp"
#include "camera_rig.hpp"
#include "overlay_decal.hpp"
#include <array>
#include <safetyhook.hpp>

namespace ot {
class RenderProbe {
public:
    RenderProbe()=default;
    // mode overlay: only the OM and compile hooks, for the HUD decal (docs/24 §7);
    // observe with overlay=true shares the observation hooks with the decal.
    void enable(bool vehicle_metadata=false,const std::string& mode="observe",bool frame_timing=false,bool draw_metadata=true,bool overlay=false);
    void disable() noexcept;
    // On failure the caller must retain this object and its module reference.
    // Code may still be returning through the hook; destruction would be unsafe.
    bool close() noexcept;
    void sdk_frame(std::shared_ptr<const json> state) noexcept {
        sdk_frame_.store(state->at("frame_id").get<uint64_t>());
        sdk_time_us_.store(state->at("paused_simulation_time_us").get<uint64_t>(),std::memory_order_relaxed);
        sdk_state_.store(std::move(state));
    }
    // HUD decal (docs/24): the block must stay mapped while the probe exists.
    json overlay(bool enabled,OverlayBlock* block,const OverlayOptions& options);
    // Map the block before enable(..., overlay=true); keeps the current options.
    void overlay_mapping(OverlayBlock* block) {std::lock_guard lock(control_);overlay_block_=block;}
    json overlay_status();
    json overlay_capture(const json& request) {return decal_.capture(request);}
    bool overlay_enabled() const noexcept {return overlay_.load();}
    json status();
    // Patched code of the enabled hooks, for the parallel_passes text gate.
    std::vector<std::pair<uintptr_t,uintptr_t>> patched_ranges() {
        std::lock_guard lock(control_);
        std::vector<std::pair<uintptr_t,uintptr_t>> ranges;
        for(auto* hook:hookset()) if(*hook && hook->enabled())
            ranges.push_back({hook->target_address(),hook->target_address()+hook->original_bytes().size()});
        return ranges;
    }
    json frames(uint64_t after_id=0);
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
    void overlay_observe(const safetyhook::Context& context) noexcept;
    bool backbuffer_usage(uintptr_t view) noexcept;
    void enable_locked(bool vehicle_metadata,const std::string& mode,bool frame_timing,bool draw_metadata,bool overlay);
    void set_overlay_locked(bool enabled);
    json overlay_json();
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
    // HUD decal. overlay_ gates the render-thread path; overlay_only_ is mode overlay.
    std::atomic<bool> overlay_{false},overlay_only_{false};
    std::atomic<uint64_t> sdk_time_us_{0};
    OverlayBlock* overlay_block_=nullptr;
    OverlayOptions overlay_options_;
    OverlayDecal decal_;
    Timing overlay_timing_;
    // Render thread only; released after the hooks drained.
    template<class T> using Com=Microsoft::WRL::ComPtr<T>;
    std::array<Com<ID3D11RenderTargetView>,3> composition_;
    std::array<Com<ID3D11DepthStencilView>,3> main_depth_;
    int armed_graph_=-1; // set by a composition bind, consumed by the next single-target bind
    std::atomic<bool> overlay_reset_{false}; // pipe thread asks the render thread to drop the state below
    struct UsageEntry {uintptr_t view;bool backbuffer;};
    std::array<UsageEntry,64> usage_cache_{};
    size_t usage_next_=0;
    std::atomic<uint64_t> scene_binds_{0},composition_binds_{0},blit_compiled_span_{0},blit_backbuffer_usage_{0},
        blit_both_{0},blit_neither_{0},blit_draws_{0},blit_no_camera_{0};

};
}
