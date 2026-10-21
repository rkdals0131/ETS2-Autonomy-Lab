#pragma once
#include "messages.hpp"
#include "windows_support.hpp"
#include "../../ot/native/ipc_layout.hpp"
#include <deque>
#include <memory>
#include <mutex>
#include <optional>

namespace bridge {
// The relay's own SDK pose history for the HUD path (docs/23 §7, docs/24 §4):
// base_link in ROS world per SDK frame, stamped with paused simulation time.
// The stack publishes base_link paths and never sees this ground truth.
struct StampedPose {uint64_t stamp_us;BasePose pose;};
class PoseHistory {
public:
    explicit PoseHistory(size_t capacity=128):capacity_(capacity) {}
    void push(uint64_t stamp_us,const BasePose& pose);
    // Newest pose stamped at or before stamp_us; zero means the newest pose.
    std::optional<StampedPose> at(uint64_t stamp_us) const;
    std::optional<StampedPose> latest() const;
private:
    const size_t capacity_;
    mutable std::mutex mutex_;
    std::deque<StampedPose> poses_;
};
// base_link from an SDK state sample (engine.vehicle.pose_physics, Tier 1).
std::optional<BasePose> engine_base_pose(const json& state,const V& base);
inline constexpr uint64_t overlay_max_age_us=500000;
// ROS marks -> OT_Overlay content in SCS world (X = x, Z = -y, Y = z). base_link
// points use the pose at the stamp; marks older than 500 ms than the newest pose,
// or with no pose, are written invalid. Each stroke is cut at 200 m; strokes beyond
// the block's 8 are dropped and all are thinned together to its 256 points (at
// least two each).
ot::OverlayContent overlay_content(const OverlayMarks& marks,const PoseHistory& history,std::string* reason=nullptr);
// Writer side of Local\OT_Overlay; the core creates the mapping on overlay enable.
class OverlayWriter {
public:
    explicit OverlayWriter(std::wstring name=L"Local\\OT_Overlay"):name_(std::move(name)) {}
    ~OverlayWriter();
    OverlayWriter(const OverlayWriter&)=delete;
    // False while the core has not created the mapping yet, or when its ABI differs (a core
    // older or newer than this relay): the HUD is then off, the relay keeps running.
    bool write(const ot::OverlayContent& content);
    void clear();
    json status();
private:
    bool open();
    std::wstring name_;
    std::mutex mutex_;
    HANDLE file_=nullptr;
    ot::OverlayBlock* block_=nullptr;
    uint64_t written_=0,unmapped_=0,invalid_=0,abi_mismatch_=0;
};
}
