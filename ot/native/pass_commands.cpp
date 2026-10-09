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
struct CameraLayout {
    uintptr_t callback_offset;
    uintptr_t wrapper_vtable_rva;
    uintptr_t inner_callback_offset;
    uintptr_t inner_vtable_rva;
    uintptr_t work_offset;
    uintptr_t batch_id_offset;
    uintptr_t batch_pool_rva;
    uintptr_t batch_stride;
    uintptr_t camera_vtable_rva;
    uintptr_t deferred_vtable_rva;
    uintptr_t position_offset;
    uintptr_t cell_offset;
    uintptr_t batch_mask_offset;
    uintptr_t viewport_depth_offset;
    uintptr_t viewport_mode_offset;
    uintptr_t viewport_rect_offset;
    uintptr_t projection_offset;
    uintptr_t projection_modifier_offset;
    uintptr_t projection_modifier_flag_offset;
    uintptr_t rotation_offset;
    uintptr_t ray_offset;
    uintptr_t dimensions_offset;
    double cell_scale;
};
const CameraLayout& camera_layout() {
    static const CameraLayout value=[] {
        const auto s=json::parse(OT_SCHEMA).at("render_pass_camera");
        return CameraLayout{
            s.at("callback_offset").get<uintptr_t>(),
            s.at("wrapper_vtable_rva").get<uintptr_t>(),
            s.at("inner_callback_offset").get<uintptr_t>(),
            s.at("inner_vtable_rva").get<uintptr_t>(),
            s.at("work_offset").get<uintptr_t>(),
            s.at("batch_id_offset").get<uintptr_t>(),
            s.at("batch_pool_rva").get<uintptr_t>(),
            s.at("batch_stride").get<uintptr_t>(),
            s.at("camera_vtable_rva").get<uintptr_t>(),
            s.at("deferred_vtable_rva").get<uintptr_t>(),
            s.at("position_offset").get<uintptr_t>(),
            s.at("cell_offset").get<uintptr_t>(),
            s.at("batch_mask_offset").get<uintptr_t>(),
            s.at("viewport_depth_offset").get<uintptr_t>(),
            s.at("viewport_mode_offset").get<uintptr_t>(),
            s.at("viewport_rect_offset").get<uintptr_t>(),
            s.at("projection_offset").get<uintptr_t>(),
            s.at("projection_modifier_offset").get<uintptr_t>(),
            s.at("projection_modifier_flag_offset").get<uintptr_t>(),
            s.at("rotation_offset").get<uintptr_t>(),
            s.at("ray_offset").get<uintptr_t>(),
            s.at("dimensions_offset").get<uintptr_t>(),
            s.at("cell_scale").get<double>()};
    }();
    return value;
}
struct VehicleLayout {
    uintptr_t work_scene_offset;
    uintptr_t scene_groups_offset;
    uintptr_t scene_geometry_offset;
    uintptr_t scene_overrides_offset;
    uintptr_t group_stride;
    uintptr_t group_items_offset;
    uintptr_t draw_item_stride;
    uintptr_t draw_item_geometry_offset;
    uintptr_t traffic_pointer_rva;
    uintptr_t traffic_objects_offset;
    uintptr_t parked_actor_model_holder_offset;
    uintptr_t holder_model_offset;
    uintptr_t ai_actor_model_offset;
    uintptr_t model_vtable_rva;
    uintptr_t model_lod_array_offset;
    uintptr_t model_object_geometry_array_offset;
    uintptr_t model_object_component_offset;
    uintptr_t model_component_vtable_rva;
    uintptr_t geometry_additional_batch_id_offset;
    uintptr_t model_component_local_xyz_offset;
    uintptr_t model_component_cell_xz_offset;
    uintptr_t model_component_rotation_offset;
    uintptr_t model_reference_offset;
    uintptr_t actor_placement_offset;
    uintptr_t actor_aabb_offset;
    uintptr_t spawned_array_1_offset;
    uintptr_t spawned_array_2_offset;
};
const VehicleLayout& vehicle_layout() {
    static const VehicleLayout value=[] {
        const auto s=json::parse(OT_SCHEMA).at("render_vehicle");
        return VehicleLayout{
            s.at("work_scene_offset").get<uintptr_t>(),
            s.at("scene_groups_offset").get<uintptr_t>(),
            s.at("scene_geometry_offset").get<uintptr_t>(),
            s.at("scene_overrides_offset").get<uintptr_t>(),
            s.at("group_stride").get<uintptr_t>(),
            s.at("group_items_offset").get<uintptr_t>(),
            s.at("draw_item_stride").get<uintptr_t>(),
            s.at("draw_item_geometry_offset").get<uintptr_t>(),
            s.at("traffic_pointer_rva").get<uintptr_t>(),
            s.at("traffic_objects_offset").get<uintptr_t>(),
            s.at("parked_actor_model_holder_offset").get<uintptr_t>(),
            s.at("holder_model_offset").get<uintptr_t>(),
            s.at("ai_actor_model_offset").get<uintptr_t>(),
            s.at("model_vtable_rva").get<uintptr_t>(),
            s.at("model_lod_array_offset").get<uintptr_t>(),
            s.at("model_object_geometry_array_offset").get<uintptr_t>(),
            s.at("model_object_component_offset").get<uintptr_t>(),
            s.at("model_component_vtable_rva").get<uintptr_t>(),
            s.at("geometry_additional_batch_id_offset").get<uintptr_t>(),
            s.at("model_component_local_xyz_offset").get<uintptr_t>(),
            s.at("model_component_cell_xz_offset").get<uintptr_t>(),
            s.at("model_component_rotation_offset").get<uintptr_t>(),
            s.at("model_reference_offset").get<uintptr_t>(),
            s.at("actor_placement_offset").get<uintptr_t>(),
            s.at("actor_aabb_offset").get<uintptr_t>(),
            s.at("spawned_array_1_offset").get<uintptr_t>(),
            s.at("spawned_array_2_offset").get<uintptr_t>()};
    }();
    return value;
}
RenderCameraSample camera_at_compile(uintptr_t pass,uintptr_t base) {
    RenderCameraSample result;result.qpc=qpc_now();
    try {
        const auto& layout=camera_layout();
        const auto callback=read<uintptr_t>(pass+layout.callback_offset);
        if(!callback || read<uintptr_t>(callback)!=base+layout.wrapper_vtable_rva)
            throw std::runtime_error("Unsupported surface pass callback");
        const auto inner=read<uintptr_t>(callback+layout.inner_callback_offset);
        if(!inner || read<uintptr_t>(inner)!=base+layout.inner_vtable_rva)
            throw std::runtime_error("Unsupported surface pass work layout");
        result.work=read<uintptr_t>(pass+layout.work_offset);
        if(!result.work) throw std::runtime_error("Surface pass work is absent");
        result.batch=read<uint16_t>(result.work+layout.batch_id_offset);
        const auto pool=array(base+layout.batch_pool_rva);
        if(result.batch>=pool.size) throw std::runtime_error("Surface pass component batch is absent");
        const auto batch=pool.data+result.batch*layout.batch_stride;
        const auto components=array(batch);
        for(uint64_t i=0;i<components.size;++i) {
            const auto component=read<uintptr_t>(components.data+i*sizeof(uintptr_t));
            if(!component) continue;
            const auto type=read<uintptr_t>(component);
            if(type==base+layout.camera_vtable_rva) result.camera=component;
            if(type==base+layout.deferred_vtable_rva) result.deferred=component;
        }
        if(!result.camera || !result.deferred) throw std::runtime_error("Pass camera or deferred state is absent");
        result.local=floats_at<3>(result.camera+layout.position_offset);
        result.cells=read<std::array<int16_t,2>>(result.camera+layout.cell_offset);
        const auto scale=layout.cell_scale;
        result.world={result.local[0]+scale*result.cells[0],result.local[1],result.local[2]+scale*result.cells[1]};
        result.component_mask=read<uint32_t>(batch+layout.batch_mask_offset);
        result.viewport_depth=floats_at<2>(result.work+layout.viewport_depth_offset);
        result.viewport_mode=read<uint32_t>(result.work+layout.viewport_mode_offset);
        result.viewport_rect=floats_at<4>(result.work+layout.viewport_rect_offset);
        result.projection=floats_at<16>(result.work+layout.projection_offset);
        result.projection_modifier=floats_at<4>(result.work+layout.projection_modifier_offset);
        result.projection_modifier_flag=read<uint8_t>(result.work+layout.projection_modifier_flag_offset);
        result.rotation=floats_at<16>(result.camera+layout.rotation_offset);
        result.ray=floats_at<4>(result.deferred+layout.ray_offset);
        result.dimensions=floats_at<4>(result.deferred+layout.dimensions_offset);
        result.available=true;
    } catch(const std::exception& e) {result.error=e.what();}
    return result;
}

// Arrays belong to the engine. Bound observational work without treating a
// budget cutoff as evidence that no more objects exist.
std::span<const uintptr_t> pointers(uintptr_t address,size_t stride,size_t offset,
                                size_t limit,bool& truncated,std::vector<uintptr_t>& result) {
    const auto a=array(address);
    const auto count=std::min<uint64_t>(a.size,limit);
    truncated=truncated || count<a.size;
    result.resize(static_cast<size_t>(count));
    if(stride==sizeof(uintptr_t) && offset==0) {
        if(count && !copy_memory(a.data,result.data(),result.size()*sizeof(uintptr_t)))
            throw std::runtime_error("Cannot read render vehicle array");
        return result;
    }
    thread_local std::vector<uint8_t> bytes;
    bytes.resize(static_cast<size_t>(count)*stride);
    if(count && !copy_memory(a.data,bytes.data(),bytes.size()))
        throw std::runtime_error("Cannot read render vehicle array");
    for(size_t i=0;i<result.size();++i) std::memcpy(&result[i],bytes.data()+i*stride+offset,sizeof(uintptr_t));
    return result;
}
RenderActorPlacement actor_placement(uintptr_t address) {
    static_assert(sizeof(RenderActorPlacement)==32);
    const auto p=read<RenderActorPlacement>(address);
    for(auto x:p.local) if(!std::isfinite(x)) throw std::runtime_error("Nonfinite actor position");
    for(auto x:p.quaternion) if(!std::isfinite(x)) throw std::runtime_error("Nonfinite actor quaternion");
    return p;
}
RenderVehiclesSample vehicles_at_compile(uintptr_t work,uintptr_t base) {
    RenderVehiclesSample result;result.qpc_begin=qpc_now();
    // Reuse storage, not engine observations. Every pass still reads its own
    // submitted geometry and current model transforms.
    struct Scratch {
        std::vector<uintptr_t> items,lods,geometry,components,submitted,matches;
        std::vector<std::pair<uintptr_t,bool>> actors;
    };
    thread_local Scratch scratch;
    auto& submitted=scratch.submitted;submitted.clear();
    auto& actors=scratch.actors;actors.clear();
    bool truncated=false;
    try {
        const auto& layout=vehicle_layout();const auto& camera=camera_layout();
        const auto pool=array(base+camera.batch_pool_rva);
        const auto batch_stride=camera.batch_stride;
        const auto scene=read<uintptr_t>(work+layout.work_scene_offset);
        if(!scene) throw std::runtime_error("Pass has no scene draw list");
        const auto groups=array(scene+layout.scene_groups_offset);
        const auto source=array(scene+layout.scene_geometry_offset);
        const auto overrides=array(scene+layout.scene_overrides_offset);
        result.scene=scene;result.source_count=source.size;result.group_count=groups.size;
        result.override_count=overrides.size;result.scene_observed=true;
        if(source.size && !groups.size)
            throw std::runtime_error("Pass geometry has no prepared draw groups at this compilation boundary");
        const auto count=std::min<uint64_t>(groups.size,1024);
        truncated=truncated || count<groups.size;
        for(uint64_t i=0;i<count;++i) {
            const auto group=groups.data+i*layout.group_stride;
            for(auto q:pointers(group+layout.group_items_offset,layout.draw_item_stride,
                                 layout.draw_item_geometry_offset,8192,truncated,scratch.items)) if(q) submitted.push_back(q);
        }
        std::sort(submitted.begin(),submitted.end());
        submitted.erase(std::unique(submitted.begin(),submitted.end()),submitted.end());
        result.unique_count=submitted.size();result.geometry_observed=true;
        const auto traffic=read<uintptr_t>(base+layout.traffic_pointer_rva);
        if(!traffic) throw std::runtime_error("Traffic manager is absent");
        for(const auto offset:{layout.spawned_array_1_offset,layout.spawned_array_2_offset})
            for(auto a:pointers(traffic+offset,16,0,128,truncated,scratch.items)) if(a) actors.emplace_back(a,false);
        for(auto a:pointers(traffic+layout.traffic_objects_offset,8,0,512,truncated,scratch.items)) {
            if(!a) continue;
            const auto vtable=read<uintptr_t>(a);
            const auto getter=read<uintptr_t>(vtable+8);
            const auto code=read<std::array<uint8_t,6>>(getter);
            if(code[0]!=0xB8 || code[5]!=0xC3) continue;
            uint32_t type{};std::memcpy(&type,code.data()+1,sizeof(type));
            if(type==5 || type==6) actors.emplace_back(a,true);
        }
        std::sort(actors.begin(),actors.end(),[](const auto& a,const auto& b){return a.first!=b.first?a.first<b.first:a.second>b.second;});
        actors.erase(std::unique(actors.begin(),actors.end(),[](const auto& a,const auto& b){return a.first==b.first;}),actors.end());
        result.actors_considered=actors.size();result.actors_observed=true;
        for(const auto& [actor,parked]:actors) {
            try {
                const auto holder=parked?read<uintptr_t>(actor+layout.parked_actor_model_holder_offset):0;
                const auto model=parked?(holder?read<uintptr_t>(holder+layout.holder_model_offset):0):
                    read<uintptr_t>(actor+layout.ai_actor_model_offset);
                if(!model) continue;
                if(read<uintptr_t>(model)!=base+layout.model_vtable_rva)
                    throw std::runtime_error("Unsupported vehicle model layout");
                const auto lods=pointers(model+layout.model_lod_array_offset,8,0,8,truncated,scratch.lods);
                for(size_t lod=0;lod<lods.size();++lod) {
                    const auto object=lods[lod];if(!object) continue;
                    auto& matches=scratch.matches;matches.clear();
                    for(auto q:pointers(object+layout.model_object_geometry_array_offset,8,0,256,truncated,scratch.geometry))
                        if(q && std::binary_search(submitted.begin(),submitted.end(),q)) matches.push_back(q);
                    if(matches.empty()) continue;
                    const auto component=read<uintptr_t>(object+layout.model_object_component_offset);
                    if(!component || read<uintptr_t>(component)!=base+layout.model_component_vtable_rva)
                        throw std::runtime_error("Unsupported vehicle transform component");
                    // Establish the actual Q -> additional batch -> model link;
                    // cached Q slots can still describe a previous pass.
                    for(auto q:matches) {
                        const auto id=read<uint16_t>(q+layout.geometry_additional_batch_id_offset);
                        if(id>=pool.size) throw std::runtime_error("Vehicle geometry has no component batch");
                        const auto components=pointers(pool.data+id*batch_stride,8,0,32,truncated,scratch.components);
                        if(std::find(components.begin(),components.end(),component)==components.end())
                            throw std::runtime_error("Vehicle geometry no longer references its model component");
                    }
                    const auto local=floats_at<3>(component+layout.model_component_local_xyz_offset);
                    const auto cells=read<std::array<int16_t,2>>(component+layout.model_component_cell_xz_offset);
                    RenderVehicleSample vehicle;
                    vehicle.actor=actor;vehicle.parked=parked;vehicle.model=model;vehicle.object=object;
                    vehicle.lod=lod;vehicle.component=component;vehicle.geometry=matches;vehicle.qpc=qpc_now();
                    vehicle.rotation=floats_at<16>(component+layout.model_component_rotation_offset);
                    vehicle.local=local;vehicle.cells=cells;
                    vehicle.reference=floats_at<3>(model+layout.model_reference_offset);
                    vehicle.actor_placement=actor_placement(actor+layout.actor_placement_offset);
                    vehicle.aabb=floats_at<6>(actor+layout.actor_aabb_offset);
                    result.vehicles.push_back(std::move(vehicle));
                }
            } catch(const std::exception& e) {
                result.errors.emplace_back(actor,e.what());
            }
        }
        result.available=true;
    } catch(const std::exception& e) {result.error=e.what();}
    result.truncated=truncated;result.qpc_end=qpc_now();
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
RenderPassPtr PassCommands::describe(uintptr_t input,uint32_t vehicle_mask,const std::shared_ptr<const json>& sdk,uint32_t camera_mask,bool diagnostic) {
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
            bool mirror_output=false,geometry_output=false;
            unsigned camera_slot=9;
            std::array<RenderTarget,8> targets{};
            for(uint64_t j=0;j<outputs.size;++j) {
                const auto id=read<uint32_t>(outputs.data+j*8);
                if(id>=images.size) continue;
                const auto label=read<uintptr_t>(images.data+id*0x7F0+0xA8);
                std::array<char,8> prefix{};
                if(label && copy_memory(label,prefix.data(),prefix.size()) &&
                   std::memcmp(prefix.data(),"mirror",6)==0 && prefix[6]>='0' && prefix[6]<='8' &&
                   prefix[7]=='\0' && (camera_mask&(1u<<(prefix[6]-'0')))) {
                    // Streaming consumes the geometry and final-color targets.
                    // Lighting/blur intermediates need no JSON or command labels;
                    // their binds still end the outgoing capture normally.
                    const auto address=read<uintptr_t>(images.data+id*0x7F0+8);
                    std::array<char,16> name{};
                    RenderTarget target=RenderTarget::other;
                    if(address && copy_memory(address,name.data(),name.size())) {
                        if(std::memcmp(name.data(),"attributes_0",sizeof("attributes_0"))==0) target=RenderTarget::attributes0;
                        else if(std::memcmp(name.data(),"attributes_3",sizeof("attributes_3"))==0) target=RenderTarget::attributes3;
                        else if(std::memcmp(name.data(),"composition_raw",sizeof("composition_raw"))==0) target=RenderTarget::color;
                    }
                    if(j<targets.size()) targets[j]=target;
                    if(target!=RenderTarget::other) camera_slot=prefix[6]-'0';
                    geometry_output|=target==RenderTarget::attributes0;
                    mirror_output|=camera_mask==UINT32_MAX || target==RenderTarget::attributes0 || target==RenderTarget::color;
                }
            }
            // pass+0xC0 names the implementation (e.g. deferred or quad_drawer),
            // not the camera. Filter on output image namespaces before building
            // strings or samples. Empty boundaries in begin retire reused ranges.
            if(!mirror_output) return {};
            auto result=std::make_shared<RenderPassSample>();
            result->pass=pass;result->input=input;result->graph=b;
            result->camera_slot=camera_slot;result->targets=targets;result->geometry=geometry_output;
            if(diagnostic) {
                result->name=string_at(read<uintptr_t>(pass+0x20));
                result->space=string_at(read<uintptr_t>(pass+0xC0));
                for(const auto offset:{0x658,0x6C0}) {
                    const auto refs=array(pass+offset);
                    for(uint64_t j=0;j<refs.size;++j) {
                        const auto id=read<uint32_t>(refs.data+j*8);
                        if(id>=images.size) continue;
                        const auto image=images.data+id*0x7F0;
                        result->links.push_back({static_cast<uintptr_t>(offset),id,read<uint16_t>(image+0x740),
                            string_at(read<uintptr_t>(image+0xA8)),string_at(read<uintptr_t>(image+8))});
                    }
                }
            }
            if(result->geometry) {
                result->sdk=sdk;
                result->camera=camera_at_compile(pass,base);
                if((vehicle_mask&(1u<<result->camera_slot)) && result->camera.available) {
                    auto& observation=result->vehicles.emplace(vehicles_at_compile(result->camera.work,base));
                    observation.draw_count=draw_batch.draws.size();
                    observation.draws_truncated=draw_batch.truncated;observation.draw_error=std::move(draw_batch.error);
                    for(auto& vehicle:observation.vehicles) {
                        for(size_t i=0;i<draw_batch.draws.size();++i) {
                            auto d=draw_batch.draws[i];
                            if(std::find(vehicle.geometry.begin(),vehicle.geometry.end(),d.geometry)==vehicle.geometry.end()) continue;
                            d.item_index=i;vehicle.draws.push_back(d);
                        }
                    }
                }
            }
            return result;
        }
    }
    return {};
}
void PassCommands::begin(uintptr_t frame,uintptr_t input,uintptr_t output,uint16_t id,uint32_t vehicle_mask,std::shared_ptr<const json> sdk,uint32_t camera_mask,bool diagnostic) noexcept {
    try {
        auto pass=describe(input,vehicle_mask,sdk,camera_mask,diagnostic);
        if(!pass) {
            // Unlabelled appends cannot overlap an earlier labelled span. Pool
            // reuse does: retire the old spans at the engine's empty boundary,
            // without allocating before/after vectors for unrelated passes.
            const auto header=array(output+0x18);
            bool empty=true;
            for(uint64_t i=0;i<header.size;++i)
                if(read<Block>(header.data+i*sizeof(Block)).size) {empty=false;break;}
            std::lock_guard lock(mutex_);
            ++inputs_;pending_.erase(frame);
            if(empty) compiled_.erase(id);
            return;
        }
        auto before=blocks(output);
        const bool empty=std::all_of(before.begin(),before.end(),[](const Block& b){return b.size==0;});
        std::lock_guard lock(mutex_);
        ++inputs_;++named_;
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
RenderPassPtr PassCommands::lookup(uint16_t id,uintptr_t token) noexcept {
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
