#pragma once
#include "ot.hpp"
#include <array>
#include <d3d11_4.h>
#include <wrl/client.h>

namespace ot {
struct LidarPattern {
    json description;
    std::vector<std::array<float,4>> directions;
    bool interpolate=false;
    float depth_edge_ratio=1.04f;
};
std::shared_ptr<const LidarPattern> make_lidar_pattern(const json& config);
struct ExposureState {
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11Buffer> value,constants;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> output;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader;
    uint64_t previous=0;
    void update(ID3D11DeviceContext1* context,ID3D11ShaderResourceView* source,float initial_gain);
};
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
               const D3D11_VIEWPORT& viewport,const std::string& camera,const std::array<float,16>* projection=nullptr,bool readback=true,
               std::shared_ptr<const LidarPattern> lidar={});
    void color(ID3D11DeviceContext1* context,ID3D11Texture2D* source,float gain,const std::string& camera,bool preview=false);
    bool collect(ID3D11DeviceContext* context);
    void share(bool enabled) {shared_=enabled;}
    void exposure(std::shared_ptr<ExposureState> value) {exposure_=std::move(value);}
    json seal(ID3D11DeviceContext* context);
    bool reusable() const {return !awaiting_reader_ || (fence_ && fence_->GetCompletedValue()>=fence_value_);}
    void abandon() {awaiting_reader_=false;}
    void release_gpu();
    // Descriptions select this sample's outputs. Retain CPU storage so arming
    // the next sample does not zero every pixel on the render thread.
    void clear() {for(auto& image:images) image.description=nullptr;lidar_description=nullptr;color_copied_=false;exposure_sample=nullptr;}
    uint64_t allocations() const {return allocations_;}
    std::array<Image,3> images; // depth, color, preview
    json lidar_description;
    std::vector<uint8_t> lidar_pixels;
    json exposure_sample;
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
        std::unique_ptr<Handle> shared_handle;
        uint64_t resource_id=0;
    };
    std::array<Work,3> work_;
    Com<ID3D11Device> device_;
    uint64_t allocations_=0;
    bool shared_=false,awaiting_reader_=false;
    bool color_copied_=false;
    std::shared_ptr<ExposureState> exposure_;
    Com<ID3D11Buffer> exposure_staging_;
    Com<ID3D11Fence> fence_;
    std::unique_ptr<Handle> fence_handle_;
    uint64_t fence_value_=0,fence_id_=0;
    struct LidarWork {
        std::shared_ptr<const LidarPattern> pattern;
        Com<ID3D11Buffer> beams,output,staging,constants;
        Com<ID3D11ShaderResourceView> directions,depth;
        Com<ID3D11UnorderedAccessView> uav;
        Com<ID3D11ComputeShader> shader;
        ID3D11Texture2D* depth_source=nullptr;
    } lidar_;
    void gather(ID3D11DeviceContext1* context,const std::shared_ptr<const LidarPattern>& pattern,
                const std::array<float,16>& projection,const D3D11_VIEWPORT& viewport,const std::string& camera);
    void dispatch(ID3D11DeviceContext1* context,std::array<ID3D11Texture2D*,3> sources,
                  const std::array<float,28>& constants,unsigned kind,const std::string& camera,bool readback=true);
};
}
