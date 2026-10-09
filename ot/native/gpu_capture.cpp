#include "gpu_capture.hpp"
#include <fstream>
#include <stdexcept>
#include <dxgi1_4.h>

namespace ot {
uint64_t qpc_now() noexcept { LARGE_INTEGER value{};QueryPerformanceCounter(&value);return value.QuadPart; }
uint64_t qpc_frequency() noexcept {
    static const uint64_t frequency=[] {LARGE_INTEGER value{};QueryPerformanceFrequency(&value);return value.QuadPart;}();
    return frequency;
}
namespace {
using Microsoft::WRL::ComPtr;
std::string target_name(const json* pass,unsigned index) {
    if(!pass) return {};
    for(const auto& image:pass->at("linked_images")) {
        if(image.at("reference_array_offset")!=0x658) continue;
        if(index--==0) return image.at("namespace").get<std::string>()+"/"+image.at("name").get<std::string>();
    }
    return {};
}
ComPtr<ID3D11Texture2D> texture(uintptr_t view) {
    ComPtr<ID3D11Texture2D> result;
    if(view) {
        ComPtr<ID3D11Resource> resource;
        reinterpret_cast<ID3D11RenderTargetView*>(view)->GetResource(&resource);
        if(resource) resource.As(&result);
    }
    return result;
}
void check(HRESULT result,const char* operation) {
    if(FAILED(result)) throw std::runtime_error(std::string(operation)+" HRESULT="+std::to_string(result));
}
}

void GpuCapture::release_sources() {
    for(auto& image:images_) image.source.Reset();
    geometry_view_.Reset();color_view_.Reset();
    geometry_depth_.source.Reset();context_.Reset();
}
void GpuCapture::release_gpu() {
    release_sources();packed_.release_gpu();
    for(auto& image:images_) image.staging.Reset();
    geometry_depth_.staging.Reset();
    for(auto& constants:geometry_constants_) constants.staging.Reset();
    for(auto& constants:vehicle_constants_) constants.staging.Reset();
    completion_.Reset();device_.Reset();
}
void GpuCapture::prepare_staging(Image& image,const D3D11_TEXTURE2D_DESC& desc,ID3D11Device* device) {
    D3D11_TEXTURE2D_DESC previous{};if(image.staging) image.staging->GetDesc(&previous);
    if(!image.staging || previous.Width!=desc.Width || previous.Height!=desc.Height || previous.Format!=desc.Format) {
        image.staging.Reset();
        check(device->CreateTexture2D(&desc,nullptr,&image.staging),"CreateTexture2D(staging)");++allocations_;
    }
}
void GpuCapture::prepare_constants(Constants& sample,UINT bytes,ID3D11Device* device) {
    D3D11_BUFFER_DESC previous{};if(sample.staging) sample.staging->GetDesc(&previous);
    if(!sample.staging || previous.ByteWidth!=bytes) {
        sample.staging.Reset();
        D3D11_BUFFER_DESC staging{};staging.ByteWidth=bytes;
        staging.Usage=D3D11_USAGE_STAGING;staging.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        check(device->CreateBuffer(&staging,nullptr,&sample.staging),"CreateBuffer(constant staging)");++allocations_;
    }
}
void GpuCapture::cancel() noexcept {
    try {
        std::lock_guard lock(mutex_);
        release_gpu();
        // A completed CPU sample remains available for saving after panic.
        if(phase_==Phase::armed || phase_==Phase::waiting_gpu) phase_=Phase::idle;
    } catch(...) {}
}
json GpuCapture::status(bool metadata) const {
    const char* phase=phase_==Phase::idle?"idle":phase_==Phase::armed?"armed":
        phase_==Phase::waiting_gpu?"waiting_gpu":phase_==Phase::ready?"ready":"error";
    json result={{"phase",phase},{"capture_sequence",sequence_},{"bindings_seen",bindings_seen_},
            {"camera",camera_},{"requested_frame_id",requested_frame_},{"last_label",last_label_},
            {"gpu_polls",gpu_polls_},{"error",error_},{"gpu_allocations",allocations_+packed_.allocations()},
            {"saved_directory",saved_.string()}};
    if(metadata) result["metadata"]=metadata_;
    return result;
}
json GpuCapture::command(const std::string& action,uint64_t requested_frame,bool metadata,const CaptureOptions& options) {
    std::lock_guard lock(mutex_);
    if(action=="arm") {
        if(phase_==Phase::armed || phase_==Phase::waiting_gpu)
            throw std::runtime_error("A camera capture is already pending");
        release_sources();
        if(!packed_.reusable()) throw std::runtime_error("Shared GPU sample still belongs to the relay");
        options_=options;packed_.clear();packed_.share(options.shared_gpu);packed_.exposure(options.exposure);
        for(auto& image:images_) image.pixels.clear();
        geometry_depth_.pixels.clear();
        for(auto& constants:geometry_constants_) {constants.bytes.clear();constants.description=nullptr;}
        clear_vehicle_constants();
        metadata_=json::object();error_.clear();last_label_.clear();saved_.clear();
        geometry_binding_=geometry_sdk_=gpu_polls_=bindings_seen_=polled_frame_=0;
        geometry_pass_=color_pass_=geometry_gpu_=nullptr;requested_frame_=requested_frame;
        request_started_=GetTickCount64();phase_=Phase::armed;
    } else if(action=="cancel") {
        release_gpu();if(phase_!=Phase::ready) phase_=Phase::idle;
    } else if(action=="save") return save();
    else if(action!="status") throw std::runtime_error("Unknown camera capture action");
    auto result=status(metadata);
    if(action=="status" && device_) {
        ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;ComPtr<IDXGIAdapter3> memory;
        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        if(SUCCEEDED(device_.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter.As(&memory)) &&
           SUCCEEDED(memory->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&info)))
            result["video_memory"]={{"usage_bytes",info.CurrentUsage},{"budget_bytes",info.Budget}};
    }
    return result;
}
void GpuCapture::observe(ID3D11DeviceContext* context,uint32_t count,const uintptr_t* targets,
                         uint64_t binding_sequence,uint64_t sdk_frame,uint64_t render_frame,
                         uint64_t observation_session,const json* pass) noexcept {
    const auto phase=phase_.load(std::memory_order_relaxed);
    if(phase!=Phase::armed && phase!=Phase::waiting_gpu) return;
    // A status query must not make us skip the binding that ends a camera
    // pass. Idle/ready samples skip the lock (including during file saving);
    // pending captures serialize with the command worker and recheck state.
    std::lock_guard lock(mutex_);
    if(phase_!=Phase::armed && phase_!=Phase::waiting_gpu) return;
    try {
        ++bindings_seen_;
        if(GetTickCount64()-request_started_>30000) throw std::runtime_error("Camera capture timed out after 30 seconds");
        if(!context)
            throw std::runtime_error("Camera readback requires the observed immediate context");
        if(phase_==Phase::waiting_gpu) {
            if(context!=context_.Get()) return;
            if(polled_frame_==render_frame) return;
            polled_frame_=render_frame;
            collect(context);return;
        }
        if(!render_frame || (requested_frame_ && render_frame<requested_frame_)) return;
        if(requested_frame_ && render_frame>requested_frame_)
            throw std::runtime_error(camera_+" did not finish rendering in the requested Present interval");
        ComPtr<ID3D11Texture2D> next;
        bool resolved=false;
        const auto next_texture=[&]() {
            if(!resolved) {if(count) next=texture(targets[0]);resolved=true;}
            return next.Get();
        };
        // Retained views cannot be recycled during this sample. Most binds
        // need only pointer comparison; resolve a resource on an actual change.
        const auto leaves=[&](unsigned expected,ID3D11RenderTargetView* view,ID3D11Texture2D* source) {
            return count!=expected || (targets[0]!=reinterpret_cast<uintptr_t>(view) && next_texture()!=source);
        };
        if(images_[0].source && geometry_gpu_.is_null() && context==context_.Get() &&
           geometry_frame_==render_frame && leaves(4,geometry_view_.Get(),images_[0].source.Get())) {
            ComPtr<ID3D11RenderTargetView> current;
            context->OMGetRenderTargets(1,&current,nullptr);
            if(current.Get()==geometry_view_.Get() || texture(reinterpret_cast<uintptr_t>(current.Get())).Get()==images_[0].source.Get())
                geometry_constants(context,binding_sequence);
        }
        // Copy before a new group overwrites the shared G-buffer. Unbinding or
        // switching to multiple targets also ends the outgoing color pass.
        if(images_[2].source && context==context_.Get() &&
           leaves(1,color_view_.Get(),images_[2].source.Get())) {
            ComPtr<ID3D11RenderTargetView> current;
            context->OMGetRenderTargets(1,&current,nullptr);
            if((current.Get()==color_view_.Get() || texture(reinterpret_cast<uintptr_t>(current.Get())).Get()==images_[2].source.Get()) && geometry_frame_==render_frame) {
                submit(context,binding_sequence,sdk_frame,render_frame,observation_session);
                return;
            }
            release_sources();
        }
        const auto label=target_name(pass,0);
        if(!label.empty()) last_label_=label;
        if(count==4) {
            release_sources();
            if(label!=camera_+"/attributes_0" || target_name(pass,3)!=camera_+"/attributes_3") return;
            if(context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)
                throw std::runtime_error("Camera readback requires the observed immediate context");
            next_texture();
            auto flags=texture(targets[3]);
            if(!next || !flags) return;
            Com<ID3D11Device> device;context->GetDevice(&device);
            if(device_.Get()!=device.Get()) {release_gpu();device_=device;}
            images_[0].source=std::move(next);images_[1].source=std::move(flags);
            geometry_view_=reinterpret_cast<ID3D11RenderTargetView*>(targets[0]);
            context_=context;geometry_binding_=binding_sequence;geometry_sdk_=sdk_frame;geometry_frame_=render_frame;
            geometry_pass_=*pass;geometry_gpu_=nullptr;geometry_depth_.pixels.clear();
            for(auto& constants:geometry_constants_) {constants.bytes.clear();constants.description=nullptr;}
            clear_vehicle_constants();
        } else if(count==1 && images_[0].source && context==context_.Get() && label==camera_+"/composition_raw") {
            next_texture();color_view_=reinterpret_cast<ID3D11RenderTargetView*>(targets[0]);
            images_[2].source=std::move(next);color_pass_=*pass;
        }
    } catch(const std::exception& e) {error_=e.what();release_gpu();phase_=Phase::error;}
    catch(...) {error_="Camera readback failed";release_gpu();phase_=Phase::error;}
}
void GpuCapture::geometry_constants(ID3D11DeviceContext* context,uint64_t binding_sequence) {
    Com<ID3D11DeviceContext1> context1;
    check(context->QueryInterface(IID_PPV_ARGS(&context1)),"QueryInterface(DeviceContext1)");
    Com<ID3D11Device> device;context->GetDevice(&device);
    Com<ID3D11VertexShader> vs;Com<ID3D11PixelShader> ps;
    context->VSGetShader(&vs,nullptr,nullptr);context->PSGetShader(&ps,nullptr,nullptr);
    UINT viewport_count=D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    std::array<D3D11_VIEWPORT,D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> viewports{};
    context->RSGetViewports(&viewport_count,viewports.data());
    json viewport_json=json::array();
    for(UINT i=0;i<viewport_count;++i) {
        const auto& v=viewports[i];
        viewport_json.push_back({{"x",v.TopLeftX},{"y",v.TopLeftY},{"width",v.Width},{"height",v.Height},
            {"min_depth",v.MinDepth},{"max_depth",v.MaxDepth}});
    }
    geometry_gpu_={{"phase","before_leaving_gbuffer_binding"},{"qpc",qpc_now()},
        {"binding_sequence",binding_sequence},{"viewports",viewport_json},
        {"vertex_shader",reinterpret_cast<uintptr_t>(vs.Get())},{"pixel_shader",reinterpret_cast<uintptr_t>(ps.Get())}};
    Com<ID3D11DepthStencilView> dsv;context->OMGetRenderTargets(0,nullptr,&dsv);
    if(dsv) {
        Com<ID3D11Resource> resource;dsv->GetResource(&resource);
        check(resource.As(&geometry_depth_.source),"Depth resource Texture2D");
        auto& depth=geometry_depth_;depth.source->GetDesc(&depth.desc);
        const auto& desc=depth.desc;
        D3D11_TEXTURE2D_DESC attributes{};images_[0].source->GetDesc(&attributes);
        auto staging=desc;
        const char* layout{};
        if(desc.Format==DXGI_FORMAT_D32_FLOAT_S8X24_UINT || desc.Format==DXGI_FORMAT_R32G8X24_TYPELESS) {
            staging.Format=DXGI_FORMAT_R32G8X24_TYPELESS;depth_pixel_bytes_=8;layout="D32_FLOAT_S8X24_UINT";
        } else if(desc.Format==DXGI_FORMAT_D24_UNORM_S8_UINT || desc.Format==DXGI_FORMAT_R24G8_TYPELESS) {
            staging.Format=DXGI_FORMAT_R24G8_TYPELESS;depth_pixel_bytes_=4;layout="D24_UNORM_S8_UINT";
        } else if(desc.Format==DXGI_FORMAT_D32_FLOAT || desc.Format==DXGI_FORMAT_R32_TYPELESS) {
            staging.Format=DXGI_FORMAT_R32_TYPELESS;depth_pixel_bytes_=4;layout="D32_FLOAT";
        } else throw std::runtime_error("Unsupported geometry depth texture format");
        if(desc.SampleDesc.Count!=1 || desc.MipLevels!=1 || desc.ArraySize!=1 ||
           desc.Width!=attributes.Width || desc.Height!=attributes.Height)
            throw std::runtime_error("Unsupported or unaligned geometry depth texture");
        if(options_.raw()) {
            staging.Usage=D3D11_USAGE_STAGING;staging.BindFlags=0;
            staging.CPUAccessFlags=D3D11_CPU_ACCESS_READ;staging.MiscFlags=0;
            prepare_staging(depth,staging,device.Get());
            depth.pixels.resize(static_cast<size_t>(desc.Width)*desc.Height*depth_pixel_bytes_);
            context->CopyResource(depth.staging.Get(),depth.source.Get());
            geometry_gpu_["depth_texture"]={{"file",camera_+"_geometry_depth.bin"},
                {"width",desc.Width},{"height",desc.Height},{"row_bytes",desc.Width*depth_pixel_bytes_},
                {"format",layout},{"source_dxgi_format",static_cast<unsigned>(desc.Format)},
                {"resource",reinterpret_cast<uintptr_t>(depth.source.Get())}};
        }
        if(options_.packed() && (options_.depth() || options_.lidar())) {
            if(viewport_count!=1 || !(viewports[0].Width>0 && viewports[0].Height>0 && viewports[0].MaxDepth>viewports[0].MinDepth))
                throw std::runtime_error("RGB-D packing requires one valid geometry viewport");
            packed_.depth(context1.Get(),depth.source.Get(),images_[0].source.Get(),images_[1].source.Get(),viewports[0],camera_,options_.metric()?&geometry_pass_.at("camera_at_compile").at("projection_row_major"):nullptr,options_.depth(),options_.lidar_pattern);
            geometry_gpu_["packed_depth_texture"]=packed_.images[0].description;
        }
    }
    if(options_.packed() && !dsv) throw std::runtime_error("RGB-D packing requires a geometry depth buffer");
    if(options_.metric() && !options_.raw()) return; // Production metadata uses the captured CPU pass; GPU constants remain a research output.
    for(size_t stage=0;stage<geometry_constants_.size();++stage) {
        auto& sample=geometry_constants_[stage];
        Com<ID3D11Buffer> source;UINT first{},count{};
        if(stage==0) context1->VSGetConstantBuffers1(0,1,&source,&first,&count);
        else context1->PSGetConstantBuffers1(0,1,&source,&first,&count);
        sample.description={{"stage",stage==0?"vs":"ps"},{"slot",0},{"bound",source!=nullptr},
            {"first_constant",first},{"num_constants",count}};
        if(!source) continue;
        D3D11_BUFFER_DESC desc{};source->GetDesc(&desc);
        const uint64_t offset=static_cast<uint64_t>(first)*16;
        const uint64_t bound_size=static_cast<uint64_t>(count)*16;
        // The API exposes 16-byte constant units; bound ranges can extend past
        // the buffer, where shader reads are zero. Copy only existing storage.
        const auto bytes=offset<desc.ByteWidth?std::min<uint64_t>(bound_size,desc.ByteWidth-offset):0;
        sample.description.update({{"source_buffer",reinterpret_cast<uintptr_t>(source.Get())},
            {"source_byte_width",desc.ByteWidth},{"source_byte_offset",offset},{"copied_bytes",bytes}});
        if(!bytes) continue;
        prepare_constants(sample,static_cast<UINT>(bytes),device.Get());
        sample.bytes.resize(static_cast<size_t>(bytes));
        sample.description["file"]=camera_+(stage==0?"_geometry_vs_cb0.bin":"_geometry_ps_cb0.bin");
        D3D11_BOX box{static_cast<UINT>(offset),0,0,static_cast<UINT>(offset+bytes),1,1};
        context->CopySubresourceRegion(sample.staging.Get(),0,0,0,0,source.Get(),0,&box);
    }
    // The existing image completion query is inserted later on this context,
    // so it also covers these buffer copies without an additional GPU wait.
    vehicle_constants(context,device.Get());
}
void GpuCapture::clear_vehicle_constants() {
    vehicle_constants_used_=0;
    for(auto& sample:vehicle_constants_) {sample.bytes.clear();sample.description=nullptr;}
}
void GpuCapture::vehicle_constants(ID3D11DeviceContext* context,ID3D11Device* device) {
    if(!geometry_pass_.contains("vehicles_at_compile")) return;
    geometry_gpu_["vehicle_constants_scope"]="draw-batch VS slot 0 ranges copied at G-buffer exit; per-draw execution not hooked";
    geometry_gpu_["vehicle_constants_truncated_for_read_budget"]=false;
    for(const auto& vehicle:geometry_pass_.at("vehicles_at_compile").at("vehicles")) {
        for(const auto& draw:vehicle.at("draws")) {
            const auto& bound=draw.at("vs_cb0");
            if(!bound.at("known").get<bool>() || !bound.at("source_buffer").get<uintptr_t>()) continue;
            if(vehicle_constants_used_>=64) {
                geometry_gpu_["vehicle_constants_truncated_for_read_budget"]=true;return;
            }
            // The engine keeps these draw resources alive through command
            // execution. Acquire our reference while still inside that pass.
            Com<ID3D11Buffer> source=reinterpret_cast<ID3D11Buffer*>(bound.at("source_buffer").get<uintptr_t>());
            D3D11_BUFFER_DESC desc{};source->GetDesc(&desc);
            if(!(desc.BindFlags&D3D11_BIND_CONSTANT_BUFFER))
                throw std::runtime_error("Vehicle draw references a non-constant buffer");
            const uint64_t offset=bound.at("first_constant").get<uint32_t>()*uint64_t{16};
            const uint64_t size=bound.at("num_constants").get<uint32_t>()*uint64_t{16};
            const auto bytes=offset<desc.ByteWidth?std::min<uint64_t>(size,desc.ByteWidth-offset):0;
            if(bytes>65536) throw std::runtime_error("Vehicle constant range exceeds the D3D11 shader limit");
            const auto index=vehicle_constants_used_++;
            if(index==vehicle_constants_.size()) vehicle_constants_.emplace_back();
            auto& sample=vehicle_constants_[index];
            sample.description=bound;
            sample.description.update({{"stage","vs"},{"slot",0},{"actor_address",vehicle.at("actor_address")},
                {"geometry_address",draw.at("geometry_address")},{"draw_item_index",draw.at("draw_item_index")},
                {"source_byte_width",desc.ByteWidth},{"source_byte_offset",offset},{"copied_bytes",bytes}});
            if(bytes) {
                prepare_constants(sample,static_cast<UINT>(bytes),device);
                sample.bytes.resize(static_cast<size_t>(bytes));
                sample.description["file"]=camera_+"_vehicle_"+std::to_string(index)+"_vs_cb0.bin";
                D3D11_BOX box{static_cast<UINT>(offset),0,0,static_cast<UINT>(offset+bytes),1,1};
                context->CopySubresourceRegion(sample.staging.Get(),0,0,0,0,source.Get(),0,&box);
            }
        }
    }
}
void GpuCapture::submit(ID3D11DeviceContext* context,uint64_t sequence,uint64_t sdk_frame,
                        uint64_t render_frame,uint64_t observation_session) {
    const auto cpu_begin=qpc_now();
    ComPtr<ID3D11Device> device;context->GetDevice(&device);
    const std::array<DXGI_FORMAT,3> formats={DXGI_FORMAT_R16G16B16A16_FLOAT,
        DXGI_FORMAT_R16G16B16A16_UINT,DXGI_FORMAT_R16G16B16A16_FLOAT};
    json descriptions=json::array();
    const char* names[]={"attributes0","attributes3","color"};
    for(size_t i=0;i<images_.size();++i) {
        auto& image=images_[i];image.source->GetDesc(&image.desc);
        auto desc=image.desc;
        if(desc.Format!=formats[i] || desc.SampleDesc.Count!=1 || desc.MipLevels!=1 || desc.ArraySize!=1 ||
           desc.Width!=images_[0].desc.Width || desc.Height!=images_[0].desc.Height)
            throw std::runtime_error("Unsupported or unaligned camera texture descriptors");
        if(options_.raw()) {
            desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.MiscFlags=0;
            prepare_staging(image,desc,device.Get());
            image.pixels.resize(static_cast<size_t>(desc.Width)*desc.Height*8);
            descriptions.push_back({{"file",camera_+"_"+names[i]+".bin"},
                {"width",desc.Width},{"height",desc.Height},{"format",i==1?"R16G16B16A16_UINT":"R16G16B16A16_FLOAT"},
                {"resource",reinterpret_cast<uintptr_t>(image.source.Get())},{"row_bytes",desc.Width*8}});
        }
    }
    if(options_.packed()) {
        if(options_.depth() && packed_.images[0].description.is_null()) throw std::runtime_error("RGB-D geometry depth was not captured");
        Com<ID3D11DeviceContext1> context1;
        check(context->QueryInterface(IID_PPV_ARGS(&context1)),"QueryInterface(DeviceContext1)");
        if(options_.color()) packed_.color(context1.Get(),images_[2].source.Get(),options_.color_gain,camera_);
        if(options_.preview()) packed_.color(context1.Get(),images_[2].source.Get(),options_.color_gain,camera_,true);
        for(const auto& image:packed_.images) if(!image.description.is_null()) descriptions.push_back(image.description);
        if(!packed_.lidar_description.is_null()) descriptions.push_back(packed_.lidar_description);
    }
    D3D11_QUERY_DESC query{D3D11_QUERY_EVENT,0};
    if(!completion_) {check(device->CreateQuery(&query,&completion_),"CreateQuery(EVENT)");++allocations_;}
    for(auto& image:images_) if(options_.raw()) context->CopyResource(image.staging.Get(),image.source.Get());
    context->End(completion_.Get());
    ++sequence_;
    metadata_={{"capture_sequence",sequence_},{"camera",camera_},{"capture_format",options_.format},{"phase","leaving_camera_composition"},
        {"geometry_pass",geometry_pass_},{"color_pass",color_pass_},
        {"geometry_gpu",geometry_gpu_},{"sensor_dimensions",{images_[2].desc.Width,images_[2].desc.Height}},
        {"render_frame_id",render_frame},{"frame_id_source","Present return intervals"},
        {"observation_session_qpc",observation_session},{"qpc_frequency",qpc_frequency()},
        {"copy_submission_qpc",cpu_begin},{"copy_submission_cpu_ticks",qpc_now()-cpu_begin},
        {"sdk_frame_hint",sdk_frame},{"geometry_sdk_frame_hint",geometry_sdk_},
        {"geometry_binding_sequence",geometry_binding_},{"copy_binding_sequence",sequence},
        {"context",reinterpret_cast<uintptr_t>(context)},{"render_thread_id",GetCurrentThreadId()},
        {"ordered_same_context",true},{"images",descriptions},{"source_row_order_preserved",true}};
    if(options_.shared_gpu) metadata_["shared_gpu"]=packed_.seal(context);
    phase_=Phase::waiting_gpu;
}
void GpuCapture::collect(ID3D11DeviceContext* context) {
    ++gpu_polls_;
    const auto ready=context->GetData(completion_.Get(),nullptr,0,D3D11_ASYNC_GETDATA_DONOTFLUSH);
    if(ready==S_FALSE) return;
    check(ready,"GetData(EVENT)");
    const auto cpu_begin=qpc_now();
    std::array<D3D11_MAPPED_SUBRESOURCE,3> mapped{};
    size_t count=0;
    for(;count<images_.size();++count) {
        if(!options_.raw()) continue;
        const auto result=context->Map(images_[count].staging.Get(),0,D3D11_MAP_READ,
                                       D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped[count]);
        if(result==DXGI_ERROR_WAS_STILL_DRAWING || FAILED(result)) {
            for(size_t i=0;i<count;++i) if(images_[i].staging) context->Unmap(images_[i].staging.Get(),0);
            if(result==DXGI_ERROR_WAS_STILL_DRAWING) return;
            check(result,"Map(staging)");
        }
    }
    for(size_t i=0;i<images_.size();++i) {
        if(!options_.raw()) continue;
        auto& image=images_[i];const auto row=static_cast<size_t>(image.desc.Width)*8;
        for(unsigned y=0;y<image.desc.Height;++y)
            std::memcpy(image.pixels.data()+y*row,static_cast<uint8_t*>(mapped[i].pData)+y*mapped[i].RowPitch,row);
        context->Unmap(image.staging.Get(),0);
    }
    if(options_.raw() && !geometry_depth_.pixels.empty()) {
        D3D11_MAPPED_SUBRESOURCE mapped_depth{};
        const auto result=context->Map(geometry_depth_.staging.Get(),0,D3D11_MAP_READ,
            D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped_depth);
        if(result==DXGI_ERROR_WAS_STILL_DRAWING) return;
        check(result,"Map(depth staging)");
        const auto row=static_cast<size_t>(geometry_depth_.desc.Width)*depth_pixel_bytes_;
        for(UINT y=0;y<geometry_depth_.desc.Height;++y)
            std::memcpy(geometry_depth_.pixels.data()+y*row,
                static_cast<const uint8_t*>(mapped_depth.pData)+y*mapped_depth.RowPitch,row);
        context->Unmap(geometry_depth_.staging.Get(),0);
    }
    if(options_.packed() && !packed_.collect(context)) return;
    if(!packed_.exposure_sample.is_null()) {
        metadata_["color_exposure"]=packed_.exposure_sample;
        for(auto& desc:metadata_["images"]) if(desc.contains("linear_gain")) desc["linear_gain"]=packed_.exposure_sample.at("linear_gain");
    }
    json constants_json=json::array();
    for(auto& sample:geometry_constants_) {
        if(!sample.bytes.empty()) {
            D3D11_MAPPED_SUBRESOURCE mapped_buffer{};
            const auto result=context->Map(sample.staging.Get(),0,D3D11_MAP_READ,
                D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped_buffer);
            if(result==DXGI_ERROR_WAS_STILL_DRAWING) return;
            check(result,"Map(constant staging)");
            std::memcpy(sample.bytes.data(),mapped_buffer.pData,sample.bytes.size());
            context->Unmap(sample.staging.Get(),0);
        }
        if(!sample.description.is_null()) constants_json.push_back(sample.description);
    }
    if(!metadata_["geometry_gpu"].is_null()) metadata_["geometry_gpu"]["constant_buffers"]=std::move(constants_json);
    json vehicle_json=json::array();
    for(auto& sample:vehicle_constants_) {
        if(!sample.bytes.empty()) {
            D3D11_MAPPED_SUBRESOURCE mapped_buffer{};
            const auto result=context->Map(sample.staging.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped_buffer);
            if(result==DXGI_ERROR_WAS_STILL_DRAWING) return;
            check(result,"Map(vehicle constant staging)");
            std::memcpy(sample.bytes.data(),mapped_buffer.pData,sample.bytes.size());
            context->Unmap(sample.staging.Get(),0);
        }
        if(!sample.description.is_null()) vehicle_json.push_back(sample.description);
    }
    if(geometry_pass_.contains("vehicles_at_compile"))
        metadata_["geometry_gpu"]["vehicle_constant_buffers"]=std::move(vehicle_json);
    metadata_["readback_ready_qpc"]=qpc_now();
    metadata_["readback_cpu_ticks"]=qpc_now()-cpu_begin;
    release_sources();phase_=Phase::ready;
}
void GpuCapture::append_bundle(json& views,std::vector<BundleBlob>& blobs) {
    std::lock_guard lock(mutex_);
    if(phase_!=Phase::ready) throw std::runtime_error("Camera CPU sample is not complete");
    views.push_back({{"camera",camera_},{"metadata",metadata_}});
    for(size_t i=0;i<images_.size();++i) if(!images_[i].pixels.empty())
        blobs.push_back({camera_,metadata_.at("images")[i].at("file").get<std::string>(),images_[i].pixels.data(),images_[i].pixels.size()});
    for(const auto& image:packed_.images) if(!image.description.is_null() && !options_.shared_gpu)
        blobs.push_back({camera_,image.description.at("file").get<std::string>(),image.pixels.data(),image.pixels.size()});
    if(!packed_.lidar_description.is_null()) blobs.push_back({camera_,packed_.lidar_description.at("file").get<std::string>(),packed_.lidar_pixels.data(),packed_.lidar_pixels.size()});
    if(!geometry_depth_.pixels.empty())
        blobs.push_back({camera_,metadata_.at("geometry_gpu").at("depth_texture").at("file").get<std::string>(),
            geometry_depth_.pixels.data(),geometry_depth_.pixels.size()});
    for(const auto& sample:geometry_constants_) if(!sample.bytes.empty())
        blobs.push_back({camera_,sample.description.at("file").get<std::string>(),sample.bytes.data(),sample.bytes.size()});
    for(const auto& sample:vehicle_constants_) if(!sample.bytes.empty())
        blobs.push_back({camera_,sample.description.at("file").get<std::string>(),sample.bytes.data(),sample.bytes.size()});
}
json GpuCapture::save() {
    if(phase_!=Phase::ready) throw std::runtime_error("No completed camera CPU sample to save");
    if(!saved_.empty()) return status();
    // GetTickCount64 also distinguishes SDK reloads within one game PID.
    const auto directory=log_directory()/(camera_+"-"+std::to_string(GetTickCount64()));
    if(!fs::create_directory(directory)) throw std::runtime_error("Capture output directory already exists");
    for(size_t i=0;i<images_.size();++i) {
        if(images_[i].pixels.empty()) continue;
        std::ofstream output(directory/metadata_["images"][i]["file"].get<std::string>(),std::ios::binary);
        output.exceptions(std::ios::badbit|std::ios::failbit);
        output.write(reinterpret_cast<const char*>(images_[i].pixels.data()),images_[i].pixels.size());
        output.close();
    }
    for(const auto& image:packed_.images) if(!image.description.is_null()) {
        std::ofstream output(directory/image.description.at("file").get<std::string>(),std::ios::binary);
        output.exceptions(std::ios::badbit|std::ios::failbit);
        output.write(reinterpret_cast<const char*>(image.pixels.data()),image.pixels.size());output.close();
    }
    if(!packed_.lidar_description.is_null()) {
        std::ofstream output(directory/packed_.lidar_description.at("file").get<std::string>(),std::ios::binary);
        output.exceptions(std::ios::badbit|std::ios::failbit);
        output.write(reinterpret_cast<const char*>(packed_.lidar_pixels.data()),packed_.lidar_pixels.size());output.close();
    }
    for(const auto& sample:geometry_constants_) if(!sample.bytes.empty()) {
        std::ofstream output(directory/sample.description.at("file").get<std::string>(),std::ios::binary);
        output.exceptions(std::ios::badbit|std::ios::failbit);
        output.write(reinterpret_cast<const char*>(sample.bytes.data()),sample.bytes.size());
        output.close();
    }
    for(const auto& sample:vehicle_constants_) if(!sample.bytes.empty()) {
        std::ofstream output(directory/sample.description.at("file").get<std::string>(),std::ios::binary);
        output.exceptions(std::ios::badbit|std::ios::failbit);
        output.write(reinterpret_cast<const char*>(sample.bytes.data()),sample.bytes.size());
        output.close();
    }
    if(!geometry_depth_.pixels.empty()) {
        std::ofstream output(directory/metadata_.at("geometry_gpu").at("depth_texture").at("file").get<std::string>(),std::ios::binary);
        output.exceptions(std::ios::badbit|std::ios::failbit);
        output.write(reinterpret_cast<const char*>(geometry_depth_.pixels.data()),geometry_depth_.pixels.size());
        output.close();
    }
    // Publish metadata last; incomplete files are not presented as a saved capture.
    std::ofstream output(directory/"images.json");output.exceptions(std::ios::badbit|std::ios::failbit);
    output<<metadata_.dump(2);output.close();saved_=directory;
    return status();
}
}
