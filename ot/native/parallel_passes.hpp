#pragma once
#include "ot.hpp"
#include "pass_stream.hpp"
#include "hook_support.hpp"
#include <array>
#include <string>
#include <unordered_map>
#include <safetyhook.hpp>

namespace ot {
// docs/28 M3a: type 1 render passes are deferred from 0x226960 and recorded at
// the end of the pass loop in 0x227140 (0x227B1F), still on the main thread
// and in the original order. Recording a pass twice is not possible: pass
// callbacks consume per-pass state (0x151BF80 asserts on an empty blit pair
// array the second time), so there is no same-frame shadow recording.
class ParallelPasses {
public:
    enum class Mode : int {off=0,defer=1};
    // docs/28 §5 environment gate inputs: hooks of this DLL (absolute ranges),
    // patches the user accepted (RVA, size) and the recording closure (RVAs).
    struct GateInputs {
        CodeRanges ours;
        std::vector<std::pair<uintptr_t,size_t>> accepted;
        std::vector<uintptr_t> closure;
    };
    ParallelPasses()=default;
    void enable(Mode mode,const GateInputs& gate);
    // Returns to the original path at a frame boundary, then disables hooks.
    void disable() noexcept;
    // On failure the caller must retain this object and its module reference.
    bool close() noexcept;
    void sdk_frame(DWORD main_thread,bool paused) noexcept {main_thread_.store(main_thread);paused_.store(paused);}
    json status();
    bool active() const noexcept {return requested_.load()!=0;}
    ~ParallelPasses();
private:
    using Execute=void(*)(uintptr_t,uintptr_t,uintptr_t,void*);
    using Allocate=uint32_t*(*)(passes::CmdStorage*,uint64_t);
    struct Deferred {uintptr_t builder,value,pass;passes::Splice splice;};
    struct Timing {
        uint64_t count=0,total=0,maximum=0;
        void add(uint64_t ticks) noexcept {++count;total+=ticks;if(ticks>maximum) maximum=ticks;}
        json snapshot() const {return {{"count",count},{"total_ticks",total},{"max_ticks",maximum}};}
    };
    struct KeyStats {
        std::string name,space,last_raw;
        uint64_t passes=0,ticks=0,max_ticks=0,dwords=0,raw_changes=0;
    };
    struct Counters {
        uint64_t frames_off=0,frames_defer=0,deferred=0,marker=0,overflow=0;
        uint64_t splice_used=0,splice_fallback=0,orphaned=0;
        std::array<uint64_t,4> chunks{};      // storage records at deferral: 0,1,2,3+
        std::array<uint64_t,3> front{},back{}; // dwords before/after the body: 0, 4, other
        uint64_t frame_dwords=0,frame_ticks=0,largest_ticks=0,frames_measured=0;
        Timing join;
    };
    static void execute_detour(uintptr_t builder,uintptr_t value,uintptr_t pass,void* marker);
    static void join_callback(safetyhook::Context& context) noexcept;
    bool defer(uintptr_t builder,uintptr_t value,uintptr_t pass,void* marker) noexcept;
    void join() noexcept;
    void latch() noexcept;
    void record(const Deferred& item,uint64_t& frame_dwords,uint64_t& largest);
    void turn_off(const std::string& reason) noexcept;
    bool device_matches() const noexcept;
    uint64_t pass_key(uintptr_t pass,KeyStats& seen) const;
    bool quiescent() noexcept;
    json scan_text(const GateInputs& gate);
    void watch_code(const GateInputs& gate);
    bool code_unchanged() noexcept;

    std::mutex control_;
    safetyhook::InlineHook execute_hook_;
    safetyhook::MidHook join_hook_;
    std::atomic<Execute> original_{};
    Allocate allocate_{};
    uintptr_t base_{},device_{},device_vtable_{};
    HMODULE module_reference_{};
    uintptr_t module_begin_{},module_end_{};
    std::atomic<int> requested_{0};
    std::atomic<bool> frame_active_{false}; // a frame latched a non-off mode and has not joined
    std::atomic<DWORD> main_thread_{0};
    std::atomic<bool> paused_{true};
    // Main thread only (the deferral hook and the join both run there).
    Mode frame_mode_=Mode::off;
    bool latched_=false,joining_=false;
    uint64_t latch_qpc_=0;
    uint32_t overflow_frames_=0;
    std::atomic<uint64_t> watch_qpc_{0};
    // First 32 bytes of the closure functions and of our hook sites at enable.
    std::vector<std::pair<uintptr_t,std::array<uint8_t,32>>> watched_;
    std::array<Deferred,1024> queue_{};
    size_t queued_=0;
    std::vector<uint32_t> tail_;
    std::unordered_map<uint64_t,uint32_t> occurrences_;
    struct FrameKey {uint64_t key;uint64_t ticks,dwords;std::string raw;};
    std::vector<FrameKey> frame_keys_;
    // Shared with status().
    std::mutex stats_mutex_;
    Counters counters_;
    std::unordered_map<uint64_t,KeyStats> keys_;
    std::string reason_,error_;
    json gate_=nullptr;
    bool workers_allowed_=false;
};
}
