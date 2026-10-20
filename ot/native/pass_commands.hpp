#pragma once
#include "render_sample.hpp"
#include <array>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace ot {
// Names travel with compiled command ranges, independently of reused textures.
class PassCommands {
public:
    void draw_batch(uintptr_t input,uintptr_t items,uintptr_t bindings,uint32_t count) noexcept;
    // main: also name the main0 scene/composition passes and blit_to_target
    // (camera only; docs/24 §3). camera_mask 0 skips every mirror.
    void begin(uintptr_t frame,uintptr_t input,uintptr_t output,uint16_t id,uint32_t vehicle_mask=0,std::shared_ptr<const json> sdk={},uint32_t camera_mask=UINT32_MAX,bool diagnostic=true,bool main=false) noexcept;
    void end(uintptr_t frame) noexcept;
    RenderPassPtr lookup(uint16_t id,uintptr_t token) noexcept;
    // Render thread, on binds while the decal is on: only the few main spans,
    // under a shared lock and without the lookup() counters.
    RenderPassPtr main_at(uint16_t id,uintptr_t token) noexcept;
    // Latest exterior scene pass compiled into graph buffer 0-2.
    RenderPassPtr main_scene(unsigned graph) const noexcept {return graph<main_scene_.size()?main_scene_[graph].load():nullptr;}
    void clear();
    json status();
    json main_status();
private:
    struct Block {uintptr_t data;uint64_t size,capacity;};
    struct Span {uintptr_t begin,end;RenderPassPtr pass;};
    struct Pending {
        uintptr_t output;
        uint16_t id;
        std::vector<Block> before;
        RenderPassPtr pass;
    };
    static std::vector<Block> blocks(uintptr_t output);
    RenderPassPtr describe(uintptr_t input,uint32_t vehicle_mask,const std::shared_ptr<const json>& sdk,uint32_t camera_mask,bool diagnostic,bool main);
    void retire_main(uint16_t id);
    struct MainSpan {uint16_t id;uintptr_t begin,end;RenderPassPtr pass;};
    SRWLOCK main_lock_=SRWLOCK_INIT;
    std::vector<MainSpan> main_spans_;
    std::unordered_set<uint16_t> main_ids_; // guarded by mutex_
    std::array<std::atomic<std::shared_ptr<const RenderPassSample>>,3> main_scene_;
    std::array<std::atomic<uint64_t>,4> main_compiled_{}; // scene, composition, blit, scene camera errors
    using Draw=RenderDrawSample;
    struct DrawBatch {std::vector<Draw> draws;bool truncated=false;std::string error;};
    std::mutex draws_mutex_;
    std::unordered_map<uintptr_t,DrawBatch> draws_;
    std::mutex mutex_;
    std::unordered_map<uintptr_t,Pending> pending_;
    std::unordered_map<uint16_t,std::vector<Span>> compiled_;
    uint64_t inputs_=0,named_=0,matches_=0,unmatched_=0,errors_=0;
    std::string error_;
};
}
