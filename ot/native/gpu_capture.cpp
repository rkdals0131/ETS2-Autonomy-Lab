#include "gpu_capture.hpp"
#include <fstream>
#include <stdexcept>

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

void GpuCapture::release_gpu() {
    for(auto& image:images_) {image.source.Reset();image.staging.Reset();}
    completion_.Reset();context_.Reset();
}
void GpuCapture::cancel() noexcept {
    try {
        std::lock_guard lock(mutex_);
        release_gpu();
        // A completed CPU sample remains available for saving after panic.
        if(phase_==Phase::armed || phase_==Phase::waiting_gpu) phase_=Phase::idle;
    } catch(...) {}
}
json GpuCapture::status() const {
    const char* phase=phase_==Phase::idle?"idle":phase_==Phase::armed?"armed":
        phase_==Phase::waiting_gpu?"waiting_gpu":phase_==Phase::ready?"ready":"error";
    return {{"phase",phase},{"capture_sequence",sequence_},{"bindings_seen",bindings_seen_},
            {"camera",camera_},{"requested_frame_id",requested_frame_},{"last_label",last_label_},
            {"gpu_polls",gpu_polls_},{"error",error_},
            {"metadata",metadata_},{"saved_directory",saved_.string()}};
}
json GpuCapture::command(const std::string& action,uint64_t requested_frame) {
    std::lock_guard lock(mutex_);
    if(action=="arm") {
        if(phase_==Phase::armed || phase_==Phase::waiting_gpu)
            throw std::runtime_error("A camera capture is already pending");
        release_gpu();
        for(auto& image:images_) image.pixels.clear();
        metadata_=json::object();error_.clear();last_label_.clear();saved_.clear();
        geometry_binding_=geometry_sdk_=gpu_polls_=bindings_seen_=0;
        geometry_pass_=color_pass_=nullptr;requested_frame_=requested_frame;
        request_started_=GetTickCount64();phase_=Phase::armed;
    } else if(action=="cancel") {
        release_gpu();if(phase_!=Phase::ready) phase_=Phase::idle;
    } else if(action=="save") return save();
    else if(action!="status") throw std::runtime_error("Unknown camera capture action");
    return status();
}
void GpuCapture::observe(ID3D11DeviceContext* context,uint32_t count,const uintptr_t* targets,
                         uint64_t binding_sequence,uint64_t sdk_frame,uint64_t render_frame,
                         uint64_t observation_session,const json* pass) noexcept {
    std::unique_lock lock(mutex_,std::try_to_lock);
    if(!lock || (phase_!=Phase::armed && phase_!=Phase::waiting_gpu)) return;
    try {
        ++bindings_seen_;
        if(GetTickCount64()-request_started_>30000) throw std::runtime_error("Camera capture timed out after 30 seconds");
        if(!context || context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)
            throw std::runtime_error("Camera readback requires the observed immediate context");
        if(phase_==Phase::waiting_gpu) {
            if(context!=context_.Get()) return;
            collect(context);return;
        }
        if(!render_frame || (requested_frame_ && render_frame<requested_frame_)) return;
        if(requested_frame_ && render_frame>requested_frame_)
            throw std::runtime_error(camera_+" did not finish rendering in the requested Present interval");
        auto next=count?texture(targets[0]):ComPtr<ID3D11Texture2D>{};
        // Copy before a new group overwrites the shared G-buffer. Unbinding or
        // switching to multiple targets also ends the outgoing color pass.
        if(images_[2].source && context==context_.Get() &&
           (count!=1 || next.Get()!=images_[2].source.Get())) {
            ComPtr<ID3D11RenderTargetView> current;
            context->OMGetRenderTargets(1,&current,nullptr);
            auto current_texture=texture(reinterpret_cast<uintptr_t>(current.Get()));
            if(current_texture.Get()==images_[2].source.Get() && geometry_frame_==render_frame) {
                submit(context,binding_sequence,sdk_frame,render_frame,observation_session);
                return;
            }
            release_gpu();
        }
        const auto label=target_name(pass,0);
        if(!label.empty()) last_label_=label;
        if(count==4) {
            release_gpu();
            if(label!=camera_+"/attributes_0" || target_name(pass,3)!=camera_+"/attributes_3") return;
            auto flags=texture(targets[3]);
            if(!next || !flags) return;
            images_[0].source=std::move(next);images_[1].source=std::move(flags);
            context_=context;geometry_binding_=binding_sequence;geometry_sdk_=sdk_frame;geometry_frame_=render_frame;
            geometry_pass_=*pass;
        } else if(count==1 && images_[0].source && context==context_.Get() && label==camera_+"/composition_raw") {
            images_[2].source=std::move(next);color_pass_=*pass;
        }
    } catch(const std::exception& e) {error_=e.what();release_gpu();phase_=Phase::error;}
    catch(...) {error_="Camera readback failed";release_gpu();phase_=Phase::error;}
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
        desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.MiscFlags=0;
        check(device->CreateTexture2D(&desc,nullptr,&image.staging),"CreateTexture2D(staging)");
        image.pixels.resize(static_cast<size_t>(desc.Width)*desc.Height*8);
        descriptions.push_back({{"file",camera_+"_"+names[i]+".bin"},
            {"width",desc.Width},{"height",desc.Height},{"format",i==1?"R16G16B16A16_UINT":"R16G16B16A16_FLOAT"},
            {"resource",reinterpret_cast<uintptr_t>(image.source.Get())},{"row_bytes",desc.Width*8}});
    }
    D3D11_QUERY_DESC query{D3D11_QUERY_EVENT,0};
    check(device->CreateQuery(&query,&completion_),"CreateQuery(EVENT)");
    for(auto& image:images_) context->CopyResource(image.staging.Get(),image.source.Get());
    context->End(completion_.Get());
    ++sequence_;
    metadata_={{"capture_sequence",sequence_},{"camera",camera_},{"phase","leaving_camera_composition"},
        {"geometry_pass",geometry_pass_},{"color_pass",color_pass_},
        {"render_frame_id",render_frame},{"frame_id_source","Present return intervals"},
        {"observation_session_qpc",observation_session},{"qpc_frequency",qpc_frequency()},
        {"copy_submission_qpc",cpu_begin},{"copy_submission_cpu_ticks",qpc_now()-cpu_begin},
        {"sdk_frame_hint",sdk_frame},{"geometry_sdk_frame_hint",geometry_sdk_},
        {"geometry_binding_sequence",geometry_binding_},{"copy_binding_sequence",sequence},
        {"context",reinterpret_cast<uintptr_t>(context)},{"render_thread_id",GetCurrentThreadId()},
        {"ordered_same_context",true},{"images",descriptions},{"source_row_order_preserved",true}};
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
        const auto result=context->Map(images_[count].staging.Get(),0,D3D11_MAP_READ,
                                       D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped[count]);
        if(result==DXGI_ERROR_WAS_STILL_DRAWING || FAILED(result)) {
            for(size_t i=0;i<count;++i) context->Unmap(images_[i].staging.Get(),0);
            if(result==DXGI_ERROR_WAS_STILL_DRAWING) return;
            check(result,"Map(staging)");
        }
    }
    for(size_t i=0;i<images_.size();++i) {
        auto& image=images_[i];const auto row=static_cast<size_t>(image.desc.Width)*8;
        for(unsigned y=0;y<image.desc.Height;++y)
            std::memcpy(image.pixels.data()+y*row,static_cast<uint8_t*>(mapped[i].pData)+y*mapped[i].RowPitch,row);
        context->Unmap(image.staging.Get(),0);
    }
    metadata_["readback_ready_qpc"]=qpc_now();
    metadata_["readback_cpu_ticks"]=qpc_now()-cpu_begin;
    release_gpu();phase_=Phase::ready;
}
json GpuCapture::save() {
    if(phase_!=Phase::ready) throw std::runtime_error("No completed camera CPU sample to save");
    if(!saved_.empty()) return status();
    // GetTickCount64 also distinguishes SDK reloads within one game PID.
    const auto directory=log_directory()/(camera_+"-"+std::to_string(GetTickCount64()));
    if(!fs::create_directory(directory)) throw std::runtime_error("Capture output directory already exists");
    for(size_t i=0;i<images_.size();++i) {
        std::ofstream output(directory/metadata_["images"][i]["file"].get<std::string>(),std::ios::binary);
        output.exceptions(std::ios::badbit|std::ios::failbit);
        output.write(reinterpret_cast<const char*>(images_[i].pixels.data()),images_[i].pixels.size());
        output.close();
    }
    // Publish metadata last; incomplete files are not presented as a saved capture.
    std::ofstream output(directory/"images.json");output.exceptions(std::ios::badbit|std::ios::failbit);
    output<<metadata_.dump(2);output.close();saved_=directory;
    return status();
}
}
