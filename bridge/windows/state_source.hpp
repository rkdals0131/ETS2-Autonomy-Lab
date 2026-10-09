#pragma once
#include "motion_sensors.hpp"
#include <memory>
#include <mutex>
#include <thread>

namespace bridge {
struct StateSample {
    json telemetry;
    MotionSample motion;
};

// Reads SDK/physics and advances sensor models independently of ROS sockets.
class StateSource {
public:
    StateSource(const VehicleProfile& vehicle,const json& settings);
    ~StateSource();
    std::shared_ptr<const StateSample> latest() const;
    const json& configuration() const {return configuration_;}
private:
    MotionSensors sensors_;
    json configuration_;
    mutable std::mutex mutex_;
    std::shared_ptr<const StateSample> latest_;
    std::exception_ptr error_;
    std::jthread worker_;
};
}
