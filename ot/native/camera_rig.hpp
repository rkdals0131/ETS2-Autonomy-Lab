#pragma once
#include "ot.hpp"
#include <array>
#include <safetyhook.hpp>

namespace ot {
// Redirect the submission's source pointer to a private copy. The persistent
// mirror object is untouched; the engine copies pose/frustum into its frame work.
class CameraRig {
public:
    json configure(const json& request);
    json status();
    void clear() noexcept { configuration_.store(nullptr); }
    void select(safetyhook::Context& context) noexcept;
    void begin(safetyhook::Context& context) noexcept;
    void end() noexcept;
    void dimensions(safetyhook::Context& context) noexcept;
    uint32_t in_flight() const noexcept { return in_flight_.load(); }
    uint32_t mask() const noexcept { auto c=configuration_.load();return c?c->mask:0; }
private:
    struct View {
        bool enabled=false,chassis=false;
        std::array<double,3> position{};
        std::array<double,4> rotation{};
        float hfov{},vfov{};
        std::array<uint32_t,2> resolution{};
    };
    struct Configuration { std::array<View,6> views;uint32_t mask=0; };
    std::atomic<std::shared_ptr<const Configuration>> configuration_;
    std::atomic<uint32_t> in_flight_{0};
    std::array<std::atomic<uint64_t>,6> applied_{};
    std::atomic<uint64_t> unavailable_{0};
};
}
