#pragma once
#include "ot.hpp"
#include <array>
#include <d3d11_1.h>
#include <wrl/client.h>

namespace ot {
class GpuPack {
    template<class T> using Com=Microsoft::WRL::ComPtr<T>;
public:
    struct Image {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
        json description;
        std::vector<uint8_t> pixels;
    };
    void depth(ID3D11DeviceContext1* context,ID3D11Texture2D* source,
               ID3D11Texture2D* attributes,ID3D11Texture2D* material,
               const D3D11_VIEWPORT& viewport,const std::string& camera,const json* projection=nullptr);
    void color(ID3D11DeviceContext1* context,ID3D11Texture2D* source,float gain,const std::string& camera);
    bool collect(ID3D11DeviceContext* context);
    void release_gpu();
    void clear() {for(auto& image:images) {image.description=nullptr;image.pixels.clear();}}
    uint64_t allocations() const {return allocations_;}
    std::array<Image,2> images; // depth, color
private:
    struct Work {
        std::array<Com<ID3D11Texture2D>,3> copies;
        std::array<Com<ID3D11ShaderResourceView>,3> views;
        std::array<D3D11_TEXTURE2D_DESC,3> input_desc{};
        Com<ID3D11Texture2D> output;
        Com<ID3D11UnorderedAccessView> uav;
        Com<ID3D11ComputeShader> shader;
        Com<ID3D11Buffer> constants;
        D3D11_TEXTURE2D_DESC output_desc{};
    };
    std::array<Work,2> work_;
    Com<ID3D11Device> device_;
    uint64_t allocations_=0;
    void dispatch(ID3D11DeviceContext1* context,std::array<ID3D11Texture2D*,3> sources,
                  const std::array<float,28>& constants,bool depth,const std::string& camera);
};
}
