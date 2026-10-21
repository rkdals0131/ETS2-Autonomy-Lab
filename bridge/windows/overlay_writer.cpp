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
ot::OverlayContent overlay_content(const OverlayMarks& marks,const PoseHistory& history,std::string* reason) {
    ot::OverlayContent content;
    content.stamp_us=marks.stamp_us;content.ttl_ms=marks.ttl_ms;
    const auto fail=[&](const char* why) {if(reason) *reason=why;content.valid=false;content.point_count=content.stroke_count=0;return content;};
    if(marks.strokes.empty()) return fail("cleared by empty marks");
    const auto newest=history.latest();
    std::optional<StampedPose> pose;
    if(marks.frame=="base_link") {
        pose=history.at(marks.stamp_us);
        if(!pose) return fail(newest?"marks stamp precedes the pose history":"no SDK pose yet");
    } else if(marks.frame!="world") return fail("frame_id must be base_link or world");
    if(marks.stamp_us && newest && newest->stamp_us>marks.stamp_us && newest->stamp_us-marks.stamp_us>overlay_max_age_us)
        return fail("marks stamp is older than 500 ms");
    // SCS world X, Z per point (ROS world is X, -Z, Y), each stroke cut at 200 m.
    std::vector<std::vector<std::array<double,2>>> strokes;
    for(const auto& stroke:marks.strokes) {
        if(strokes.size()==ot::overlay_max_strokes) break;
        if(stroke.points.size()<2) return fail("a stroke needs two points");
        if(!std::isfinite(stroke.width_m) || stroke.width_m<=0 || !std::isfinite(stroke.min_px) || stroke.min_px<0) return fail("invalid stroke width");
        auto& xz=strokes.emplace_back();double length=0;
        for(const auto& p:stroke.points) {
            if(!finite(p)) return fail("nonfinite stroke point");
            const auto w=pose?add(pose->pose.position,mul(pose->pose.rotation,p)):p;
            if(!xz.empty()) {
                length+=std::hypot(w[0]-xz.back()[0],-w[1]-xz.back()[1]);
                if(length>200) break;
            }
            xz.push_back({w[0],-w[1]});
        }
    }
    // Thin to the block: each stroke keeps its share of the 256 points, at least two.
    size_t total=0;for(const auto& xz:strokes) total+=xz.size();
    uint32_t next=0;
    for(size_t k=0;k<strokes.size();++k) {
        const auto& xz=strokes[k];const auto& in=marks.strokes[k];
        const auto budget=total<=ot::overlay_max_points?xz.size():2+xz.size()*(ot::overlay_max_points-2*strokes.size())/total;
        const auto count=std::min(xz.size(),std::min<size_t>(budget,ot::overlay_max_points-next));
        if(count<2) break;
        for(size_t i=0;i<count;++i) {
            const auto source=count==xz.size()?i:static_cast<size_t>(std::llround(double(i)*(xz.size()-1)/(count-1)));
            content.points[next+i][0]=xz[source][0];content.points[next+i][1]=xz[source][1];
        }
        auto& stroke=content.strokes[content.stroke_count++];
        stroke.first=next;stroke.count=static_cast<uint32_t>(count);stroke.width_m=in.width_m;stroke.min_px=in.min_px;
        std::copy(in.rgba.begin(),in.rgba.end(),stroke.rgba);
        next+=static_cast<uint32_t>(count);
    }
    content.point_count=next;
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
        // An exception here ended the relay from the ROS executor (2026-10-21: relay ABI 2,
        // core 0.46 ABI 1); a mismatched core only loses the HUD.
        if(block_) UnmapViewOfFile(block_);
        CloseHandle(file_);file_=nullptr;block_=nullptr;
        ++abi_mismatch_;
        return false;
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
    return {{"mapped",block_!=nullptr},{"written",written_},{"invalid",invalid_},{"waiting_for_core",unmapped_},{"abi_mismatch",abi_mismatch_}};
}
}
