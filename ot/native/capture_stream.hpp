#pragma once
#include "gpu_capture.hpp"

namespace ot {
// Three independently reusable GPU readback bundles. Only observe() touches
// the immediate context; the worker schedules frames and publishes CPU bytes.
class CaptureStream {
public:
    CaptureStream(uint32_t mask,const CaptureOptions& options,double hz,double duration,
                  const std::atomic<uint64_t>& presents,uint64_t session,Transport& publisher);
    ~CaptureStream();
    void start();
    void stop() noexcept;
    void update(const CaptureOptions& options) {pending_options_.store(std::make_shared<const CaptureOptions>(options));}
    bool running() const noexcept {return running_.load();}
    uint32_t frame_mask(uint64_t frame) const noexcept;
    bool pending() const noexcept;
    json status();
    void observe(ID3D11DeviceContext* context,uint32_t count,const uintptr_t* targets,
                 uint64_t sequence,uint64_t sdk_frame,uint64_t render_frame,const json* pass) noexcept;
private:
    struct Slot {
        std::atomic<bool> active{false};
        std::atomic<uint64_t> frame{0};
        std::atomic<uint32_t> mask{0};
        std::vector<std::unique_ptr<GpuCapture>> cameras;
    };
    void run() noexcept;
    void finish_slot(Slot& slot);
    std::array<Slot,3> slots_;
    std::array<std::shared_ptr<ExposureState>,6> exposure_;
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
    std::atomic<uint64_t> ring_busy_{0},same_frame_{0},queue_dropped_{0},ended_{0};
    std::mutex result_mutex_;
    std::string error_,reason_;
};
}
