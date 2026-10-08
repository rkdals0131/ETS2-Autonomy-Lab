#pragma once
#include "ot.hpp"
#include "gpu_pack.hpp"
#include <array>
#include <d3d11_1.h>
#include <wrl/client.h>

namespace ot {
struct CaptureOptions {
    std::string format="raw";
    float color_gain=1;
    bool selective=false;
    std::array<uint8_t,6> outputs{3,3,3,3,3,3}; // color=1, depth=2, preview=4, lidar=8, metadata=16
    uint8_t products=3;
    bool color() const {return products&1;}
    bool depth() const {return products&2;}
    bool preview() const {return products&4;}
    bool lidar() const {return products&8;}
    bool raw() const {return format=="raw" || format=="raw+rgbd8" || format=="raw+ros";}
    bool metric() const {return format=="ros" || format=="raw+ros";}
    bool packed() const {return format!="raw";}
};
// One requested camera sample. All context calls run on the game's render
// thread; file I/O runs only in the existing command worker.
class GpuCapture {
public:
    enum class Phase { idle,armed,waiting_gpu,ready,error };
    Phase phase() const noexcept {return phase_.load();}
    explicit GpuCapture(std::string camera):camera_(std::move(camera)) {}
    json command(const std::string& action,uint64_t requested_frame=0,bool metadata=true,const CaptureOptions& options={});
    void cancel() noexcept;
    void append_bundle(json& views,std::vector<BundleBlob>& blobs);
    void observe(ID3D11DeviceContext* context, uint32_t count, const uintptr_t* targets,
                 uint64_t binding_sequence, uint64_t sdk_frame, uint64_t render_frame,
                 uint64_t observation_session,const json* pass) noexcept;
private:
    template<class T> using Com=Microsoft::WRL::ComPtr<T>;
    struct Image {
        Com<ID3D11Texture2D> source,staging;
        D3D11_TEXTURE2D_DESC desc{};
        std::vector<uint8_t> pixels;
    };
    struct Constants {
        Com<ID3D11Buffer> staging;
        std::vector<uint8_t> bytes;
        json description;
    };
    void submit(ID3D11DeviceContext* context, uint64_t sequence, uint64_t sdk_frame,
                uint64_t render_frame, uint64_t observation_session);
    void collect(ID3D11DeviceContext* context);
    void geometry_constants(ID3D11DeviceContext* context,uint64_t binding_sequence);
    void vehicle_constants(ID3D11DeviceContext* context,ID3D11Device* device);
    void clear_vehicle_constants();
    void release_gpu();
    void release_sources();
    void prepare_staging(Image& image,const D3D11_TEXTURE2D_DESC& desc,ID3D11Device* device);
    void prepare_constants(Constants& sample,UINT bytes,ID3D11Device* device);
    json status(bool metadata=true) const;
    json save();
    std::mutex mutex_;
    const std::string camera_;
    json metadata_=json::object(),geometry_pass_,color_pass_,geometry_gpu_;
    std::atomic<Phase> phase_{Phase::idle};
    std::array<Image,3> images_; // attributes0, attributes3, color
    CaptureOptions options_;
    GpuPack packed_;
    Image geometry_depth_;
    uint32_t depth_pixel_bytes_=0;
    std::array<Constants,2> geometry_constants_; // VS/PS slot 0 at G-buffer exit
    std::vector<Constants> vehicle_constants_; // VS slot 0 of matched vehicle draw items
    size_t vehicle_constants_used_=0;
    Com<ID3D11Query> completion_;
    Com<ID3D11DeviceContext> context_;
    Com<ID3D11Device> device_;
    uint64_t allocations_=0;
    uint64_t sequence_=0,request_started_=0,geometry_binding_=0,geometry_sdk_=0;
    uint64_t geometry_frame_=0,requested_frame_=0;
    uint64_t gpu_polls_=0,bindings_seen_=0;
    std::string error_,last_label_;
    fs::path saved_;
};
}
