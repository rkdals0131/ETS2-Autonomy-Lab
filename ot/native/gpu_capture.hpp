#pragma once
#include "ot.hpp"
#include <array>
#include <d3d11.h>
#include <wrl/client.h>
#include <unordered_map>

namespace ot {
// One requested mirror5 sample. All context calls run on the game's render
// thread; file I/O runs only in the existing command worker.
class GpuCapture {
public:
    explicit GpuCapture(const json& schema):layout_(schema.at("render_graph")) {}
    json command(const std::string& action);
    void cancel() noexcept;
    void label_image(uintptr_t image, uintptr_t image_id_address) noexcept;
    void observe(ID3D11DeviceContext* context, uint32_t count, const uintptr_t* targets,
                 uint64_t binding_sequence, uint64_t sdk_frame, uint64_t render_frame,
                 uint64_t observation_session) noexcept;
private:
    template<class T> using Com=Microsoft::WRL::ComPtr<T>;
    struct Image {
        Com<ID3D11Texture2D> source,staging;
        D3D11_TEXTURE2D_DESC desc{};
        std::vector<uint8_t> pixels;
    };
    enum class Phase { idle,armed,waiting_gpu,ready,error };
    std::string graph_name(ID3D11Resource* resource);
    void submit(ID3D11DeviceContext* context, uint64_t sequence, uint64_t sdk_frame,
                uint64_t render_frame, uint64_t observation_session);
    void collect(ID3D11DeviceContext* context);
    void release_gpu();
    json status() const;
    json save();
    std::mutex mutex_;
    json layout_,metadata_=json::object();
    Phase phase_=Phase::idle;
    std::array<Image,3> images_; // attributes0, attributes3, color
    Com<ID3D11Query> completion_;
    Com<ID3D11DeviceContext> context_;
    uint64_t sequence_=0,request_started_=0,geometry_binding_=0,geometry_sdk_=0;
    uint64_t geometry_frame_=0;
    uint64_t gpu_polls_=0,bindings_seen_=0;
    mutable std::mutex names_mutex_;
    std::atomic<bool> label_tracking_{false};
    uint64_t label_callbacks_=0;
    std::unordered_map<uint16_t,std::string> image_labels_;
    std::string error_,last_label_;
    fs::path saved_;
};
}
