#include "pass_commands.hpp"
#include "build_identity.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>
#include <unordered_set>

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

// Arrays belong to the engine. Bound observational work without treating a
// budget cutoff as evidence that no more objects exist.
std::vector<uintptr_t> pointers(uintptr_t address,size_t stride,size_t offset,
                                size_t limit,bool& truncated) {
    const auto a=array(address);
    const auto count=std::min<uint64_t>(a.size,limit);
    truncated=truncated || count<a.size;
    std::vector<uint8_t> bytes(static_cast<size_t>(count)*stride);
    if(count && !copy_memory(a.data,bytes.data(),bytes.size()))
        throw std::runtime_error("Cannot read render vehicle array");
    std::vector<uintptr_t> result(static_cast<size_t>(count));
    for(size_t i=0;i<result.size();++i) std::memcpy(&result[i],bytes.data()+i*stride+offset,sizeof(uintptr_t));
    return result;
}
json actor_placement(uintptr_t address) {
    struct Placement {std::array<float,3> local;std::array<int16_t,2> cells;std::array<float,4> quaternion;};
    static_assert(sizeof(Placement)==32);
    const auto p=read<Placement>(address);
    for(auto x:p.local) if(!std::isfinite(x)) throw std::runtime_error("Nonfinite actor position");
    for(auto x:p.quaternion) if(!std::isfinite(x)) throw std::runtime_error("Nonfinite actor quaternion");
    return {{"local_xyz",p.local},{"cell_xz",p.cells},{"quaternion_wxyz",p.quaternion},
        {"world_xyz",std::array<double,3>{p.local[0]+512.0*p.cells[0],p.local[1],p.local[2]+512.0*p.cells[1]}}};
}
json vehicles_at_compile(uintptr_t work,uintptr_t base) {
    json result={{"available",false},{"qpc_begin",qpc_now()},
        {"sample_phase","dx11_compile_pass_begin"},
        {"scope","AI and parked body models in this pass; actor pose is a separate simulation observation"},
        {"vehicles",json::array()},{"errors",json::array()}};
    bool truncated=false;
    try {
        static const json schema=json::parse(OT_SCHEMA);
        const auto& s=schema.at("render_vehicle");
        const auto offset=[&](const char* key){return s.at(key).get<uintptr_t>();};
        const auto& camera=schema.at("render_pass_camera");
        const auto pool=array(base+camera.at("batch_pool_rva").get<uintptr_t>());
        const auto batch_stride=camera.at("batch_stride").get<size_t>();
        const auto scene=read<uintptr_t>(work+offset("work_scene_offset"));
        if(!scene) throw std::runtime_error("Pass has no scene draw list");
        const auto groups=array(scene+offset("scene_groups_offset"));
        const auto source=array(scene+offset("scene_geometry_offset"));
        const auto overrides=array(scene+offset("scene_overrides_offset"));
        result.update({{"scene_address",scene},{"source_geometry_count",source.size},
            {"prepared_group_count",groups.size},{"state_override_count",overrides.size}});
        std::unordered_set<uintptr_t> submitted;
        if(source.size && !groups.size)
            throw std::runtime_error("Pass geometry has no prepared draw groups at this compilation boundary");
        const auto count=std::min<uint64_t>(groups.size,1024);
        truncated=truncated || count<groups.size;
        for(uint64_t i=0;i<count;++i) {
            const auto group=groups.data+i*offset("group_stride");
            for(auto q:pointers(group+offset("group_items_offset"),offset("draw_item_stride"),
                                 offset("draw_item_geometry_offset"),8192,truncated)) if(q) submitted.insert(q);
        }
        result["geometry_scope"]="prepared_draw_items; per-range dispatch suppression and final draw execution not traced";
        result["unique_geometry_count"]=submitted.size();
        const auto traffic=read<uintptr_t>(base+offset("traffic_pointer_rva"));
        if(!traffic) throw std::runtime_error("Traffic manager is absent");
        std::unordered_map<uintptr_t,bool> actors; // false=AI, true=parked
        for(const auto key:{"spawned_array_1_offset","spawned_array_2_offset"})
            for(auto a:pointers(traffic+offset(key),16,0,128,truncated)) if(a) actors.emplace(a,false);
        for(auto a:pointers(traffic+offset("traffic_objects_offset"),8,0,512,truncated)) {
            if(!a) continue;
            const auto vtable=read<uintptr_t>(a);
            const auto getter=read<uintptr_t>(vtable+8);
            const auto code=read<std::array<uint8_t,6>>(getter);
            if(code[0]!=0xB8 || code[5]!=0xC3) continue;
            uint32_t type{};std::memcpy(&type,code.data()+1,sizeof(type));
            if(type==5 || type==6) actors.insert_or_assign(a,true);
        }
        result["actors_considered"]=actors.size();
        for(const auto& [actor,parked]:actors) {
            try {
                const auto holder=parked?read<uintptr_t>(actor+offset("parked_actor_model_holder_offset")):0;
                const auto model=parked?(holder?read<uintptr_t>(holder+offset("holder_model_offset")):0):
                    read<uintptr_t>(actor+offset("ai_actor_model_offset"));
                if(!model) continue;
                if(read<uintptr_t>(model)!=base+offset("model_vtable_rva"))
                    throw std::runtime_error("Unsupported vehicle model layout");
                const auto lods=pointers(model+offset("model_lod_array_offset"),8,0,8,truncated);
                for(size_t lod=0;lod<lods.size();++lod) {
                    const auto object=lods[lod];if(!object) continue;
                    std::vector<uintptr_t> matches;
                    for(auto q:pointers(object+offset("model_object_geometry_array_offset"),8,0,256,truncated))
                        if(q && submitted.contains(q)) matches.push_back(q);
                    if(matches.empty()) continue;
                    const auto component=read<uintptr_t>(object+offset("model_object_component_offset"));
                    if(!component || read<uintptr_t>(component)!=base+offset("model_component_vtable_rva"))
                        throw std::runtime_error("Unsupported vehicle transform component");
                    // Establish the actual Q -> additional batch -> model link;
                    // cached Q slots can still describe a previous pass.
                    for(auto q:matches) {
                        const auto id=read<uint16_t>(q+offset("geometry_additional_batch_id_offset"));
                        if(id>=pool.size) throw std::runtime_error("Vehicle geometry has no component batch");
                        const auto components=pointers(pool.data+id*batch_stride,8,0,32,truncated);
                        if(std::find(components.begin(),components.end(),component)==components.end())
                            throw std::runtime_error("Vehicle geometry no longer references its model component");
                    }
                    const auto local=floats_at<3>(component+offset("model_component_local_xyz_offset"));
                    const auto cells=read<std::array<int16_t,2>>(component+offset("model_component_cell_xz_offset"));
                    result["vehicles"].push_back({{"actor_address",actor},{"kind",parked?"parked":"ai"},
                        {"model_address",model},{"model_object_address",object},{"lod_index",lod},
                        {"component_address",component},{"geometry_addresses",matches},{"qpc",qpc_now()},
                        {"model_rotation_row_major",floats_at<16>(component+offset("model_component_rotation_offset"))},
                        {"model_local_xyz",local},{"model_cell_xz",cells},
                        {"model_world_xyz",std::array<double,3>{local[0]+512.0*cells[0],local[1],local[2]+512.0*cells[1]}},
                        {"model_reference_offset_raw",floats_at<3>(model+offset("model_reference_offset"))},
                        {"actor_observation",{{"placement",actor_placement(actor+offset("actor_placement_offset"))},
                                               {"aabb_raw",floats_at<6>(actor+offset("actor_aabb_offset"))}}}});
                }
            } catch(const std::exception& e) {
                result["errors"].push_back({{"actor_address",actor},{"error",e.what()}});
            }
        }
        result["available"]=true;
    } catch(const std::exception& e) {result["error"]=e.what();}
    result["truncated_for_read_budget"]=truncated;
    result["qpc_end"]=qpc_now();
    return result;
}
}
void PassCommands::draw_batch(uintptr_t input,uintptr_t items,uintptr_t bindings,uint32_t count) noexcept {
    // 2B6B20 emits delta binding packets for each item. Consume every packet
    // in this chunk, including non-vehicle draws, to preserve inherited slots.
    std::lock_guard lock(draws_mutex_);
    try {
        if(!draws_.contains(input) && draws_.size()>=1024)
            throw std::runtime_error("Draw observation input budget exceeded");
        auto& batch=draws_[input];
        Draw state{};
        const auto observed=std::min<uint32_t>(count,8192);
        batch.truncated=batch.truncated || count>observed;
        for(uint32_t i=0;i<observed;++i) {
            // Stage 0 is VS: 2B78D0 dispatches it to VSSetConstantBuffers1.
            const auto packet=read<uintptr_t>(bindings+static_cast<size_t>(i)*48);
            if(packet) {
                const auto flags=read<uint32_t>(packet+4);
                const auto mask=(flags>>4)&15;
                if(mask&1) {
                    const auto n=std::popcount(mask);
                    state.buffer=read<uintptr_t>(packet+8);
                    state.first=read<uint32_t>(packet+8+n*8);
                    state.count=read<uint32_t>(packet+8+n*12);
                    state.known=true;
                }
            }
            if(batch.draws.size()>=8192) {batch.truncated=true;break;}
            state.geometry=read<uintptr_t>(items+static_cast<size_t>(i)*24+16);
            state.qpc=qpc_now();
            batch.draws.push_back(state);
        }
    } catch(const std::exception& e) {
        if(auto found=draws_.find(input);found!=draws_.end()) found->second.error=e.what();
    } catch(...) {}
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
std::shared_ptr<const json> PassCommands::describe(uintptr_t input,bool vehicles,const std::shared_ptr<const json>& sdk) {
    DrawBatch draw_batch;
    {
        std::lock_guard lock(draws_mutex_);
        if(auto found=draws_.find(input);found!=draws_.end()) {
            draw_batch=std::move(found->second);draws_.erase(found);
        }
    }
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
            const auto images=array(manager);
            const auto outputs=array(pass+0x658);
            bool mirror_output=false;
            for(uint64_t j=0;j<outputs.size;++j) {
                const auto id=read<uint32_t>(outputs.data+j*8);
                if(id>=images.size) continue;
                const auto label=read<uintptr_t>(images.data+id*0x7F0+0xA8);
                std::array<char,6> prefix{};
                if(label && copy_memory(label,prefix.data(),prefix.size()) &&
                   std::memcmp(prefix.data(),"mirror",prefix.size())==0) {mirror_output=true;break;}
            }
            // pass+0xC0 names the implementation (e.g. deferred or quad_drawer),
            // not the camera. Filter on output image namespaces before building
            // strings/JSON; begin/end still retain unnamed command intervals.
            if(!mirror_output) return {};
            json result={{"pass_address",pass},{"command_buffer",input},{"graph_buffer",b},
                {"pass_name",string_at(read<uintptr_t>(pass+0x20))},
                {"pass_namespace",string_at(read<uintptr_t>(pass+0xC0))}};
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
            if(mirror_surface) {
                if(sdk) {
                    auto& sample=result["sdk_at_compile"]=json::object();
                    for(const auto* key:{"frame_id","truck_generation","paused","render_time_us","simulation_time_us",
                            "paused_simulation_time_us","timer_flags","sdk"}) sample[key]=sdk->at(key);
                    sample["association"]="last SDK frame_end before pass compilation";
                    if(sdk->contains("engine") && sdk->at("engine").contains("vehicle"))
                        result["ego_at_compile"]=sdk->at("engine").at("vehicle");
                }
                auto camera=camera_at_compile(pass,base);
                if(vehicles && camera.at("available").get<bool>()) {
                    result["vehicles_at_compile"]=vehicles_at_compile(camera.at("work_address").get<uintptr_t>(),base);
                    auto& observation=result["vehicles_at_compile"];
                    observation["draw_bindings"]={{"sample_phase","after_dx11_draw_binding_preparation"},
                        {"scope","emitted draw batch VS slot 0; final draw execution not hooked"},
                        {"observed_draw_items",draw_batch.draws.size()},
                        {"truncated_for_read_budget",draw_batch.truncated},{"error",draw_batch.error}};
                    for(auto& vehicle:observation["vehicles"]) {
                        auto& draws=vehicle["draws"]=json::array();
                        const auto geometry=vehicle.at("geometry_addresses").get<std::vector<uintptr_t>>();
                        for(size_t i=0;i<draw_batch.draws.size();++i) {
                            const auto& d=draw_batch.draws[i];
                            if(std::find(geometry.begin(),geometry.end(),d.geometry)==geometry.end()) continue;
                            draws.push_back({{"draw_item_index",i},{"geometry_address",d.geometry},{"qpc",d.qpc},
                                {"vs_cb0",{{"known",d.known},{"source_buffer",d.buffer},
                                    {"first_constant",d.first},{"num_constants",d.count}}}});
                        }
                    }
                }
                result["camera_at_compile"]=std::move(camera);
            }
            result["linked_images"]=std::move(links);
            return std::make_shared<const json>(std::move(result));
        }
    }
    return {};
}
void PassCommands::begin(uintptr_t frame,uintptr_t input,uintptr_t output,uint16_t id,bool vehicles,std::shared_ptr<const json> sdk) noexcept {
    try {
        auto before=blocks(output);
        const bool empty=std::all_of(before.begin(),before.end(),[](const Block& b){return b.size==0;});
        auto pass=describe(input,vehicles,sdk);
        std::lock_guard lock(mutex_);
        ++inputs_;if(pass) ++named_;
        // The engine resets an allocated compiled buffer before filling it.
        // Its first empty boundary retires the previous use of this pool ID.
        if(empty) compiled_.erase(id);
        pending_.insert_or_assign(frame,Pending{output,id,std::move(before),std::move(pass)});
    } catch(const std::exception& e) {
        {std::lock_guard lock(draws_mutex_);draws_.erase(input);}
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
    {std::lock_guard lock(draws_mutex_);draws_.clear();}
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
