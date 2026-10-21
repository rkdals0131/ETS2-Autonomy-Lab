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
    StateSource(const VehicleProfile& vehicle,const json& settings,const std::string& core_version);
    ~StateSource();
    std::shared_ptr<const StateSample> latest() const;
    const json& configuration() const {return configuration_;}
    // The worker ended on GameSessionChanged.
    bool game_session_changed() const {
        std::lock_guard lock(mutex_);
        if(!error_) return false;
        try {std::rethrow_exception(error_);} catch(const GameSessionChanged&) {return true;} catch(...) {return false;}
    }
private:
    MotionSensors sensors_;
    json truck_attributes_;
    json configuration_;
    mutable std::mutex mutex_;
    std::shared_ptr<const StateSample> latest_;
    std::exception_ptr error_;
    std::jthread worker_;
};
}
