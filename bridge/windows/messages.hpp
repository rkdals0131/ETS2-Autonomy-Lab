#pragma once
#include "wire.hpp"
#include "motion_sensors.hpp"
#include <set>
struct IWICImagingFactory;
namespace bridge {
using Demand=std::set<std::string>;
struct SensorBundle {json manifest;Bytes data;size_t blob_offset=0;};
SensorBundle decode_bundle(Bytes data);
Packet state_messages(const json& state,const std::string& session,const std::array<double,3>& base);
Packet static_messages(const json& rig,const json& patterns,const std::string& session,const json& sensors);
void append_motion_messages(Packet& packet,const MotionSample& sample,const Demand& demand,uint64_t& last_gnss_us);
void add_sensor_configuration(Packet& packet,const json& configuration);
void add_diagnostics(Packet& packet,const json& values);
Packet sensor_messages(const SensorBundle& bundle,const std::string& session,const Demand& demand,uint64_t dropped,uint64_t stream_id,const json& rig,IWICImagingFactory* imaging,uint32_t lidar_preview_stride);
}
