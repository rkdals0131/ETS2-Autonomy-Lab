#pragma once
#include "render_sample.hpp"
#include <memory>
#include <unordered_map>

namespace ot {
// Names travel with compiled command ranges, independently of reused textures.
class PassCommands {
public:
    void draw_batch(uintptr_t input,uintptr_t items,uintptr_t bindings,uint32_t count) noexcept;
    void begin(uintptr_t frame,uintptr_t input,uintptr_t output,uint16_t id,uint32_t vehicle_mask=0,std::shared_ptr<const json> sdk={},uint32_t camera_mask=UINT32_MAX,bool diagnostic=true) noexcept;
    void end(uintptr_t frame) noexcept;
    RenderPassPtr lookup(uint16_t id,uintptr_t token) noexcept;
    void clear();
    json status();
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
    RenderPassPtr describe(uintptr_t input,uint32_t vehicle_mask,const std::shared_ptr<const json>& sdk,uint32_t camera_mask,bool diagnostic);
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
