#include "pass_commands.hpp"
#include "build_identity.hpp"
#include <algorithm>
#include <cmath>
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
template<size_t N> std::array<float,N> floats_at(uintptr_t address) {
    auto values=read<std::array<float,N>>(address);
    for(auto x:values) if(!std::isfinite(x))
        throw std::runtime_error("Nonfinite pass camera value");
    return values;
}
json camera_at_compile(uintptr_t pass,uintptr_t base) {
    json result={{"available",false},{"sample_phase","dx11_compile_pass_begin"},
        {"qpc",qpc_now()},{"scope","pass_base_state; per-draw overrides not inspected"}};
    try {
        static const json s=json::parse(OT_SCHEMA).at("render_pass_camera");
        const auto offset=[&](const char* key){return s.at(key).get<uintptr_t>();};
        const auto callback=read<uintptr_t>(pass+offset("callback_offset"));
        if(!callback || read<uintptr_t>(callback)!=base+offset("wrapper_vtable_rva"))
            throw std::runtime_error("Unsupported surface pass callback");
        const auto inner=read<uintptr_t>(callback+offset("inner_callback_offset"));
        if(!inner || read<uintptr_t>(inner)!=base+offset("inner_vtable_rva"))
            throw std::runtime_error("Unsupported surface pass work layout");
        const auto work=read<uintptr_t>(pass+offset("work_offset"));
        if(!work) throw std::runtime_error("Surface pass work is absent");
        const auto id=read<uint16_t>(work+offset("batch_id_offset"));
        const auto pool=array(base+offset("batch_pool_rva"));
        if(id>=pool.size) throw std::runtime_error("Surface pass component batch is absent");
        const auto batch=pool.data+id*offset("batch_stride");
        const auto components=array(batch);
        uintptr_t camera{},deferred{};
        for(uint64_t i=0;i<components.size;++i) {
            const auto component=read<uintptr_t>(components.data+i*sizeof(uintptr_t));
            if(!component) continue;
            const auto type=read<uintptr_t>(component);
            if(type==base+offset("camera_vtable_rva")) camera=component;
            if(type==base+offset("deferred_vtable_rva")) deferred=component;
        }
        if(!camera || !deferred) throw std::runtime_error("Pass camera or deferred state is absent");
        const auto local=floats_at<3>(camera+offset("position_offset"));
        const auto cells=read<std::array<int16_t,2>>(camera+offset("cell_offset"));
        const auto scale=s.at("cell_scale").get<double>();
        result.update({{"work_address",work},{"component_batch_id",id},
            {"component_mask",read<uint32_t>(batch+offset("batch_mask_offset"))},
            {"camera_address",camera},{"deferred_state_address",deferred},
            {"viewport_depth",floats_at<2>(work+offset("viewport_depth_offset"))},
            {"viewport_mode",read<uint32_t>(work+offset("viewport_mode_offset"))},
            {"viewport_rect_raw",floats_at<4>(work+offset("viewport_rect_offset"))},
            {"projection_row_major",floats_at<16>(work+offset("projection_offset"))},
            {"projection_modifier",floats_at<4>(work+offset("projection_modifier_offset"))},
            {"projection_modifier_flag",read<uint8_t>(work+offset("projection_modifier_flag_offset"))},
            {"camera_rotation_row_major",floats_at<16>(camera+offset("rotation_offset"))},
            {"camera_local_xyz",local},{"camera_cell_xz",cells},
            {"camera_world_xyz",std::array<double,3>{local[0]+scale*cells[0],local[1],local[2]+scale*cells[1]}},
            {"world_units","game_length_units"},
            {"ray",floats_at<4>(deferred+offset("ray_offset"))},
            {"deferred_dimensions",floats_at<4>(deferred+offset("dimensions_offset"))}});
        result["available"]=true;
    } catch(const std::exception& e) {result["error"]=e.what();}
    return result;
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
    // The input command buffer is embedded at pass+0x2E8. The constructor
    // stores its graph index at +0x1F8; confirm membership in the live array
    // before interpreting it as a pass. Non-pass command buffers stay unnamed.
    if(input<0x2E8) return {};
    const auto pass=input-0x2E8;
    uint32_t index{};
    if(!read_memory(pass+0x1F8,index)) return {};
    for(unsigned b=0;b<3;++b) {
        const auto manager=base+0x304FC50+b*0x2A8;
        const auto passes=array(manager+0xF0);
        if(index<passes.size && read<uintptr_t>(passes.data+index*8)==pass) {
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
            const bool mirror_surface=std::any_of(links.begin(),links.end(),[](const json& image) {
                return image.at("name")=="attributes_0" &&
                    image.at("namespace").get_ref<const std::string&>().starts_with("mirror");
            });
            if(mirror_surface) result["camera_at_compile"]=camera_at_compile(pass,base);
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
