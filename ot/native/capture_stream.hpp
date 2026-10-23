#pragma once
#include "gpu_capture.hpp"

namespace ot {
// Three independently reusable GPU readback bundles. Only observe() touches
// the immediate context; the worker schedules frames and publishes CPU bytes.
// A bundle's views may be selected in parts, one rig group a frame (docs/28
// §11.22); the oldest bundle with unselected views goes first.
class CaptureStream {
public:
    CaptureStream(uint32_t mask,const CaptureOptions& options,double hz,double duration,
                  const std::atomic<uint64_t>& presents,uint64_t session,Transport& publisher);
    ~CaptureStream();
    void start();
    void stop() noexcept;
    void update(const CaptureOptions& options) {pending_options_.store(std::make_shared<const CaptureOptions>(options));}
    bool running() const noexcept {return running_.load();}
    uint32_t select_pending(uint32_t ready_mask=UINT32_MAX) noexcept;
    uint32_t unselected_mask() const noexcept;
    uint32_t compiling_mask(bool vehicles_only=false) const noexcept;
    bool compiling() const noexcept;
    bool compiling_diagnostics() const noexcept;
    bool pending() const noexcept;
    json status();
    void observe(ID3D11DeviceContext* context,uint32_t count,const uintptr_t* targets,
                 uint64_t sequence,uint64_t sdk_frame,uint64_t render_frame,const RenderPassPtr& pass) noexcept;
private:
    struct Slot {
        std::atomic<bool> active{false};
        std::atomic<bool> selected{false};   // some view selected
        std::atomic<uint32_t> chosen{0};      // the views selected so far
        std::atomic<uint64_t> sequence{0};    // arming order
        std::atomic<uint64_t> frame{0};
        std::atomic<uint32_t> mask{0};
        std::atomic<uint32_t> vehicle_mask{0};
        std::atomic<bool> diagnostic{true};
        std::atomic<uint64_t> armed_at{0};  // GetTickCount64
        std::vector<std::unique_ptr<GpuCapture>> cameras;
    };
    void run() noexcept;
    void finish_slot(Slot& slot);
    std::array<Slot,3> slots_;
    std::array<std::shared_ptr<ExposureState>,9> exposure_;
    CaptureOptions options_;
    std::atomic<std::shared_ptr<const CaptureOptions>> pending_options_;
    std::vector<unsigned> camera_indices_;
    json names_=json::array();
    double hz_,duration_;
    const std::atomic<uint64_t>& presents_;
    const uint64_t session_,id_=qpc_now();
    Transport& publisher_;
    Handle stop_event_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> armed_{0},completed_{0},published_{0},failed_{0},canceled_{0};
    std::atomic<uint64_t> ring_busy_{0},same_frame_{0},queue_dropped_{0},stalled_{0},ended_{0};
    std::atomic_flag selecting_=ATOMIC_FLAG_INIT;
    std::atomic<uint64_t> selected_{0},spanned_{0},sequence_{0};
    Slot* oldest_unselected() noexcept;
    const Slot* oldest_unselected() const noexcept;
    std::mutex result_mutex_;
    std::string error_,reason_;
};
}
