#include "gpu_pack.hpp"
#include "pack_depth.hpp"
#include "pack_color.hpp"
#include "gather_lidar.hpp"
#include "auto_exposure.hpp"
#include <set>
#include <numbers>
#include <stdexcept>
#include <DirectXMath.h>
#include <dxgi1_2.h>

namespace ot {
namespace {
template<class T> using Com=Microsoft::WRL::ComPtr<T>;
void check(HRESULT hr,const char* operation) {
    if(FAILED(hr)) throw std::runtime_error(std::string(operation)+" HRESULT="+std::to_string(hr));
}
// Save precisely the compute bindings changed by the pack pass, including
// constant-buffer ranges, dynamic shader linkage and append/consume counters.
struct ComputeState {
    ID3D11DeviceContext1* context;
    Com<ID3D11ComputeShader> shader;
    std::array<ID3D11ClassInstance*,D3D11_SHADER_MAX_INTERFACES> instances{};
    UINT instance_count=static_cast<UINT>(instances.size()),first{},count{};
    std::array<ID3D11ShaderResourceView*,4> resources{};
    Com<ID3D11UnorderedAccessView> output;
    Com<ID3D11Buffer> constants;
    explicit ComputeState(ID3D11DeviceContext1* c):context(c) {
        context->CSGetShader(&shader,instances.data(),&instance_count);
        context->CSGetShaderResources(0,4,resources.data());
        context->CSGetUnorderedAccessViews(0,1,&output);
        context->CSGetConstantBuffers1(0,1,&constants,&first,&count);
    }
    ~ComputeState() {
        ID3D11UnorderedAccessView* null_output{};
        context->CSSetUnorderedAccessViews(0,1,&null_output,nullptr);
        context->CSSetShaderResources(0,4,resources.data());
        const UINT keep=UINT(-1);
        context->CSSetUnorderedAccessViews(0,1,output.GetAddressOf(),&keep);
        context->CSSetConstantBuffers1(0,1,constants.GetAddressOf(),&first,&count);
        context->CSSetShader(shader.Get(),instances.data(),instance_count);
        for(auto* p:resources) if(p) p->Release();
        for(UINT i=0;i<instance_count;++i) if(instances[i]) instances[i]->Release();
    }
};
}
void ExposureState::update(ID3D11DeviceContext1* context,ID3D11ShaderResourceView* source,float initial_gain) {
    Com<ID3D11Device> current;context->GetDevice(&current);
    if(current.Get()!=device.Get()) {*this=ExposureState{};device=current;}
    if(!value) {
        D3D11_BUFFER_DESC desc{};desc.ByteWidth=16;desc.Usage=D3D11_USAGE_DEFAULT;
        desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;desc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;desc.StructureByteStride=16;
        std::array<float,4> first{initial_gain,0,0,0};D3D11_SUBRESOURCE_DATA data{first.data(),0,0};
        check(device->CreateBuffer(&desc,&data,&value),"Create exposure state");
        check(device->CreateShaderResourceView(value.Get(),nullptr,&view),"Create exposure SRV");
        check(device->CreateUnorderedAccessView(value.Get(),nullptr,&output),"Create exposure UAV");
        desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;desc.MiscFlags=desc.StructureByteStride=0;
        check(device->CreateBuffer(&desc,nullptr,&constants),"Create exposure constants");
        check(device->CreateComputeShader(ot_auto_exposure,sizeof(ot_auto_exposure),nullptr,&shader),"Create exposure shader");
    }
    const auto now=qpc_now();const float dt=previous?static_cast<float>(std::min(1.0,double(now-previous)/qpc_frequency())):0;
    previous=now;std::array<float,4> parameters{dt,0,0,0};
    context->UpdateSubresource(constants.Get(),0,nullptr,parameters.data(),0,0);
    ComputeState restore(context);
    ID3D11ShaderResourceView* inputs[]{source,nullptr,nullptr,nullptr};
    context->CSSetShaderResources(0,4,inputs);context->CSSetUnorderedAccessViews(0,1,output.GetAddressOf(),nullptr);
    context->CSSetConstantBuffers(0,1,constants.GetAddressOf());context->CSSetShader(shader.Get(),nullptr,0);context->Dispatch(1,1,1);
}
void GpuPack::release_gpu() {
    exposure_staging_.Reset();
    fence_handle_.reset();fence_.Reset();fence_value_=0;awaiting_reader_=false;
    lidar_=LidarWork{};
    for(auto& image:images) image.staging.Reset();
    for(auto& work:work_) work=Work{};
    device_.Reset();
}
std::shared_ptr<const LidarPattern> make_lidar_pattern(const json& config) {
    auto result=std::make_shared<LidarPattern>();
    const auto az=config.at("azimuth_deg").get<std::array<double,3>>();
    const auto matrix=config.at("camera_from_sensor").get<std::array<double,9>>();
    const double maximum=config.at("max_range");
    if(!std::isfinite(maximum) || maximum<=0 || !std::isfinite(az[0]) || !std::isfinite(az[1]) ||
       !std::isfinite(az[2]) || az[2]<=0 || az[1]<az[0]) throw std::runtime_error("Invalid LiDAR beam extent");
    for(double x:matrix) if(!std::isfinite(x)) throw std::runtime_error("Invalid LiDAR source rotation");
    const auto columns=static_cast<size_t>(std::floor((az[1]-az[0])/az[2]+1e-7))+1;
    std::set<double> elevations;
    for(const auto& band:config.at("elevation_bands_deg")) {
        const double lo=band.at(0),hi=band.at(1);const auto count=band.at(2).get<size_t>();
        if(!std::isfinite(lo) || !std::isfinite(hi) || count<2 || count>1024 || hi<lo)
            throw std::runtime_error("Invalid LiDAR elevation band");
        for(size_t i=0;i<count;++i) elevations.insert(lo+(hi-lo)*i/(count-1));
    }
    // Bound the external pattern by the buffer/API capacity, before allocating.
    if(elevations.empty() || columns>65536 || columns*elevations.size()>1048576)
        throw std::runtime_error("LiDAR pattern exceeds the gather buffer capacity");
    constexpr double radians=std::numbers::pi/180;
    for(double elevation:elevations) for(size_t x=0;x<columns;++x) {
        const double a=(az[0]+x*az[2])*radians,e=elevation*radians;
        const std::array<double,3> direction{std::cos(e)*std::cos(a),std::cos(e)*std::sin(a),std::sin(e)};
        std::array<double,3> source{};
        for(size_t i=0;i<3;++i) for(size_t j=0;j<3;++j) source[i]+=matrix[i*3+j]*direction[j];
        const auto norm=std::sqrt(source[0]*source[0]+source[1]*source[1]+source[2]*source[2]);
        if(!std::isfinite(norm) || norm==0) throw std::runtime_error("Degenerate LiDAR direction transform");
        result->directions.push_back({static_cast<float>(source[0]/norm),static_cast<float>(source[1]/norm),
            static_cast<float>(source[2]/norm),static_cast<float>(maximum)});
    }
    result->description=config;result->description["elevations_deg"]=elevations;
    result->description["columns"]=columns;result->description["beam_count"]=result->directions.size();
    return result;
}
void GpuPack::gather(ID3D11DeviceContext1* context,const std::shared_ptr<const LidarPattern>& pattern,
                     const json& projection,const D3D11_VIEWPORT& vp,const std::string& camera) {
    auto& work=lidar_;const auto count=static_cast<UINT>(pattern->directions.size());
    if(!work.pattern || work.pattern->description!=pattern->description) {
        work=LidarWork{};work.pattern=pattern;
        D3D11_BUFFER_DESC desc{};desc.ByteWidth=count*16;desc.Usage=D3D11_USAGE_DEFAULT;
        desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;desc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;desc.StructureByteStride=16;
        D3D11_SUBRESOURCE_DATA data{pattern->directions.data(),0,0};
        check(device_->CreateBuffer(&desc,&data,&work.beams),"CreateBuffer(lidar directions)");++allocations_;
        check(device_->CreateShaderResourceView(work.beams.Get(),nullptr,&work.directions),"CreateSRV(lidar directions)");++allocations_;
        desc.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
        check(device_->CreateBuffer(&desc,nullptr,&work.output),"CreateBuffer(lidar returns)");++allocations_;
        check(device_->CreateUnorderedAccessView(work.output.Get(),nullptr,&work.uav),"CreateUAV(lidar returns)");++allocations_;
        desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        check(device_->CreateBuffer(&desc,nullptr,&work.staging),"CreateBuffer(lidar staging)");++allocations_;
        desc={};desc.ByteWidth=96;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        check(device_->CreateBuffer(&desc,nullptr,&work.constants),"CreateBuffer(lidar constants)");++allocations_;
        check(device_->CreateComputeShader(ot_gather_lidar,sizeof(ot_gather_lidar),nullptr,&work.shader),"CreateCS(lidar)");++allocations_;
    }
    if(work.depth_source!=work_[0].output.Get()) {
        work.depth.Reset();check(device_->CreateShaderResourceView(work_[0].output.Get(),nullptr,&work.depth),"CreateSRV(lidar depth)");++allocations_;
        work.depth_source=work_[0].output.Get();
    }
    struct Parameters {std::array<float,16> projection;std::array<float,4> viewport;std::array<UINT,4> dimensions;};
    Parameters values{projection.get<std::array<float,16>>(),{vp.TopLeftX,vp.TopLeftY,vp.Width,vp.Height},
        {work_[0].output_desc.Width,work_[0].output_desc.Height,count,0}};
    static_assert(sizeof(values)==96);
    context->UpdateSubresource(work.constants.Get(),0,nullptr,&values,0,0);
    {
        ComputeState restore(context);
        ID3D11ShaderResourceView* resources[]{work.depth.Get(),work.directions.Get(),nullptr};
        context->CSSetShaderResources(0,3,resources);context->CSSetUnorderedAccessViews(0,1,work.uav.GetAddressOf(),nullptr);
        context->CSSetConstantBuffers(0,1,work.constants.GetAddressOf());context->CSSetShader(work.shader.Get(),nullptr,0);
        context->Dispatch((count+127)/128,1,1);
    }
    context->CopyResource(work.staging.Get(),work.output.Get());
    lidar_pixels.resize(static_cast<size_t>(count)*16);
    lidar_description=pattern->description;
    lidar_description.update({{"file",camera+"_lidar.bin"},{"encoding","range_f32_status_u32_pixel_u32_depth_f32"},
        {"source_width",values.dimensions[0]},{"source_height",values.dimensions[1]},{"bytes_per_beam",16}});
}
void GpuPack::depth(ID3D11DeviceContext1* context,ID3D11Texture2D* source,
                    ID3D11Texture2D* attributes,ID3D11Texture2D* material,
                    const D3D11_VIEWPORT& vp,const std::string& camera,const json* projection,bool readback,std::shared_ptr<const LidarPattern> lidar) {
    std::array<float,28> values{0,0,0,0,vp.TopLeftX,vp.TopLeftY,vp.Width,vp.Height,vp.MinDepth,vp.MaxDepth};
    if(projection) {
        const auto p=projection->get<std::array<float,16>>();
        DirectX::XMFLOAT4X4 matrix;std::memcpy(&matrix,p.data(),sizeof(matrix));
        auto m=DirectX::XMLoadFloat4x4(&matrix);
        const auto correction=DirectX::XMMatrixSet(1,0,0,0,0,1,0,0,0,0,-.5f,.5f,0,0,0,1);
        DirectX::XMVECTOR determinant;
        const auto inverse=DirectX::XMMatrixInverse(&determinant,DirectX::XMMatrixMultiply(correction,m));
        if(!std::isfinite(DirectX::XMVectorGetX(determinant)) || DirectX::XMVectorGetX(determinant)==0)
            throw std::runtime_error("Singular depth projection");
        DirectX::XMStoreFloat4x4(&matrix,inverse);
        std::memcpy(values.data()+12,&matrix,sizeof(matrix));values[1]=1;
    }
    dispatch(context,{source,attributes,material},values,0,camera,readback);
    if(lidar) gather(context,lidar,*projection,vp,camera);
}
void GpuPack::color(ID3D11DeviceContext1* context,ID3D11Texture2D* source,float gain,const std::string& camera,bool preview) {
    dispatch(context,{source,nullptr,nullptr},{gain,0,preview?2.0f:1.0f,exposure_?1.f:0.f,0,0,0,0,0,0,0,0},preview?2:1,camera);
}
void GpuPack::dispatch(ID3D11DeviceContext1* context,std::array<ID3D11Texture2D*,3> sources,
                       const std::array<float,28>& values,unsigned kind,const std::string& camera,bool readback) {
    const bool depth=kind==0;
    Com<ID3D11Device> device;context->GetDevice(&device);
    if(device_.Get()!=device.Get()) {release_gpu();device_=device;}
    auto& work=work_[kind];
    // Full color and preview consume the same HDR image before it is reused.
    // Share that private input copy; each output keeps its own dimensions/UAV.
    auto& input=work_[depth?0:1];
    auto& copies=input.copies;auto& views=input.views;
    D3D11_TEXTURE2D_DESC dimensions{};sources[0]->GetDesc(&dimensions);
    for(size_t i=0;i<sources.size();++i) {
        if(!sources[i]) continue;
        D3D11_TEXTURE2D_DESC desc{};sources[i]->GetDesc(&desc);
        if(desc.Width!=dimensions.Width || desc.Height!=dimensions.Height ||
           desc.MipLevels!=1 || desc.ArraySize!=1 || desc.SampleDesc.Count!=1)
            throw std::runtime_error("RGB-D packing requires aligned single-sample 2D textures");
        DXGI_FORMAT view_format=desc.Format;
        if(depth && i==0) {
            if(desc.Format==DXGI_FORMAT_D32_FLOAT_S8X24_UINT || desc.Format==DXGI_FORMAT_R32G8X24_TYPELESS) {
                desc.Format=DXGI_FORMAT_R32G8X24_TYPELESS;view_format=DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
            } else if(desc.Format==DXGI_FORMAT_D24_UNORM_S8_UINT || desc.Format==DXGI_FORMAT_R24G8_TYPELESS) {
                desc.Format=DXGI_FORMAT_R24G8_TYPELESS;view_format=DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            } else if(desc.Format==DXGI_FORMAT_D32_FLOAT || desc.Format==DXGI_FORMAT_R32_TYPELESS) {
                desc.Format=DXGI_FORMAT_R32_TYPELESS;view_format=DXGI_FORMAT_R32_FLOAT;
            } else throw std::runtime_error("Unsupported RGB-D depth format");
        }
        desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags=desc.MiscFlags=0;
        const auto& previous=input.input_desc[i];
        if(!copies[i] || previous.Width!=desc.Width || previous.Height!=desc.Height || previous.Format!=desc.Format) {
            copies[i].Reset();views[i].Reset();
            check(device->CreateTexture2D(&desc,nullptr,&copies[i]),"CreateTexture2D(pack input)");++allocations_;
            D3D11_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=view_format;
            srv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;srv.Texture2D.MipLevels=1;
            check(device->CreateShaderResourceView(copies[i].Get(),&srv,&views[i]),"CreateShaderResourceView(pack input)");++allocations_;
            input.input_desc[i]=desc;
        }
        if(depth || !color_copied_) context->CopyResource(copies[i].Get(),sources[i]);
    }
    if(!depth && !color_copied_ && exposure_) {
        exposure_->update(context,views[0].Get(),values[0]);
        if(!exposure_staging_) {
            D3D11_BUFFER_DESC desc{};desc.ByteWidth=16;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            check(device->CreateBuffer(&desc,nullptr,&exposure_staging_),"Create exposure staging");
        }
        context->CopyResource(exposure_staging_.Get(),exposure_->value.Get());
    }
    if(!depth) color_copied_=true;
    auto desc=dimensions;if(kind==2) {desc.Width/=2;desc.Height/=2;}desc.Format=depth?DXGI_FORMAT_R32_FLOAT:DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags=0;
    desc.MiscFlags=shared_?(D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_NTHANDLE):0;
    auto& image=images[kind];
    if(!work.output || work.output_desc.Width!=desc.Width || work.output_desc.Height!=desc.Height || work.output_desc.MiscFlags!=desc.MiscFlags) {
        work.output.Reset();work.uav.Reset();image.staging.Reset();
        work.shared_handle.reset();
        check(device->CreateTexture2D(&desc,nullptr,&work.output),"CreateTexture2D(pack output)");++allocations_;
        check(device->CreateUnorderedAccessView(work.output.Get(),nullptr,&work.uav),"CreateUnorderedAccessView(pack output)");++allocations_;
        work.output_desc=desc;
        if(shared_) {
            Com<IDXGIResource1> resource;check(work.output.As(&resource),"QueryInterface(shared resource)");
            auto handle=std::make_unique<Handle>();
            check(resource->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&handle->h),"CreateSharedHandle(pack)");
            work.shared_handle=std::move(handle);work.resource_id=qpc_now();
        }
    }
    if(!work.shader) {
        check(device->CreateComputeShader(depth?ot_pack_depth:ot_pack_color,
            depth?sizeof(ot_pack_depth):sizeof(ot_pack_color),nullptr,&work.shader),"CreateComputeShader(pack)");++allocations_;
        D3D11_BUFFER_DESC buffer{};buffer.ByteWidth=sizeof(values);buffer.Usage=D3D11_USAGE_DEFAULT;
        buffer.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        check(device->CreateBuffer(&buffer,nullptr,&work.constants),"CreateBuffer(pack constants)");++allocations_;
    }
    context->UpdateSubresource(work.constants.Get(),0,nullptr,values.data(),0,0);
    {
        ComputeState restore(context);
        ID3D11ShaderResourceView* inputs[]={views[0].Get(),views[1].Get(),views[2].Get(),!depth && exposure_?exposure_->view.Get():nullptr};
        context->CSSetShaderResources(0,4,inputs);
        context->CSSetUnorderedAccessViews(0,1,work.uav.GetAddressOf(),nullptr);
        context->CSSetConstantBuffers(0,1,work.constants.GetAddressOf());
        context->CSSetShader(work.shader.Get(),nullptr,0);
        context->Dispatch((desc.Width+7)/8,(desc.Height+7)/8,1);
    }
    if(!readback) {image.description=nullptr;return;}
    if(!shared_ && !image.staging) {
        auto staging=desc;staging.Usage=D3D11_USAGE_STAGING;staging.BindFlags=0;staging.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        staging.MiscFlags=0;
        check(device->CreateTexture2D(&staging,nullptr,&image.staging),"CreateTexture2D(pack staging)");++allocations_;
    }
    if(!shared_) {
        context->CopyResource(image.staging.Get(),work.output.Get());
        image.pixels.resize(static_cast<size_t>(desc.Width)*desc.Height*4);
    }
    image.description={{"file",camera+(depth?"_depth_f32.bin":kind==2?"_preview_ldr.bin":"_color_ldr.bin")},
        {"width",desc.Width},{"height",desc.Height},{"row_bytes",desc.Width*4},
        {"format",depth?"R32_FLOAT":"R8G8B8A8_UNORM"},
        {"encoding",depth?(values[1]!=0?"optical_depth_m_nan_invalid":"viewport_depth_nan_invalid"):"srgb_reinhard"}};
    if(!depth) image.description["linear_gain"]=values[0];
    if(shared_) image.description["shared_texture"]={{"handle",reinterpret_cast<uintptr_t>(work.shared_handle->h)},
        {"id",work.resource_id},{"dxgi_format",static_cast<unsigned>(desc.Format)}};
}
json GpuPack::seal(ID3D11DeviceContext* context) {
    if(!shared_ || std::all_of(images.begin(),images.end(),[](const auto& i){return i.description.is_null();})) return nullptr;
    if(!fence_) {
        Com<ID3D11Device5> device;check(device_.As(&device),"QueryInterface(Device5)");
        check(device->CreateFence(0,D3D11_FENCE_FLAG_SHARED,IID_PPV_ARGS(&fence_)),"CreateFence(pack)");
        auto handle=std::make_unique<Handle>();
        check(fence_->CreateSharedHandle(nullptr,GENERIC_ALL,nullptr,&handle->h),"CreateSharedHandle(fence)");
        fence_handle_=std::move(handle);fence_id_=qpc_now();
    }
    Com<ID3D11DeviceContext4> context4;check(context->QueryInterface(IID_PPV_ARGS(&context4)),"QueryInterface(Context4)");
    fence_value_+=2;
    check(context4->Signal(fence_.Get(),fence_value_-1),"Signal(pack ready)");
    awaiting_reader_=true;
    Com<IDXGIDevice> dxgi;Com<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC description{};
    check(device_.As(&dxgi),"QueryInterface(DXGI device)");check(dxgi->GetAdapter(&adapter),"GetAdapter");
    check(adapter->GetDesc(&description),"GetAdapterDesc");
    return {{"pid",GetCurrentProcessId()},{"id",fence_id_},{"handle",reinterpret_cast<uintptr_t>(fence_handle_->h)},
        {"ready",fence_value_-1},{"released",fence_value_},{"adapter_low",description.AdapterLuid.LowPart},{"adapter_high",description.AdapterLuid.HighPart}};
}
bool GpuPack::collect(ID3D11DeviceContext* context) {
    if(color_copied_ && exposure_) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const auto hr=context->Map(exposure_staging_.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);
        if(hr==DXGI_ERROR_WAS_STILL_DRAWING) return false;check(hr,"Map exposure");
        std::array<float,4> values{};std::memcpy(values.data(),mapped.pData,sizeof(values));context->Unmap(exposure_staging_.Get(),0);
        exposure_sample={{"automatic",true},{"linear_gain",values[0]},{"log_average_luminance",values[1]},{"target_gain",values[2]}};
        for(unsigned kind:{1u,2u}) if(!images[kind].description.is_null()) images[kind].description["linear_gain"]=values[0];
    }
    for(auto& image:images) {
        if(image.description.is_null() || shared_) continue;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const auto hr=context->Map(image.staging.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);
        if(hr==DXGI_ERROR_WAS_STILL_DRAWING) return false;
        check(hr,"Map(pack staging)");
        const auto row=image.description.at("row_bytes").get<size_t>();
        const auto height=image.description.at("height").get<UINT>();
        for(UINT y=0;y<height;++y)
            std::memcpy(image.pixels.data()+y*row,static_cast<const uint8_t*>(mapped.pData)+y*mapped.RowPitch,row);
        context->Unmap(image.staging.Get(),0);
    }
    if(!lidar_description.is_null()) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const auto hr=context->Map(lidar_.staging.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);
        if(hr==DXGI_ERROR_WAS_STILL_DRAWING) return false;
        check(hr,"Map(lidar staging)");std::memcpy(lidar_pixels.data(),mapped.pData,lidar_pixels.size());
        context->Unmap(lidar_.staging.Get(),0);
    }
    return true;
}
}
