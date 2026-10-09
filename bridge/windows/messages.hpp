#pragma once
#include "wire.hpp"
#include <set>
struct IWICImagingFactory;
namespace bridge {
using Demand=std::set<std::string>;
struct SensorBundle {json manifest;Bytes data;size_t blob_offset=0;};
SensorBundle decode_bundle(Bytes data);
json lidar_patterns(const json& rig,const json& profile);
Packet state_messages(const json& state,const std::string& session,const std::array<double,3>& base);
Packet static_messages(const json& rig,const json& patterns,const std::string& session,const json& settings);
class MotionSensors {
public:
    MotionSensors(const json& rig,const json& truck,const json& settings);
    void configuration(Packet& packet) const;
    void append(Packet& packet,const json& state,const Demand& demand);
private:
    struct Wheel {uint32_t index;std::array<double,3> position;double radius;};
    std::vector<Wheel> wheels_;
    std::array<double,3> base_,mount_,gnss_mount_,reference_,anchor_{},previous_velocity_{},previous_omega_{};
    uint64_t previous_us_=0,next_gnss_us_=0,generation_=0;
    bool anchored_=false;
    double x_=0,y_=0,yaw_=0;
};
void add_diagnostics(Packet& packet,const json& values);
Packet sensor_messages(const SensorBundle& bundle,const std::string& session,const Demand& demand,uint64_t dropped,uint64_t stream_id,const json& rig,IWICImagingFactory* imaging);
}
