#include "gpu_pack.hpp"
#include "pack_depth.hpp"
#include "pack_color.hpp"
#include <stdexcept>

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
    std::array<ID3D11ShaderResourceView*,3> resources{};
    Com<ID3D11UnorderedAccessView> output;
    Com<ID3D11Buffer> constants;
    explicit ComputeState(ID3D11DeviceContext1* c):context(c) {
        context->CSGetShader(&shader,instances.data(),&instance_count);
        context->CSGetShaderResources(0,3,resources.data());
        context->CSGetUnorderedAccessViews(0,1,&output);
        context->CSGetConstantBuffers1(0,1,&constants,&first,&count);
    }
    ~ComputeState() {
        ID3D11UnorderedAccessView* null_output{};
        context->CSSetUnorderedAccessViews(0,1,&null_output,nullptr);
        context->CSSetShaderResources(0,3,resources.data());
        const UINT keep=UINT(-1);
        context->CSSetUnorderedAccessViews(0,1,output.GetAddressOf(),&keep);
        context->CSSetConstantBuffers1(0,1,constants.GetAddressOf(),&first,&count);
        context->CSSetShader(shader.Get(),instances.data(),instance_count);
        for(auto* p:resources) if(p) p->Release();
        for(UINT i=0;i<instance_count;++i) if(instances[i]) instances[i]->Release();
    }
};
}
void GpuPack::depth(ID3D11DeviceContext1* context,ID3D11Texture2D* source,
                    ID3D11Texture2D* attributes,ID3D11Texture2D* material,
                    const D3D11_VIEWPORT& vp,const std::string& camera) {
    dispatch(context,{source,attributes,material},{0,0,0,0,vp.TopLeftX,vp.TopLeftY,vp.Width,vp.Height,
        vp.MinDepth,vp.MaxDepth,0,0},true,camera);
}
void GpuPack::color(ID3D11DeviceContext1* context,ID3D11Texture2D* source,float gain,const std::string& camera) {
    dispatch(context,{source,nullptr,nullptr},{gain,0,0,0,0,0,0,0,0,0,0,0},false,camera);
}
void GpuPack::dispatch(ID3D11DeviceContext1* context,std::array<ID3D11Texture2D*,3> sources,
                       const std::array<float,12>& values,bool depth,const std::string& camera) {
    Com<ID3D11Device> device;context->GetDevice(&device);
    std::array<Com<ID3D11Texture2D>,3> copies;
    std::array<Com<ID3D11ShaderResourceView>,3> views;
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
        check(device->CreateTexture2D(&desc,nullptr,&copies[i]),"CreateTexture2D(pack input)");
        D3D11_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=view_format;
        srv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;srv.Texture2D.MipLevels=1;
        check(device->CreateShaderResourceView(copies[i].Get(),&srv,&views[i]),"CreateShaderResourceView(pack input)");
        context->CopyResource(copies[i].Get(),sources[i]);
    }
    auto desc=dimensions;desc.Format=depth?DXGI_FORMAT_R32_FLOAT:DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
    desc.CPUAccessFlags=desc.MiscFlags=0;
    Com<ID3D11Texture2D> output;
    check(device->CreateTexture2D(&desc,nullptr,&output),"CreateTexture2D(pack output)");
    Com<ID3D11UnorderedAccessView> uav;
    check(device->CreateUnorderedAccessView(output.Get(),nullptr,&uav),"CreateUnorderedAccessView(pack output)");
    Com<ID3D11ComputeShader> shader;
    check(device->CreateComputeShader(depth?ot_pack_depth:ot_pack_color,
        depth?sizeof(ot_pack_depth):sizeof(ot_pack_color),nullptr,&shader),"CreateComputeShader(pack)");
    D3D11_BUFFER_DESC buffer{};buffer.ByteWidth=sizeof(values);buffer.Usage=D3D11_USAGE_IMMUTABLE;
    buffer.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA data{};data.pSysMem=values.data();
    Com<ID3D11Buffer> constants;
    check(device->CreateBuffer(&buffer,&data,&constants),"CreateBuffer(pack constants)");
    auto& image=images[depth?0:1];
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    check(device->CreateTexture2D(&desc,nullptr,&image.staging),"CreateTexture2D(pack staging)");
    {
        ComputeState restore(context);
        ID3D11ShaderResourceView* inputs[]={views[0].Get(),views[1].Get(),views[2].Get()};
        context->CSSetShaderResources(0,3,inputs);
        context->CSSetUnorderedAccessViews(0,1,uav.GetAddressOf(),nullptr);
        context->CSSetConstantBuffers(0,1,constants.GetAddressOf());
        context->CSSetShader(shader.Get(),nullptr,0);
        context->Dispatch((desc.Width+7)/8,(desc.Height+7)/8,1);
    }
    context->CopyResource(image.staging.Get(),output.Get());
    image.pixels.resize(static_cast<size_t>(desc.Width)*desc.Height*4);
    image.description={{"file",camera+(depth?"_depth_f32.bin":"_color_ldr.bin")},
        {"width",desc.Width},{"height",desc.Height},{"row_bytes",desc.Width*4},
        {"format",depth?"R32_FLOAT":"R8G8B8A8_UNORM"},
        {"encoding",depth?"viewport_depth_nan_invalid":"srgb_reinhard"}};
    if(!depth) image.description["linear_gain"]=values[0];
}
bool GpuPack::collect(ID3D11DeviceContext* context) {
    for(auto& image:images) {
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
    return true;
}
}
