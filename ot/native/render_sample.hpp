#pragma once
#include "ot.hpp"
#include <array>
#include <memory>
#include <optional>

namespace ot {
// Immutable values observed at pass compilation. Engine pointers are recorded
// for diagnostics only; serialization never dereferences them on a worker.
struct RenderCameraSample {
    bool available=false;
    uint64_t qpc{};
    uintptr_t work{},camera{},deferred{};
    uint16_t batch{};
    uint32_t component_mask{},viewport_mode{};
    uint8_t projection_modifier_flag{};
    std::array<float,2> viewport_depth{};
    std::array<float,4> viewport_rect{},projection_modifier{},ray{},dimensions{};
    std::array<float,16> projection{},rotation{};
    std::array<float,3> local{};
    std::array<int16_t,2> cells{};
    std::array<double,3> world{};
    std::string error;
    json describe() const;
};
enum class RenderTarget { other,attributes0,attributes3,color };
struct RenderDrawSample {
    uintptr_t geometry{},buffer{};
    uint32_t first{},count{};
    bool known=false;
    uint64_t qpc{};
    size_t item_index{};
    json binding() const;
};
struct RenderActorPlacement {
    std::array<float,3> local{};
    std::array<int16_t,2> cells{};
    std::array<float,4> quaternion{};
    json describe() const;
};
struct RenderVehicleSample {
    uintptr_t actor{},model{},object{},component{};
    bool parked=false;
    size_t lod{};
    uint64_t qpc{};
    std::vector<uintptr_t> geometry;
    std::vector<RenderDrawSample> draws;
    std::array<float,16> rotation{};
    std::array<float,3> local{},reference{};
    std::array<int16_t,2> cells{};
    RenderActorPlacement actor_placement;
    std::array<float,6> aabb{};
    json describe() const;
};
struct RenderVehiclesSample {
    bool available=false,truncated=false,scene_observed=false,geometry_observed=false,actors_observed=false;
    uint64_t qpc_begin{},qpc_end{};
    uintptr_t scene{};
    uint64_t source_count{},group_count{},override_count{},unique_count{},actors_considered{};
    size_t draw_count{};
    bool draws_truncated=false;
    std::string error,draw_error;
    std::vector<std::pair<uintptr_t,std::string>> errors;
    std::vector<RenderVehicleSample> vehicles;
    json describe() const;
};
struct RenderImageLink {
    uintptr_t reference_offset{};
    uint32_t id{};
    uint16_t pool{};
    std::string space,name;
};
struct RenderPassSample {
    uintptr_t pass{},input{};
    unsigned graph{},camera_slot=9;
    bool geometry=false;
    std::array<RenderTarget,8> targets{};
    std::string name,space;
    std::vector<RenderImageLink> links;
    RenderCameraSample camera;
    std::optional<RenderVehiclesSample> vehicles;
    std::shared_ptr<const json> sdk;
    json describe(bool diagnostic=true) const;
};
using RenderPassPtr=std::shared_ptr<const RenderPassSample>;
}
