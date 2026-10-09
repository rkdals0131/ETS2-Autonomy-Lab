#pragma once
#include "geometry.hpp"
#include "../../ot/include/mount_layout.hpp"
#include <vector>
#include <stdexcept>

namespace bridge {
using SensorMountMismatch=ot::MountMismatch;
struct WheelGeometry {
    uint32_t index;
    V position;
    double radius;
    bool simulated;
};
struct VehicleProfile {
    json rig;
    json configuration;
    V base;
    std::vector<WheelGeometry> wheels;
    uint64_t generation;
};
VehicleProfile resolve_vehicle(json rig,const json& truck,const json& selected);
json lidar_patterns(const json& rig,const json& profile);
}
