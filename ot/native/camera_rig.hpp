#pragma once
#include "ot.hpp"
#include <array>
#include <safetyhook.hpp>
#include <unordered_set>

namespace ot {
// Redirect the submission's source pointer to a private copy. The persistent
// mirror object is untouched; the engine copies pose/frustum into its frame work.
class CameraRig {
public:
    json configure(const json& request);
    json status();
    void clear() noexcept { configuration_.store(nullptr); }
    void select(safetyhook::Context& context,uint32_t capture_mask=UINT32_MAX) noexcept;
    void begin(safetyhook::Context& context) noexcept;
    void end(safetyhook::Context& context) noexcept;
    void dimensions(safetyhook::Context& context) noexcept;
    void ego_parts(safetyhook::Context& context) noexcept;
    void submission_drawables(safetyhook::Context& context) noexcept;
    void graph_drawables(safetyhook::Context& context) noexcept;
    void graph_cameras(safetyhook::Context& context) noexcept;
    void graph_end() noexcept;
    uint32_t in_flight() const noexcept { return in_flight_.load()+selections_.load()+graphs_.load(); }
    bool can_select() const noexcept { auto c=configuration_.load();return !c || !c->private_outputs || !in_flight(); }
    uint32_t mask() const noexcept { auto c=configuration_.load();return c?c->mask:0; }
private:
    enum class Basis { world,chassis,cabin };
    struct View {
        bool enabled=false;
        unsigned source_slot=0;
        Basis basis=Basis::world;
        std::array<double,3> position{};
        std::array<double,4> rotation{};
        float hfov{},vfov{};
        std::array<uint32_t,2> resolution{};
    };
    struct Configuration { std::array<View,9> views;uint32_t mask=0;bool ego_full_model=false,private_outputs=false; };
    struct Array {uintptr_t vtable=0,data=0;uint64_t size=0,capacity=0;};
    struct PrivateView {
        alignas(16) std::array<uint8_t,0x540> camera{};
        alignas(16) std::array<uint8_t,0x168> drawable{};
        std::array<char,64> name{};
    };
    bool prepare_private(uintptr_t cameras,const Configuration& config) noexcept;
    std::array<PrivateView,9> private_views_;
    std::array<uintptr_t,9> camera_pointers_{},drawable_pointers_{};
    Array camera_array_,drawable_array_;
    std::atomic<uintptr_t> interior_{0};
    std::atomic<bool> private_ready_{false};
    std::mutex requests_mutex_;
    std::unordered_set<uintptr_t> requests_;
    std::atomic<uint32_t> selections_{0},graphs_{0};
    std::atomic<std::shared_ptr<const Configuration>> configuration_;
    std::atomic<uint32_t> in_flight_{0};
    std::array<std::atomic<uint64_t>,9> applied_{};
    std::atomic<uint64_t> unavailable_{0};
    std::atomic<uint64_t> ego_parts_applied_{0};
};
}
