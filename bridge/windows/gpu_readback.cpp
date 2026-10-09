#include "gpu_readback.hpp"
#include <dxgi1_4.h>
#include <chrono>
#include <thread>

namespace bridge {
namespace {
using Microsoft::WRL::ComPtr;
void check(HRESULT hr,const char* operation) {
    if(FAILED(hr)) throw std::runtime_error(std::string(operation)+" HRESULT="+std::to_string(hr));
}
struct Handle {
    HANDLE value{};
    ~Handle(){if(value) CloseHandle(value);}
};
void duplicate(HANDLE process,uint64_t original,Handle& result) {
    if(!DuplicateHandle(process,reinterpret_cast<HANDLE>(original),GetCurrentProcess(),&result.value,0,FALSE,DUPLICATE_SAME_ACCESS))
        throw std::runtime_error("Cannot duplicate shared GPU handle");
}
}
void GpuReadback::read(SensorBundle& bundle,DWORD producer_pid,bool map_to_cpu) {
    const uint64_t stream=bundle.manifest.at("stream_id");
    if(stream_!=stream) {slots_.clear();stream_=stream;}
    Handle process;
    struct Pending {Texture* texture;size_t offset,row,height;};
    std::vector<Pending> pending;
    pending.reserve(bundle.manifest.at("views").size()*3);
    bool submitted=false;
    for(auto& view:bundle.manifest.at("views")) {
        auto& metadata=view.at("metadata");
        if(!metadata.contains("shared_gpu") || metadata.at("shared_gpu").is_null()) continue;
        const auto& gpu=metadata.at("shared_gpu");
        if(gpu.at("pid").get<DWORD>()!=producer_pid) throw std::runtime_error("GPU producer differs from the IPC owner");
        const LUID luid{gpu.at("adapter_low").get<DWORD>(),gpu.at("adapter_high").get<LONG>()};
        if(!device_ || adapter_.LowPart!=luid.LowPart || adapter_.HighPart!=luid.HighPart) {
            if(submitted) throw std::runtime_error("Shared cameras use different GPU adapters");
            slots_.clear();context_.Reset();device_.Reset();
            ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter> adapter;
            check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"CreateDXGIFactory");
            check(factory->EnumAdapterByLuid(luid,IID_PPV_ARGS(&adapter)),"Find game adapter");
            ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
            const D3D_FEATURE_LEVEL level=D3D_FEATURE_LEVEL_11_1;
            check(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,&level,1,D3D11_SDK_VERSION,&device,nullptr,&context),"Create relay D3D11 device");
            check(device.As(&device_),"Relay Device5");check(context.As(&context_),"Relay Context4");adapter_=luid;
        }
        auto& slot=slots_[gpu.at("id").get<uint64_t>()];
        auto open_process=[&] {
            if(!process.value) process.value=OpenProcess(PROCESS_DUP_HANDLE,FALSE,producer_pid);
            if(!process.value) throw std::runtime_error("Cannot open GPU producer for handle duplication");
        };
        if(!slot.fence) {
            open_process();Handle handle;duplicate(process.value,gpu.at("handle"),handle);
            check(device_->OpenSharedFence(handle.value,IID_PPV_ARGS(&slot.fence)),"OpenSharedFence");
        }
        const uint64_t ready=gpu.at("ready"),released=gpu.at("released");
        if(released!=ready+1) throw std::runtime_error("Invalid GPU ownership fence values");
        // Bound waits on this independent worker. A dead/paused game never blocks
        // state publication or the game's rendering thread.
        if(slot.fence->GetCompletedValue()<ready) {
            if(!completion_) completion_=CreateEventW(nullptr,FALSE,FALSE,nullptr);
            if(!completion_) throw std::runtime_error("Cannot create GPU completion event");
            check(slot.fence->SetEventOnCompletion(ready,completion_),"Wait shared pack fence");
            if(WaitForSingleObject(completion_,2000)!=WAIT_OBJECT_0) throw std::runtime_error("Shared GPU pack timed out");
        }
        for(auto& image:metadata.at("images")) {
            if(!image.contains("shared_texture")) continue;
            const auto& shared=image.at("shared_texture");const std::string file=image.at("file");
            auto& [id,texture]=slot.textures[file];
            const uint64_t next=shared.at("id");
            if(id!=next || !texture.source) {
                texture=Texture{};open_process();Handle handle;duplicate(process.value,shared.at("handle"),handle);
                check(device_->OpenSharedResource1(handle.value,IID_PPV_ARGS(&texture.source)),"OpenSharedResource1");
                texture.source->GetDesc(&texture.desc);const auto& d=texture.desc;
                if(d.Width!=image.at("width") || d.Height!=image.at("height") || image.at("row_bytes")!=d.Width*4 ||
                   (d.Format!=DXGI_FORMAT_R32_FLOAT && d.Format!=DXGI_FORMAT_R8G8B8A8_UNORM) || d.SampleDesc.Count!=1 || d.MipLevels!=1 || d.ArraySize!=1)
                    throw std::runtime_error("Unsupported shared image layout");
                auto staging=d;staging.Usage=D3D11_USAGE_STAGING;staging.BindFlags=staging.MiscFlags=0;staging.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                check(device_->CreateTexture2D(&staging,nullptr,&texture.staging),"Create relay staging");id=next;
            }
            const size_t row=texture.desc.Width*size_t{4},height=texture.desc.Height,offset=bundle.data.size();
            context_->CopyResource(texture.staging.Get(),texture.source.Get());
            if(map_to_cpu) {
                if(offset+row*height>64*1024*1024) throw std::runtime_error("Shared bundle exceeds relay capacity");
                bundle.data.resize(offset+row*height);
                bundle.manifest["files"].push_back({{"file",file},{"camera",view.at("camera")},{"offset",offset-bundle.blob_offset},{"length",row*height}});
                pending.push_back({&texture,offset,row,height});
            }
        }
        // Copy commands precede release on this context. CPU mapping reads our
        // staging, so the producer can already reuse the shared output afterwards.
        check(context_->Signal(slot.fence.Get(),released),"Release shared GPU sample");submitted=true;
    }
    // Submit all cameras before any blocking Map. Per-camera release signals
    // remain ordered after their copies; one flush submits the whole bundle.
    if(submitted) context_->Flush();
    for(const auto& item:pending) {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            check(context_->Map(item.texture->staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Map relay staging");
            for(size_t y=0;y<item.height;++y)
                std::memcpy(bundle.data.data()+item.offset+y*item.row,static_cast<uint8_t*>(mapped.pData)+y*mapped.RowPitch,item.row);
            context_->Unmap(item.texture->staging.Get(),0);
    }
}
}
