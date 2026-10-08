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
struct ArrayHeader { uintptr_t vtable,data; uint64_t size,capacity; };
std::string game_string(uintptr_t address) {
    std::array<char,384> text{};
    if(!address || !copy_memory(address,text.data(),text.size())) return {};
    const auto* end=static_cast<const char*>(std::memchr(text.data(),0,text.size()));
    return end?std::string(text.data(),static_cast<size_t>(end-text.data())):std::string{};
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

std::string GpuCapture::graph_name(ID3D11Resource* resource) {
    const auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    uintptr_t renderer{};ArrayHeader pool{};
    if(!read_memory(base+layout_["renderer_pointer_rva"].get<uintptr_t>(),renderer) || !renderer ||
       !read_memory(renderer+layout_["image_pool_offset"].get<uintptr_t>(),pool) ||
       !pool.data || pool.size>pool.capacity) return {};
    const auto manager=layout_["manager_rva"].get<uintptr_t>();
    const auto buffer_stride=layout_["buffer_stride"].get<uintptr_t>();
    const auto image_stride=layout_["image_stride"].get<uintptr_t>();
    const auto image_id_offset=layout_["image_id_offset"].get<uintptr_t>();
    const auto pool_stride=layout_["pool_stride"].get<uintptr_t>();
    const auto resource_offset=layout_["resource_offset"].get<uintptr_t>();
    {
    std::lock_guard names_lock(names_mutex_);
    for(const auto& [id,label]:image_labels_) {
        uintptr_t candidate{};
        if(id<pool.size && read_memory(pool.data+id*pool_stride+resource_offset,candidate) &&
           candidate==reinterpret_cast<uintptr_t>(resource)) return label;
    }
    }
    for(unsigned buffer=0;buffer<3;++buffer) {
        ArrayHeader graph{};
        if(!read_memory(base+manager+buffer*buffer_stride,graph) ||
           !graph.data || graph.size>graph.capacity || graph.size>4096) continue;
        for(uint64_t index=0;index<graph.size;++index) {
            const auto entry=graph.data+index*image_stride;
            uint16_t id{};uintptr_t candidate{},name{},space{};
            if(!read_memory(entry+image_id_offset,id) ||
               id==0xffff || id>=pool.size ||
               !read_memory(pool.data+id*pool_stride+resource_offset,candidate) ||
               candidate!=reinterpret_cast<uintptr_t>(resource)) continue;
            if(!read_memory(entry+layout_["name_offset"].get<uintptr_t>(),name) ||
               !read_memory(entry+layout_["namespace_offset"].get<uintptr_t>(),space)) continue;
            const auto ns=game_string(space),label=game_string(name);
            // Other mirrors share physical resources. This first capture only
            // accepts the inspected mirror5 namespace and its named output.
            if(ns=="mirror5" || (ns=="drawable" && label.find("/material/environment/front_mirror_reflection.tobj")!=std::string::npos))
                return ns+"/"+label;
        }
    }
    return {};
}
void GpuCapture::label_image(uintptr_t image,uintptr_t image_id_address) noexcept {
    if(!label_tracking_.load()) return;
    try {
        uint16_t id{};uintptr_t name{},space{};
        if(!read_memory(image_id_address,id) || id==0xffff ||
           !read_memory(image+layout_["name_offset"].get<uintptr_t>(),name) ||
           !read_memory(image+layout_["namespace_offset"].get<uintptr_t>(),space)) return;
        const auto ns=game_string(space),label=game_string(name);
        std::lock_guard names_lock(names_mutex_);
        if(!label_tracking_.load()) return;
        ++label_callbacks_;
        if(ns=="mirror5") image_labels_[id]=ns+"/"+label;
        else image_labels_.erase(id); // A reused pool ID must lose its old camera name.
    } catch(...) {}
}
void GpuCapture::release_gpu() {
    for(auto& image:images_) {image.source.Reset();image.staging.Reset();}
    completion_.Reset();context_.Reset();
}
void GpuCapture::cancel() noexcept {
    try {
        std::lock_guard lock(mutex_);
        label_tracking_=false;
        release_gpu();
        // A completed CPU sample remains available for saving after panic.
        if(phase_==Phase::armed || phase_==Phase::waiting_gpu) phase_=Phase::idle;
    } catch(...) {}
}
json GpuCapture::status() const {
    std::lock_guard names_lock(names_mutex_);
    const char* phase=phase_==Phase::idle?"idle":phase_==Phase::armed?"armed":
        phase_==Phase::waiting_gpu?"waiting_gpu":phase_==Phase::ready?"ready":"error";
    return {{"phase",phase},{"capture_sequence",sequence_},{"bindings_seen",bindings_seen_},
            {"last_label",last_label_},{"label_callbacks",label_callbacks_},{"image_labels",image_labels_},
            {"gpu_polls",gpu_polls_},{"error",error_},
            {"metadata",metadata_},{"saved_directory",saved_.string()}};
}
json GpuCapture::command(const std::string& action) {
    std::lock_guard lock(mutex_);
    if(action=="arm") {
        if(phase_==Phase::armed || phase_==Phase::waiting_gpu)
            throw std::runtime_error("A mirror5 capture is already pending");
        release_gpu();
        for(auto& image:images_) image.pixels.clear();
        metadata_=json::object();error_.clear();last_label_.clear();saved_.clear();
        geometry_binding_=geometry_sdk_=gpu_polls_=bindings_seen_=0;
        {
            std::lock_guard names_lock(names_mutex_);
            label_callbacks_=0;image_labels_.clear();
        }
        request_started_=GetTickCount64();phase_=Phase::armed;
        label_tracking_=true;
    } else if(action=="cancel") {
        label_tracking_=false;
        release_gpu();if(phase_!=Phase::ready) phase_=Phase::idle;
    } else if(action=="save") return save();
    else if(action!="status") throw std::runtime_error("Unknown mirror5 capture action");
    return status();
}
void GpuCapture::observe(ID3D11DeviceContext* context,uint32_t count,const uintptr_t* targets,
                         uint64_t binding_sequence,uint64_t sdk_frame,uint64_t render_frame,
                         uint64_t observation_session) noexcept {
    std::unique_lock lock(mutex_,std::try_to_lock);
    if(!lock || (phase_!=Phase::armed && phase_!=Phase::waiting_gpu)) return;
    try {
        ++bindings_seen_;
        if(GetTickCount64()-request_started_>30000) throw std::runtime_error("Mirror5 capture timed out after 30 seconds");
        if(!context || context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)
            throw std::runtime_error("Mirror5 readback requires the observed immediate context");
        if(phase_==Phase::waiting_gpu) {
            if(context!=context_.Get()) return;
            collect(context);return;
        }
        if(!render_frame) return; // Wait for the first observed Present boundary.
        if(count==4) {
            // Every new G-buffer group ends the previous candidate sequence.
            release_gpu();
            auto first=texture(targets[0]);
            auto label=first?graph_name(first.Get()):std::string{};
            if(!label.empty()) last_label_=label;
            if(label!="mirror5/attributes_0") return;
            auto flags=texture(targets[3]);
            if(!flags || graph_name(flags.Get())!="mirror5/attributes_3") return;
            images_[0].source=std::move(first);images_[1].source=std::move(flags);
            context_=context;geometry_binding_=binding_sequence;geometry_sdk_=sdk_frame;geometry_frame_=render_frame;
        } else if(count==1 && images_[0].source && context==context_.Get()) {
            auto next=texture(targets[0]);
            if(!next) return;
            auto label=graph_name(next.Get());
            if(!label.empty()) last_label_=label;
            if(label=="mirror5/composition_raw") images_[2].source=std::move(next);
            else if(images_[2].source && next.Get()!=images_[2].source.Get()) {
                ComPtr<ID3D11RenderTargetView> current;
                context->OMGetRenderTargets(1,&current,nullptr);
                auto current_texture=texture(reinterpret_cast<uintptr_t>(current.Get()));
                if(current_texture.Get()!=images_[2].source.Get()) return;
                if(geometry_frame_!=render_frame) {
                    release_gpu();return; // Never combine a G-buffer from a previous interval.
                }
                submit(context,binding_sequence,sdk_frame,render_frame,observation_session);
            }
        }
    } catch(const std::exception& e) {label_tracking_=false;error_=e.what();release_gpu();phase_=Phase::error;}
    catch(...) {label_tracking_=false;error_="Mirror5 readback failed";release_gpu();phase_=Phase::error;}
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
            throw std::runtime_error("Unsupported or unaligned mirror5 texture descriptors");
        desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.MiscFlags=0;
        check(device->CreateTexture2D(&desc,nullptr,&image.staging),"CreateTexture2D(staging)");
        image.pixels.resize(static_cast<size_t>(desc.Width)*desc.Height*8);
        descriptions.push_back({{"file",std::string("mirror5_")+names[i]+".bin"},
            {"width",desc.Width},{"height",desc.Height},{"format",i==1?"R16G16B16A16_UINT":"R16G16B16A16_FLOAT"},
            {"resource",reinterpret_cast<uintptr_t>(image.source.Get())},{"row_bytes",desc.Width*8}});
    }
    D3D11_QUERY_DESC query{D3D11_QUERY_EVENT,0};
    check(device->CreateQuery(&query,&completion_),"CreateQuery(EVENT)");
    for(auto& image:images_) context->CopyResource(image.staging.Get(),image.source.Get());
    context->End(completion_.Get());
    ++sequence_;
    metadata_={{"capture_sequence",sequence_},{"camera","mirror5"},{"phase","leaving_mirror5_composition"},
        {"render_frame_id",render_frame},{"frame_id_source","Present return intervals"},
        {"observation_session_qpc",observation_session},{"qpc_frequency",qpc_frequency()},
        {"copy_submission_qpc",cpu_begin},{"copy_submission_cpu_ticks",qpc_now()-cpu_begin},
        {"sdk_frame_hint",sdk_frame},{"geometry_sdk_frame_hint",geometry_sdk_},
        {"geometry_binding_sequence",geometry_binding_},{"copy_binding_sequence",sequence},
        {"context",reinterpret_cast<uintptr_t>(context)},{"render_thread_id",GetCurrentThreadId()},
        {"ordered_same_context",true},{"images",descriptions},{"source_row_order_preserved",true}};
    label_tracking_=false;phase_=Phase::waiting_gpu;
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
    if(phase_!=Phase::ready) throw std::runtime_error("No completed mirror5 CPU sample to save");
    if(!saved_.empty()) return status();
    // GetTickCount64 also distinguishes SDK reloads within one game PID.
    const auto directory=log_directory()/("mirror5-"+std::to_string(GetTickCount64()));
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
