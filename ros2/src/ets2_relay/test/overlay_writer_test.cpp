// HUD path pieces of the relay without the game (docs/24 §4, docs/23 §7): the
// SDK pose history, base_link -> SCS world, and the Local\OT_Overlay writer.
#include "overlay_writer.hpp"
#include <gtest/gtest.h>
#include <numbers>

using bridge::json;
using bridge::V;

namespace {
// An SDK state sample with engine.vehicle.pose_physics: SCS position, yaw about +Y.
json state(V position,double yaw) {
    return {{"engine",{{"vehicle",{{"available",true},{"pose_physics",{{"position_m",position},
        {"quaternion_wxyz",{std::cos(yaw/2),0.0,std::sin(yaw/2),0.0}}}}}}}}};
}
const V base{0,-.5,2}; // base_link origin in the truck model, like a rig's base_origin
std::unique_ptr<bridge::PoseHistory> history(std::initializer_list<std::pair<uint64_t,json>> samples) {
    auto h=std::make_unique<bridge::PoseHistory>();
    for(const auto& [stamp,sample]:samples) h->push(stamp,*bridge::engine_base_pose(sample,base));
    return h;
}
// One stroke, like a /ets2/overlay/path message.
bridge::OverlayMarks path(std::string frame,uint64_t stamp,std::vector<V> points) {
    bridge::OverlayMarks p;p.frame=std::move(frame);p.stamp_us=stamp;p.strokes.push_back({std::move(points)});return p;
}
}

TEST(OverlayWriter,PoseHistoryPicksNewestAtOrBeforeStamp) {
    const auto h=history({{1000000,state({0,0,0},0)},{1100000,state({0,0,-1},0)},{1200000,state({0,0,-2},0)}});
    EXPECT_EQ(h->at(1150000)->stamp_us,1100000u);
    EXPECT_EQ(h->at(1200000)->stamp_us,1200000u);
    EXPECT_EQ(h->at(5000000)->stamp_us,1200000u);
    EXPECT_EQ(h->at(0)->stamp_us,1200000u);          // zero stamp: newest pose
    EXPECT_FALSE(h->at(999999).has_value());         // older than the history
    bridge::PoseHistory two(2);
    for(uint64_t t=1;t<=5;++t) two.push(t,*bridge::engine_base_pose(state({0,0,0},0),base));
    EXPECT_FALSE(two.at(3).has_value());
    two.push(1,*bridge::engine_base_pose(state({0,0,0},0),base)); // clock restart
    EXPECT_EQ(two.latest()->stamp_us,1u);
    EXPECT_FALSE(bridge::engine_base_pose(json{{"engine",{{"vehicle",{{"available",false}}}}}},base).has_value());
}

TEST(OverlayWriter,BaseLinkPathBecomesScsWorld) {
    // Facing SCS north (-Z): base_link x ahead is -Z, y left is -X; the base
    // origin offset and the road height (ground_y) come from the same pose.
    const V truck{-12995.5,41.25,837.5};
    const auto h=history({{1000000,state(truck,0)}});
    std::string reason;
    const auto c=bridge::overlay_content(path("base_link",1000000,{{0,0,0},{10,0,0},{10,2,0}}),*h,&reason);
    ASSERT_TRUE(c.valid) << reason;
    ASSERT_EQ(c.point_count,3u);
    const double x0=truck[0]+base[0],z0=truck[2]+base[2];
    EXPECT_NEAR(c.points[0][0],x0,1e-9);EXPECT_NEAR(c.points[0][1],z0,1e-9);
    EXPECT_NEAR(c.points[1][0],x0,1e-9);EXPECT_NEAR(c.points[1][1],z0-10,1e-9);
    EXPECT_NEAR(c.points[2][0],x0-2,1e-9);EXPECT_NEAR(c.points[2][1],z0-10,1e-9);
    EXPECT_NEAR(c.ground_y,truck[1]+base[1],1e-9);
    // Yaw 90 degrees about +Y turns ahead to -X.
    const auto turned=bridge::overlay_content(path("base_link",0,{{0,0,0},{10,0,0}}),*history({{5,state({0,0,0},std::numbers::pi/2)}}));
    ASSERT_TRUE(turned.valid);
    EXPECT_NEAR(turned.points[1][0]-turned.points[0][0],-10,1e-9);
    EXPECT_NEAR(turned.points[1][1]-turned.points[0][1],0,1e-9);
}

TEST(OverlayWriter,StampSelectsThePoseAndOldPathsAreInvalid) {
    const auto h=history({{1000000,state({0,0,0},0)},{1100000,state({5,0,0},0)},{1700000,state({9,0,0},0)}});
    // Sensor stamp between samples: the pose at or before it (x = 0), not the newest.
    const auto c=bridge::overlay_content(path("base_link",1650000,{{0,0,0},{1,0,0}}),*h);
    ASSERT_TRUE(c.valid);EXPECT_NEAR(c.points[0][0],5+base[0],1e-9);
    std::string reason;
    const auto stale=bridge::overlay_content(path("base_link",1100000,{{0,0,0},{1,0,0}}),*h,&reason);
    EXPECT_FALSE(stale.valid);EXPECT_EQ(stale.point_count,0u);EXPECT_NE(reason.find("500 ms"),std::string::npos);
    EXPECT_FALSE(bridge::overlay_content(path("base_link",900000,{{0,0,0},{1,0,0}}),*h).valid);
    EXPECT_FALSE(bridge::overlay_content(path("map",0,{{0,0,0},{1,0,0}}),*h).valid);
    EXPECT_FALSE(bridge::overlay_content(path("base_link",0,{{0,0,0}}),*h).valid);
    EXPECT_FALSE(bridge::overlay_content(path("base_link",0,{{0,0,0},{1,0,0}}),bridge::PoseHistory{}).valid);
}

TEST(OverlayWriter,WorldPathsAndLimits) {
    const auto h=history({{1000000,state({100,7,-200},0)}});
    // ROS world (x, y) is SCS (X, -Z).
    const auto c=bridge::overlay_content(path("world",0,{{-12995.75,837.5,0},{-12997.25,838.0,0}}),*h);
    ASSERT_TRUE(c.valid);
    EXPECT_DOUBLE_EQ(c.points[0][0],-12995.75);EXPECT_DOUBLE_EQ(c.points[0][1],-837.5);
    EXPECT_NEAR(c.ground_y,7+base[1],1e-9);
    // World without any pose: drawn without the height test (ground unknown).
    const auto unknown=bridge::overlay_content(path("world",0,{{0,0,0},{1,0,0}}),bridge::PoseHistory{});
    EXPECT_TRUE(unknown.valid);EXPECT_TRUE(std::isnan(unknown.ground_y));
    // Thinned to 256 points with both ends kept; cut after 200 m.
    std::vector<V> dense;for(int i=0;i<1000;++i) dense.push_back({i*.1,0,0});
    const auto thin=bridge::overlay_content(path("world",0,dense),*h);
    EXPECT_EQ(thin.point_count,ot::overlay_max_points);
    EXPECT_DOUBLE_EQ(thin.points[0][0],0);EXPECT_DOUBLE_EQ(thin.points[255][0],99.9);
    std::vector<V> long_path;for(int i=0;i<500;++i) long_path.push_back({double(i),0,0});
    const auto cut=bridge::overlay_content(path("world",0,long_path),*h);
    EXPECT_EQ(cut.point_count,201u);
}

TEST(OverlayWriter,StrokesShareThePointPool) {
    const auto h=history({{1000000,state({0,0,0},0)}});
    // A long lane ribbon and a two-point bar: both kept, the bar whole, together 256 points.
    bridge::OverlayMarks marks;marks.frame="world";
    std::vector<V> ribbon;for(int i=0;i<1000;++i) ribbon.push_back({i*.1,0,0});
    marks.strokes.push_back({ribbon,.2f,1,{1,1,1,.6f}});
    marks.strokes.push_back({{{50,-1.6,0},{50,1.6,0}},.4f,2,{1,.6f,.1f,.8f}});
    const auto c=bridge::overlay_content(marks,*h);
    ASSERT_TRUE(c.valid);ASSERT_EQ(c.stroke_count,2u);
    EXPECT_LE(c.point_count,ot::overlay_max_points);EXPECT_GE(c.point_count,250u);
    EXPECT_EQ(c.strokes[0].first,0u);EXPECT_EQ(c.strokes[1].first,c.strokes[0].count);EXPECT_EQ(c.strokes[1].count,2u);
    EXPECT_EQ(c.strokes[0].first+c.strokes[0].count+c.strokes[1].count,c.point_count);
    EXPECT_FLOAT_EQ(c.strokes[1].width_m,.4f);EXPECT_FLOAT_EQ(c.strokes[1].min_px,2);EXPECT_FLOAT_EQ(c.strokes[1].rgba[1],.6f);
    EXPECT_DOUBLE_EQ(c.points[c.strokes[1].first][1],1.6); // ROS y -> SCS -Z
    // No strokes clears; a stroke without width is invalid; strokes past 8 are dropped.
    std::string reason;
    EXPECT_FALSE(bridge::overlay_content(bridge::OverlayMarks{"world"},*h,&reason).valid);EXPECT_NE(reason.find("empty"),std::string::npos);
    marks.strokes[1].width_m=0;EXPECT_FALSE(bridge::overlay_content(marks,*h).valid);
    bridge::OverlayMarks many;many.frame="world";
    for(int i=0;i<10;++i) many.strokes.push_back({{{0,double(i),0},{1,double(i),0}}});
    EXPECT_EQ(bridge::overlay_content(many,*h).stroke_count,ot::overlay_max_strokes);
}

TEST(OverlayWriter,WritesTheSharedBlock) {
    const std::wstring name=L"Local\\OT_Overlay_test_"+std::to_wstring(GetCurrentProcessId());
    bridge::OverlayWriter writer(name);
    auto c=std::make_unique<ot::OverlayContent>();c->valid=true;c->point_count=2;c->stamp_us=42;
    c->stroke_count=1;c->strokes[0]={0,2,3.5f,0,{1,1,1,.5f}};
    c->points[1][0]=1;c->points[1][1]=-1;
    EXPECT_FALSE(writer.write(*c)); // the core has not created it yet
    bridge::Handle file(CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(ot::OverlayBlock),name.c_str()));
    ASSERT_TRUE(file.h);
    auto* block=static_cast<ot::OverlayBlock*>(MapViewOfFile(file.h,FILE_MAP_ALL_ACCESS,0,0,sizeof(ot::OverlayBlock)));
    ASSERT_TRUE(block);
    ot::overlay_initialize(block);
    EXPECT_TRUE(writer.write(*c));
    auto out=std::make_unique<ot::OverlayContent>();LONG64 generation{};
    ASSERT_EQ(ot::overlay_read(block,0,*out,generation),1);
    EXPECT_EQ(generation,2);EXPECT_TRUE(out->valid);EXPECT_EQ(out->stamp_us,42u);EXPECT_EQ(out->points[1][1],-1);
    writer.clear();
    ASSERT_EQ(ot::overlay_read(block,generation,*out,generation),1);
    EXPECT_FALSE(out->valid);EXPECT_EQ(out->point_count,0u);
    EXPECT_EQ(writer.status().at("written"),1);
    UnmapViewOfFile(block);
}

int main(int argc,char** argv) {
    testing::InitGoogleTest(&argc,argv);
    return RUN_ALL_TESTS();
}
