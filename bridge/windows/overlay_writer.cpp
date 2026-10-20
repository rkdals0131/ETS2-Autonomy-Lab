#include "overlay_writer.hpp"
#include <cmath>

namespace bridge {
void PoseHistory::push(uint64_t stamp_us,const BasePose& pose) {
    std::lock_guard lock(mutex_);
    // A restarted clock invalidates the history (the relay ends its session anyway).
    if(!poses_.empty() && stamp_us<poses_.back().stamp_us) poses_.clear();
    if(!poses_.empty() && stamp_us==poses_.back().stamp_us) {poses_.back().pose=pose;return;}
    poses_.push_back({stamp_us,pose});
    while(poses_.size()>capacity_) poses_.pop_front();
}
std::optional<StampedPose> PoseHistory::at(uint64_t stamp_us) const {
    std::lock_guard lock(mutex_);
    if(poses_.empty()) return std::nullopt;
    if(!stamp_us) return poses_.back();
    for(auto i=poses_.rbegin();i!=poses_.rend();++i) if(i->stamp_us<=stamp_us) return *i;
    return std::nullopt;
}
std::optional<StampedPose> PoseHistory::latest() const {return at(0);}
std::optional<BasePose> engine_base_pose(const json& state,const V& base) {
    if(!state.contains("engine") || !state.at("engine").contains("vehicle")) return std::nullopt;
    const auto& vehicle=state.at("engine").at("vehicle");
    if(!vehicle.value("available",false) || !vehicle.contains("pose_physics")) return std::nullopt;
    const auto& p=vehicle.at("pose_physics");
    return world_base_pose(p.at("position_m").get<V>(),from_quat(p.at("quaternion_wxyz").get<Q>()),base);
}
ot::OverlayContent overlay_content(const OverlayPath& path,const PoseHistory& history,std::string* reason) {
    ot::OverlayContent content;
    content.stamp_us=path.stamp_us;content.ttl_ms=path.ttl_ms;content.width_m=path.width_m;
    std::copy(path.rgba.begin(),path.rgba.end(),content.rgba);
    const auto fail=[&](const char* why) {if(reason) *reason=why;content.valid=false;content.point_count=0;return content;};
    const auto newest=history.latest();
    std::optional<StampedPose> pose;
    if(path.frame=="base_link") {
        pose=history.at(path.stamp_us);
        if(!pose) return fail(newest?"path stamp precedes the pose history":"no SDK pose yet");
    } else if(path.frame!="world") return fail("frame_id must be base_link or world");
    if(path.stamp_us && newest && newest->stamp_us>path.stamp_us && newest->stamp_us-path.stamp_us>overlay_max_age_us)
        return fail("path stamp is older than 500 ms");
    if(path.points.size()<2) return fail("a path needs two points");
    // SCS world X, Z per point (ROS world is X, -Z, Y).
    std::vector<std::array<double,2>> xz;xz.reserve(path.points.size());
    double length=0;
    for(const auto& p:path.points) {
        if(!finite(p)) return fail("nonfinite path point");
        const auto w=pose?add(pose->pose.position,mul(pose->pose.rotation,p)):p;
        if(!xz.empty()) {
            length+=std::hypot(w[0]-xz.back()[0],-w[1]-xz.back()[1]);
            if(length>200) break;
        }
        xz.push_back({w[0],-w[1]});
    }
    // Thin evenly to the block size; the mask uses at most 160 m of it.
    const auto count=std::min<size_t>(xz.size(),ot::overlay_max_points);
    for(size_t i=0;i<count;++i) {
        const auto source=count==xz.size()?i:static_cast<size_t>(std::llround(double(i)*(xz.size()-1)/(count-1)));
        content.points[i][0]=xz[source][0];content.points[i][1]=xz[source][1];
    }
    content.point_count=static_cast<uint32_t>(count);
    // The road height at the truck: the base_link origin is a nominal ground point.
    if(const auto ground=pose?pose:newest) content.ground_y=ground->pose.position[2];
    content.valid=true;
    if(reason) reason->clear();
    return content;
}
OverlayWriter::~OverlayWriter() {
    // A relay that ends leaves no path behind for the next overlay enable.
    if(block_) {auto content=std::make_unique<ot::OverlayContent>();ot::overlay_write(block_,*content);UnmapViewOfFile(block_);}
    if(file_) CloseHandle(file_);
}
bool OverlayWriter::open() {
    if(block_) return true;
    file_=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,name_.c_str());
    if(!file_) return false;
    block_=static_cast<ot::OverlayBlock*>(MapViewOfFile(file_,FILE_MAP_ALL_ACCESS,0,0,sizeof(ot::OverlayBlock)));
    if(!block_ || !ot::overlay_header_valid(block_)) {
        if(block_) UnmapViewOfFile(block_);
        CloseHandle(file_);file_=nullptr;block_=nullptr;
        throw std::runtime_error("Unsupported Local\\OT_Overlay ABI");
    }
    return true;
}
bool OverlayWriter::write(const ot::OverlayContent& content) {
    std::lock_guard lock(mutex_);
    if(!open()) {++unmapped_;return false;}
    ot::overlay_write(block_,content);
    ++written_;if(!content.valid) ++invalid_;
    return true;
}
void OverlayWriter::clear() {
    std::lock_guard lock(mutex_);
    if(!block_) return;
    auto content=std::make_unique<ot::OverlayContent>();
    ot::overlay_write(block_,*content);
}
json OverlayWriter::status() {
    std::lock_guard lock(mutex_);
    return {{"mapped",block_!=nullptr},{"written",written_},{"invalid",invalid_},{"waiting_for_core",unmapped_}};
}
}
