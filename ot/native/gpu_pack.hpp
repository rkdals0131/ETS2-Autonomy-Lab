#pragma once
#include "ot.hpp"
#include <array>
#include <d3d11_1.h>
#include <wrl/client.h>

namespace ot {
class GpuPack {
public:
    struct Image {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
        json description;
        std::vector<uint8_t> pixels;
    };
    void depth(ID3D11DeviceContext1* context,ID3D11Texture2D* source,
               ID3D11Texture2D* attributes,ID3D11Texture2D* material,
               const D3D11_VIEWPORT& viewport,const std::string& camera);
    void color(ID3D11DeviceContext1* context,ID3D11Texture2D* source,float gain,const std::string& camera);
    bool collect(ID3D11DeviceContext* context);
    void release_gpu() {for(auto& image:images) image.staging.Reset();}
    void clear() {for(auto& image:images) image=Image{};}
    std::array<Image,2> images; // depth, color
private:
    void dispatch(ID3D11DeviceContext1* context,std::array<ID3D11Texture2D*,3> sources,
                  const std::array<float,12>& constants,bool depth,const std::string& camera);
};
}
