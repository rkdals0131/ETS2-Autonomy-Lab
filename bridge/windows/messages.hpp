#pragma once
#include "wire.hpp"
#include <set>
namespace bridge {
using Demand=std::set<std::string>;
struct SensorBundle {json manifest;Bytes data;size_t blob_offset=0;};
SensorBundle decode_bundle(Bytes data);
json lidar_patterns(const json& rig,const json& profile);
Packet state_messages(const json& state,const std::string& session,const std::array<double,3>& base);
Packet static_messages(const json& rig,const json& patterns,const std::string& session);
void add_diagnostics(Packet& packet,const json& values);
Packet sensor_messages(const SensorBundle& bundle,const std::string& session,const Demand& demand,uint64_t dropped,uint64_t stream_id,const json& rig);
}
