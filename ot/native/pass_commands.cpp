#include "pass_commands.hpp"
#include <algorithm>
#include <stdexcept>

namespace ot {
namespace {
struct Array {uintptr_t vtable,data;uint64_t size,capacity;};
template<class T> T read(uintptr_t address) {
    T value{};
    if(!read_memory(address,value)) throw std::runtime_error("Cannot read render command metadata");
    return value;
}
Array array(uintptr_t address) {
    auto value=read<Array>(address);
    if(value.size>value.capacity || (value.size && !value.data))
        throw std::runtime_error("Render command array changed or is invalid");
    return value;
}
std::string string_at(uintptr_t address) {
    std::array<char,384> value{};
    if(!address || !copy_memory(address,value.data(),value.size())) return {};
    auto end=std::find(value.begin(),value.end(),'\0');
    return end==value.end()?std::string{}:std::string(value.begin(),end);
}
}
std::vector<PassCommands::Block> PassCommands::blocks(uintptr_t output) {
    const auto header=array(output+0x18);
    std::vector<Block> result(header.size);
    if(header.size && !copy_memory(header.data,result.data(),result.size()*sizeof(Block)))
        throw std::runtime_error("Cannot read compiled command blocks");
    for(const auto& block:result)
        if(block.size>block.capacity || (block.size && !block.data))
            throw std::runtime_error("Compiled command block changed or is invalid");
    return result;
}
std::shared_ptr<const json> PassCommands::describe(uintptr_t input) {
    const auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    for(unsigned b=0;b<3;++b) {
        const auto manager=base+0x304FC50+b*0x2A8;
        const auto passes=array(manager+0xF0);
        for(uint64_t i=0;i<passes.size;++i) {
            const auto pass=read<uintptr_t>(passes.data+i*8);
            if(!pass || pass+0x2E8!=input) continue;
            json result={{"pass_address",pass},{"command_buffer",input},{"graph_buffer",b},
                {"pass_name",string_at(read<uintptr_t>(pass+0x20))},
                {"pass_namespace",string_at(read<uintptr_t>(pass+0xC0))}};
            const auto images=array(manager);
            json links=json::array();
            for(const auto offset:{0x658,0x6C0}) {
                const auto refs=array(pass+offset);
                for(uint64_t j=0;j<refs.size;++j) {
                    const auto id=read<uint32_t>(refs.data+j*8);
                    if(id>=images.size) continue;
                    const auto image=images.data+id*0x7F0;
                    links.push_back({{"reference_array_offset",offset},{"graph_image_id",id},
                        {"namespace",string_at(read<uintptr_t>(image+0xA8))},
                        {"name",string_at(read<uintptr_t>(image+8))},
                        {"pool_id_at_compile",read<uint16_t>(image+0x740)}});
                }
            }
            result["linked_images"]=std::move(links);
            return std::make_shared<const json>(std::move(result));
        }
    }
    return {};
}
void PassCommands::begin(uintptr_t frame,uintptr_t input,uintptr_t output,uint16_t id) noexcept {
    try {
        auto before=blocks(output);
        const bool empty=std::all_of(before.begin(),before.end(),[](const Block& b){return b.size==0;});
        auto pass=describe(input);
        std::lock_guard lock(mutex_);
        ++inputs_;if(pass) ++named_;
        // The engine resets an allocated compiled buffer before filling it.
        // Its first empty boundary retires the previous use of this pool ID.
        if(empty) compiled_.erase(id);
        pending_.insert_or_assign(frame,Pending{output,id,std::move(before),std::move(pass)});
    } catch(const std::exception& e) {
        std::lock_guard lock(mutex_);++errors_;error_=e.what();
        pending_.erase(frame);compiled_.erase(id);
    }
}
void PassCommands::end(uintptr_t frame) noexcept {
    try {
        std::lock_guard lock(mutex_);
        auto found=pending_.find(frame);
        if(found==pending_.end()) return;
        auto pending=std::move(found->second);pending_.erase(found);
        const auto after=blocks(pending.output);
        auto& spans=compiled_[pending.id];
        for(const auto& block:after) {
            const auto old=std::find_if(pending.before.begin(),pending.before.end(),
                [&](const Block& b){return b.data==block.data;});
            const auto first=old==pending.before.end()?0:old->size;
            if(first>block.size) throw std::runtime_error("Compiled command cursor moved backwards");
            if(first<block.size) spans.push_back({block.data+first*4,block.data+block.size*4,pending.pass});
        }
    } catch(const std::exception& e) {
        std::lock_guard lock(mutex_);++errors_;error_=e.what();
        // A failed compilation observation must not leave partial labels usable.
        compiled_.clear();pending_.erase(frame);
    }
}
std::shared_ptr<const json> PassCommands::lookup(uint16_t id,uintptr_t token) noexcept {
    std::lock_guard lock(mutex_);
    const auto found=compiled_.find(id);
    if(found!=compiled_.end())
        for(const auto& span:found->second)
            if(token>=span.begin && token<span.end) {
                if(span.pass) ++matches_;else ++unmatched_;
                return span.pass;
            }
    ++unmatched_;return {};
}
void PassCommands::clear() {
    std::lock_guard lock(mutex_);pending_.clear();compiled_.clear();
    inputs_=named_=matches_=unmatched_=errors_=0;error_.clear();
}
json PassCommands::status() {
    std::lock_guard lock(mutex_);
    return {{"input_buffers",inputs_},{"named_pass_buffers",named_},{"matched_bindings",matches_},
        {"unmatched_bindings",unmatched_},{"errors",errors_},{"last_error",error_},
        {"compiled_buffers",compiled_.size()},{"pending_inputs",pending_.size()}};
}
}
