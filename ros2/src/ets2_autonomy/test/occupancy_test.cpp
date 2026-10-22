// LiDAR occupancy over synthetic returns (perception/occupancy.hpp): ground from the lowest
// returns, obstacles above it, GT vehicles explaining theirs, the path check.
#include "perception/occupancy.hpp"
#include <gtest/gtest.h>
#include <cmath>
#include <vector>

using perception::Box;
using perception::Point;

namespace {
// Road returns every 0.5 m over x 0..100, y -10..10, at height grade * x.
std::vector<Point> road(double grade=0) {
    std::vector<Point> out;
    for(double x=0;x<=100;x+=.5) for(double y=-10;y<=10;y+=.5) out.push_back({x,y,grade*x});
    return out;
}
// A block of returns (the side facing the truck and its top) at x0..x1, y0..y1, up to height.
void block(std::vector<Point>& pts,double x0,double x1,double y0,double y1,double height,double base=0) {
    for(double y=y0;y<=y1;y+=.25) for(double z=.3;z<=height;z+=.25) pts.push_back({x0,y,base+z});
    for(double x=x0;x<=x1;x+=.5) for(double y=y0;y<=y1;y+=.5) pts.push_back({x,y,base+height});
}
std::vector<std::array<double,2>> straight() {std::vector<std::array<double,2>> p;for(double x=0;x<=120;x+=2) p.push_back({x,0});return p;}
} // namespace

TEST(Occupancy,ObstacleOnTheLaneIsBlocked) {
    auto pts=road();block(pts,40,42,-1,1,1.5);
    const auto r=perception::analyze(pts,0,0,0,{},{},straight());
    EXPECT_GT(r.unexplained.size(),20u);
    EXPECT_NEAR(r.blocked_m,40,.5);
    // The grid: observed road 0, the obstacle 100 (ego-aligned, x fastest; origin 20 m behind, 20 m right).
    const auto at=[&](double u,double v){return r.cells[size_t(int((v+20)/.5))*r.nx+size_t(int((u+20)/.5))];};
    EXPECT_EQ(at(20,5),0);EXPECT_EQ(at(40.1,0),100);EXPECT_EQ(at(-10,0),-1);
}

TEST(Occupancy,GtVehicleExplainsItsReturns) {
    auto pts=road();block(pts,40,44.5,-.9,.9,1.5);
    const auto r=perception::analyze(pts,0,0,0,{Box{42.25,0,0,4.5,1.8}},{},straight());
    EXPECT_TRUE(r.unexplained.empty());
    EXPECT_GT(r.explained.size(),20u);
    EXPECT_TRUE(std::isinf(r.blocked_m));
}

TEST(Occupancy,SlopesStayGround) {
    for(double grade:{.08,-.08,.1}) {
        const auto r=perception::analyze(road(grade),0,0,0,{},{},straight());
        EXPECT_TRUE(r.unexplained.empty())<<"grade "<<grade;
        EXPECT_TRUE(std::isinf(r.blocked_m));
    }
    // On a 10 % slope a block still stands out.
    auto pts=road(.1);block(pts,60,62,-1,1,1.5,6);
    EXPECT_NEAR(perception::analyze(pts,0,0,0,{},{},straight()).blocked_m,60,.5);
}

TEST(Occupancy,OwnTrailerAndRoadsideAreNotOnThePath) {
    auto pts=road();
    block(pts,-15,-2,-1.2,1.2,4);          // the trailer behind the cab
    block(pts,0,100,4,4.2,.9);             // a guardrail 4 m to the left
    const auto r=perception::analyze(pts,0,0,0,{},{Box{-8,0,0,13.6,2.55}},straight());
    for(const auto& q:r.unexplained) EXPECT_GT(q.y,3.5);    // the guardrail only
    EXPECT_TRUE(std::isinf(r.blocked_m));
    // A few stray returns are not a blocked stretch.
    auto stray=road();stray.push_back({30,0,1});stray.push_back({50,0,1});
    EXPECT_TRUE(std::isinf(perception::analyze(stray,0,0,0,{},{},straight()).blocked_m));
}

TEST(Occupancy,GtBoxGoesBackToTheCloudTime) {
    // 80 km/h lead: the cloud shows it where it was 0.13 s before the GT box (2.9 m back).
    const double v=80/3.6,dt=-.13;
    auto pts=road();block(pts,40-2.9,44.5-2.9,-.9,.9,1.5);
    const Box gt{42.25,0,0,4.5,1.8};
    EXPECT_NEAR(perception::analyze(pts,0,0,0,{gt},{},straight()).blocked_m,37.1,.5);   // the live false positive
    const auto r=perception::analyze(pts,0,0,0,{perception::moved(gt,v,dt)},{},straight());
    EXPECT_TRUE(r.unexplained.empty());
    EXPECT_TRUE(std::isinf(r.blocked_m));
    // Clouds 0.1 s apart: the box covers both positions.
    const auto both=perception::moved(gt,v,dt,.1);
    EXPECT_NEAR(both.length,4.5+v*.1,1e-9);
    EXPECT_TRUE(both.contains(42.25-2.9-2.25-1,0) && both.contains(42.25-2.9+2.25+1,0));
}
