#pragma once
#include "ot.hpp"
#include <array>
#include <d3d11_1.h>
#include <wrl/client.h>

namespace ot {
struct LidarPattern {
    json description;
    std::vector<std::array<float,4>> directions;
};
std::shared_ptr<const LidarPattern> make_lidar_pattern(const json& config);
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
               const D3D11_VIEWPORT& viewport,const std::string& camera,const json* projection=nullptr,bool readback=true,
               std::shared_ptr<const LidarPattern> lidar={});
    void color(ID3D11DeviceContext1* context,ID3D11Texture2D* source,float gain,const std::string& camera,bool preview=false);
    bool collect(ID3D11DeviceContext* context);
    void release_gpu();
    void clear() {for(auto& image:images) {image.description=nullptr;image.pixels.clear();} lidar_description=nullptr;lidar_pixels.clear();}
    uint64_t allocations() const {return allocations_;}
    std::array<Image,3> images; // depth, color, preview
    json lidar_description;
    std::vector<uint8_t> lidar_pixels;
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
    std::array<Work,3> work_;
    Com<ID3D11Device> device_;
    uint64_t allocations_=0;
    struct LidarWork {
        std::shared_ptr<const LidarPattern> pattern;
        Com<ID3D11Buffer> beams,output,staging,constants;
        Com<ID3D11ShaderResourceView> directions,depth;
        Com<ID3D11UnorderedAccessView> uav;
        Com<ID3D11ComputeShader> shader;
        ID3D11Texture2D* depth_source=nullptr;
    } lidar_;
    void gather(ID3D11DeviceContext1* context,const std::shared_ptr<const LidarPattern>& pattern,
                const json& projection,const D3D11_VIEWPORT& viewport,const std::string& camera);
    void dispatch(ID3D11DeviceContext1* context,std::array<ID3D11Texture2D*,3> sources,
                  const std::array<float,28>& constants,unsigned kind,const std::string& camera,bool readback=true);
};
}
