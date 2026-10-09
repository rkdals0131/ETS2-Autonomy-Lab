#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace ot {
struct PhysicalInput {
    bool connected{};
    float x{},right_trigger{},left_trigger{};
    // A/Left, D/Right, W/Up, S/Down, in that order.
    std::array<bool,8> keys{};
};
class ManualInput {
public:
    struct Sample {std::array<float,3> values{};bool active{};uint32_t active_axes{};};
    explicit ManualInput(const std::filesystem::path& path);
    ~ManualInput();
    Sample evaluate(const PhysicalInput& input) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
