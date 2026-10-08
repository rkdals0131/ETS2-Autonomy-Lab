#pragma once
#include "ot.hpp"
#include <memory>
#include <unordered_map>

namespace ot {
// Names travel with compiled command ranges, independently of reused textures.
class PassCommands {
public:
    void draw_batch(uintptr_t input,uintptr_t items,uintptr_t bindings,uint32_t count) noexcept;
    void begin(uintptr_t frame,uintptr_t input,uintptr_t output,uint16_t id,bool vehicles=false,std::shared_ptr<const json> sdk={}) noexcept;
    void end(uintptr_t frame) noexcept;
    std::shared_ptr<const json> lookup(uint16_t id,uintptr_t token) noexcept;
    void clear();
    json status();
private:
    struct Block {uintptr_t data;uint64_t size,capacity;};
    struct Span {uintptr_t begin,end;std::shared_ptr<const json> pass;};
    struct Pending {
        uintptr_t output;
        uint16_t id;
        std::vector<Block> before;
        std::shared_ptr<const json> pass;
    };
    static std::vector<Block> blocks(uintptr_t output);
    std::shared_ptr<const json> describe(uintptr_t input,bool vehicles,const std::shared_ptr<const json>& sdk);
    struct Draw {
        uintptr_t geometry{},buffer{};
        uint32_t first{},count{};
        bool known=false;
        uint64_t qpc{};
    };
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
