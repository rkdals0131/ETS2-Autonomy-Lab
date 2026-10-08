#pragma once
#include "ot.hpp"
#include "gpu_capture.hpp"
#include "pass_commands.hpp"
#include <array>
#include <safetyhook.hpp>

namespace ot {
class RenderProbe {
public:
    RenderProbe()=default;
    void enable();
    void disable() noexcept;
    // On failure the caller must retain this object and its module reference.
    // Code may still be returning through the hook; destruction would be unsafe.
    bool close() noexcept;
    void sdk_frame(uint64_t frame) noexcept { sdk_frame_.store(frame, std::memory_order_relaxed); }
    json status();
    json frames(uint64_t after_id=0);
    json capture(const std::string& action);
    json capture_views(const std::string& action);
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
        std::shared_ptr<const json> pass;
    };
    static void callback(safetyhook::Context& context) noexcept;
    static void present_callback(safetyhook::Context& context) noexcept;
    static void compile_begin_callback(safetyhook::Context& context) noexcept;
    static void compile_end_callback(safetyhook::Context& context) noexcept;
    void present(HRESULT result) noexcept;
    void observe(const safetyhook::Context& context) noexcept;
    bool quiescent() noexcept;
    std::mutex control_;
    SRWLOCK records_lock_=SRWLOCK_INIT;
    std::array<Record,128> records_{};
    uint64_t records_written_=0;
    std::atomic<uint64_t> calls_{0},missed_{0},sdk_frame_{0};
    std::atomic<bool> accepting_{false};
    safetyhook::MidHook hook_;
    safetyhook::MidHook present_hook_;
    safetyhook::MidHook compile_begin_hook_,compile_end_hook_;
    std::array<safetyhook::MidHook*,4> hookset() noexcept {
        return {&hook_,&present_hook_,&compile_begin_hook_,&compile_end_hook_};
    }
    PassCommands pass_commands_;
    struct PresentRecord { uint64_t id,qpc,sdk_frame;HRESULT result;DWORD thread; };
    SRWLOCK frames_lock_=SRWLOCK_INIT;
    std::array<PresentRecord,600> frames_{};
    uint64_t frames_written_=0;
    std::atomic<uint64_t> presents_{0},missed_frames_{0};
    std::atomic<bool> frame_boundary_seen_{false};
    const uint64_t observation_session_=qpc_now();
    HMODULE module_reference_{};
    uintptr_t module_begin_{},module_end_{};
    std::string last_error_;
    GpuCapture gpu_{"mirror5"},gpu0_{"mirror0"},gpu1_{"mirror1"},gpu2_{"mirror2"};
    uint64_t bundle_frame_=0;
    std::array<GpuCapture*,4> cameras() noexcept {return {&gpu0_,&gpu1_,&gpu2_,&gpu_};}
};
}
