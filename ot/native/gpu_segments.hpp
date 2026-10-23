#pragma once
#include "ot.hpp"
#include "render_sample.hpp"
#include <d3d11.h>
#include <wrl/client.h>
#include <map>
#include <unordered_map>

namespace ot {
// docs/28 §11.23: GPU time between consecutive game render-target binds,
// named by what the opening bind draws into (a main pass, a sensor view, or
// the target size). Work the snowymoon proxy adds after a game bind counts
// toward that bind. Timestamp queries on the render thread's immediate
// context; off unless started.
class GpuSegments {
public:
    // Control thread.
    json control(const json& request);
    bool enabled() const noexcept {return enabled_.load(std::memory_order_relaxed);}
    // Render thread, at each game bind; render_frame changes at Present.
    void bind(ID3D11DeviceContext* context,uint64_t render_frame,uint32_t count,const uintptr_t* targets,
              uintptr_t depth,const RenderPassPtr& pass) noexcept;
private:
    template<class T> using Com=Microsoft::WRL::ComPtr<T>;
    static constexpr size_t frames=4,stamps=1024;
    struct Frame {
        Com<ID3D11Query> disjoint;
        std::vector<Com<ID3D11Query>> stamps;
        std::vector<std::string> labels;
        uint64_t render_frame=0;
        size_t used=0;
        bool open=false,pending=false;
    };
    void close(ID3D11DeviceContext* context,Frame& frame) noexcept;
    void resolve(ID3D11DeviceContext* context) noexcept;
    std::string label(uint32_t count,const uintptr_t* targets,uintptr_t depth,const RenderPassPtr& pass);
    std::atomic<bool> enabled_{false},clear_{false};
    // Render thread only.
    std::array<Frame,frames> frames_;
    size_t current_=0;
    std::unordered_map<uintptr_t,std::string> sizes_;
    Com<ID3D11Device> device_;
    // Shared with the control thread.
    std::mutex mutex_;
    struct Total {double ms=0,max_frame_ms=0;uint64_t segments=0;};
    std::map<std::string,Total> totals_;
    uint64_t resolved_=0,disjoint_=0,overflow_=0,skipped_=0;
    double frame_ms_=0,worst_ms_=0;
    // The slowest frame's segments in bind order.
    std::vector<std::pair<std::string,double>> worst_;
};
}
