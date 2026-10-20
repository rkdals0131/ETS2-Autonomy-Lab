#pragma once
#include "ot.hpp"
#include "overlay_mask.hpp"
#include "render_sample.hpp"
#include <array>
#include <d3d11_1.h>
#include <wrl/client.h>

namespace ot {
struct OverlayOptions {
    double z_tolerance=1,slope=0.140540834749,normal_min=.7; // slope: tan 8 degrees
    double fade_start=80,fade_end=120;                          // arc length (m), baked into the mask
    bool backbuffer_usage=false; // detector (a) DXGI_USAGE_BACK_BUFFER instead of (b) the compiled span
    json describe() const {
        return {{"z_tolerance_m",z_tolerance},{"slope_tolerance",slope},{"normal_min",normal_min},
            {"fade_m",{fade_start,fade_end}},{"detector",backbuffer_usage?"backbuffer_usage":"compiled_span"}};
    }
};
// HUD trajectory decal (docs/24 §5). A worker rasterizes the shared path into a
// BEV mask when its generation changes; the render thread only uploads the mask,
// fills one constant buffer and replays a fixed command list with the game's
// state restored by ExecuteCommandList(list, TRUE).
class OverlayDecal {
public:
    ~OverlayDecal() {stop();release_gpu();}
    // Pipe thread. The block outlives the decal (Transport owns the mapping).
    void start(OverlayBlock* block,const OverlayOptions& options);
    void stop() noexcept;
    // Only after the render hooks drained: the render thread owns these objects.
    void release_gpu() noexcept;
    std::shared_ptr<const OverlayOptions> options() const {return options_.load();}
    // Render thread, in the OM hook before blit_to_target binds the backbuffer;
    // target is main0/composition_out, depth the main scene DSV of that graph.
    void draw(ID3D11DeviceContext* context,ID3D11RenderTargetView* target,ID3D11DepthStencilView* depth,
              const RenderCameraSample& camera,uint64_t sdk_time_us) noexcept;
    json status();
    // Pipe thread: copy the next two drawn frames of the target and save them.
    json capture(const json& request);
private:
    template<class T> using Com=Microsoft::WRL::ComPtr<T>;
    struct Frame {
        uint64_t serial{};
        LONG64 generation{};
        OverlayContent content;
        overlay::Mask mask;
        uint64_t raster_qpc{};
    };
    enum Skip {no_path,invalid,expired,camera,target,depth,offscreen,unsupported,failed,skip_count};
    void run() noexcept;
    void prepare(ID3D11Device* device);
    void prepare_depth(ID3D11DeviceContext* context,ID3D11DepthStencilView* view,D3D11_TEXTURE2D_DESC& desc);
    void record(ID3D11RenderTargetView* target,UINT width,UINT height);
    void capture_frame(ID3D11DeviceContext* context,ID3D11Texture2D* target,const D3D11_TEXTURE2D_DESC& desc,
                       const Frame& frame,const overlay::Camera& camera,uint64_t sdk_time_us);
    OverlayBlock* block_=nullptr;
    std::atomic<std::shared_ptr<const OverlayOptions>> options_{std::make_shared<const OverlayOptions>()};
    std::atomic<std::shared_ptr<const Frame>> frame_;
    Handle stop_event_;
    std::thread worker_;
    std::atomic<uint64_t> rasters_{0},torn_{0},rejected_{0},raster_ticks_{0},serial_{0};
    // Render thread state.
    Com<ID3D11Device> device_;
    Com<ID3D11DeviceContext> deferred_;
    Com<ID3D11CommandList> list_;
    Com<ID3D11VertexShader> vs_;
    Com<ID3D11PixelShader> ps_;
    Com<ID3D11Buffer> constants_;
    Com<ID3D11Texture2D> mask_;
    Com<ID3D11ShaderResourceView> mask_view_,depth_view_;
    Com<ID3D11SamplerState> sampler_;
    Com<ID3D11BlendState> blend_;
    Com<ID3D11RasterizerState> raster_;
    Com<ID3D11DepthStencilState> depth_state_;
    Com<ID3D11Resource> depth_source_;
    Com<ID3D11Texture2D> depth_copy_;
    Com<ID3D11RenderTargetView> recorded_target_;
    ID3D11ShaderResourceView* recorded_depth_=nullptr;
    UINT recorded_width_=0,recorded_height_=0;
    uint64_t uploaded_serial_=0;
    bool failed_=false;
    std::array<std::atomic<uint64_t>,skip_count> skips_{};
    std::atomic<uint64_t> drawn_{0},records_{0},depth_copies_{0},gpu_allocations_{0};
    // Diagnostics the pipe thread reads.
    SRWLOCK state_lock_=SRWLOCK_INIT;
    std::string error_;
    json last_draw_=nullptr;
    // overlay capture: two consecutive drawn frames.
    struct Captured {Com<ID3D11Texture2D> staging;D3D11_TEXTURE2D_DESC desc{};json metadata;std::vector<uint8_t> pixels;bool copied=false,mapped=false;};
    SRWLOCK capture_lock_=SRWLOCK_INIT;
    std::array<Captured,2> captured_;
    unsigned capture_requested_=0;
    std::atomic<bool> capture_busy_{false};
};
}
