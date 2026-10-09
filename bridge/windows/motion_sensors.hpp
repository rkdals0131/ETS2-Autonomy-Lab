#pragma once
#include "vehicle_profile.hpp"
#include <optional>

namespace bridge {
struct WheelMeasurement {
    std::vector<uint32_t> indices;
    std::vector<double> angular,steering,radius;
    std::vector<bool> ground;
};
struct ImuMeasurement {V force,gyro;};
struct WheelOdometry {double x,y,yaw,speed,yaw_rate;};
struct GnssMeasurement {uint64_t stamp_us;V lla;};
struct MotionSample {
    uint64_t stamp_us=0;
    WheelMeasurement wheels;
    std::optional<ImuMeasurement> imu;
    std::optional<WheelOdometry> odometry;
    std::optional<GnssMeasurement> gnss;
};

// One instance per physical sensor session. Subscription and transport state
// never reset integration, move the geographic anchor, or change sampling.
class MotionSensors {
public:
    MotionSensors(const VehicleProfile& vehicle,const json& settings);
    json configuration() const;
    MotionSample update(const json& state);
private:
    std::vector<WheelGeometry> wheels_;
    V base_,mount_,gnss_mount_,reference_,anchor_{},previous_velocity_{},previous_omega_{};
    uint64_t previous_us_=0,previous_wheel_us_=0,next_gnss_us_=0,generation_=0;
    bool anchored_=false;
    double x_=0,y_=0,yaw_=0;
    std::optional<GnssMeasurement> gnss_;
};
}
