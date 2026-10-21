#pragma once
#include "topics.hpp"
#include "motion_sensors.hpp"
#include <cstdint>
#include <set>
#include <vector>
struct IWICImagingFactory;
namespace bridge {
using Bytes=std::vector<uint8_t>;
// XCDRv1 messages back to back in data; meta.messages lists topic/offset/length.
struct Packet {json meta;Bytes data;};
using Demand=std::set<std::string>;
// /ets2/overlay/path as received (docs/24 §4): base_link with the sensor stamp, or world.
// HUD decal input (docs/24): strokes in one frame (base_link or world) with one stamp. No
// strokes clears the decal. min_px: minimum on-screen half thickness at 1080 lines.
struct OverlayStrokeInput {std::vector<V> points;float width_m=3.5f,min_px=0;std::array<float,4> rgba{1,1,1,.22f};};
struct OverlayMarks {std::string frame;uint64_t stamp_us=0;uint64_t ttl_ms=1000;std::vector<OverlayStrokeInput> strokes;};
struct SensorBundle {json manifest;Bytes data;size_t blob_offset=0;};
SensorBundle decode_bundle(Bytes data);
Packet state_messages(const json& state,const std::string& session,const std::array<double,3>& base,bool gt_localization_tf);
Packet static_messages(const json& rig,const json& patterns,const std::string& session,const json& sensors);
void append_motion_messages(Packet& packet,const MotionSample& sample,const Demand& demand,uint64_t& last_gnss_us);
void add_sensor_configuration(Packet& packet,const json& configuration);
void add_diagnostics(Packet& packet,const json& values);
Packet sensor_messages(const SensorBundle& bundle,const std::string& session,const Demand& demand,uint64_t dropped,uint64_t stream_id,const json& rig,IWICImagingFactory* imaging,uint32_t lidar_preview_stride);
}
