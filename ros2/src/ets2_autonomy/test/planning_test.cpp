// The planner without ROS: the lane change state machine's transitions, and the behavior tree
// (config/behavior.xml) ticked over hand-made worlds.
#include "planning/behavior.hpp"
#include <gtest/gtest.h>
#include <cmath>
#include <vector>

using planning::LaneChangeFsm;

namespace {
LaneChangeFsm::Input in(double t,bool wanted,int side,int active=0,std::string gap="") {
    return {t,wanted,side,true,std::move(gap),active,true,0};
}
// A straight lane along x at lateral offset y.
LanePath straight(double y) {
    std::vector<std::array<double,2>> xy;
    for(double x=-300;x<=600;x+=5) xy.push_back({x,y});
    return LanePath(xy,3.5);
}
} // namespace

TEST(LaneChangeFsm,DriverSignalThroughExecution) {
    LaneChangeFsm fsm;
    EXPECT_EQ(fsm.update(in(0,true,-1)),LaneChangeFsm::Signal);
    EXPECT_EQ(fsm.update(in(2,true,-1,0,"vehicle behind")),LaneChangeFsm::Signal);          // 3 s of signal first
    EXPECT_EQ(fsm.update(in(3,true,-1,0,"vehicle behind")),LaneChangeFsm::WaitGap);
    EXPECT_EQ(fsm.note(),"waiting: vehicle behind");
    EXPECT_EQ(fsm.update(in(4,true,-1,-1)),LaneChangeFsm::Execute);                         // drive_speed moves over
    EXPECT_EQ(fsm.update(in(8,true,-1,0)),LaneChangeFsm::Done);
    // Done holds while the signal stays on, then Idle; no second lane change from it.
    EXPECT_EQ(fsm.update(in(9,true,-1,0)),LaneChangeFsm::Done);
    EXPECT_EQ(fsm.update(in(10,false,0,0)),LaneChangeFsm::Idle);
}

TEST(LaneChangeFsm,AbortsWhenTheReasonGoesOrTheLaneIsMissing) {
    LaneChangeFsm fsm;
    fsm.update(in(0,true,1));
    EXPECT_EQ(fsm.update(in(1,false,0)),LaneChangeFsm::Abort);
    EXPECT_EQ(fsm.update(in(2,false,0)),LaneChangeFsm::Idle);
    auto none=in(3,true,1);none.target_lane=false;
    fsm.update(none);
    EXPECT_EQ(fsm.update(none),LaneChangeFsm::Abort);
    EXPECT_EQ(fsm.note(),"no lane on that side");
    // A change of side restarts through Abort.
    fsm.reset();fsm.update(in(10,true,-1));
    EXPECT_EQ(fsm.update(in(11,true,1)),LaneChangeFsm::Abort);
}

TEST(LaneChangeFsm,FollowsTheExecutorWhateverWasPlanned) {
    LaneChangeFsm fsm;
    EXPECT_EQ(fsm.update(in(0,false,0,1)),LaneChangeFsm::Execute);
    EXPECT_EQ(fsm.side(),1);
    EXPECT_EQ(fsm.update(in(1,false,0,0)),LaneChangeFsm::Done);
    EXPECT_EQ(fsm.update(in(2,false,0,0)),LaneChangeFsm::Idle);
}

TEST(World,GapRouteSideAndTtc) {
    planning::World w;
    w.x=0;w.y=0;w.speed=25;
    const auto left=straight(3.5);
    EXPECT_EQ(planning::gap_problem(w,left),"");
    w.objects.push_back({1,-12,3.5,0,30,12,2.5});                 // beside the trailer, faster
    EXPECT_EQ(planning::gap_problem(w,left),"vehicle beside");
    w.objects[0].x=-40;                                           // 19 m behind the trailer closing at 5 m/s (needs 20)
    EXPECT_EQ(planning::gap_problem(w,left),"vehicle behind");
    w.objects[0].z=8;w.objects[0].height=3;                       // on an overpass: not in the lane
    EXPECT_EQ(planning::gap_problem(w,left),"");
    w.route={{900,{1,2},0}};w.path_s=0;
    EXPECT_EQ(planning::route_side(w),1);
    w.path_s=-1000;EXPECT_EQ(planning::route_side(w),0);            // 1.9 km ahead: not yet
    w.lead=true;w.lead_gap=30;w.lead_speed=5;
    EXPECT_NEAR(planning::lead_ttc(w),1.5,1e-9);
}

TEST(BehaviorTree,ShadowDecisions) {
    planning::Context c;
    BT::BehaviorTreeFactory factory;
    planning::register_nodes(factory,c);
    auto tree=factory.createTreeFromFile(ETS2_BEHAVIOR_TREE);
    const auto tick=[&](double t) {c.w.t=t;c.begin();tree.tickOnce();c.end();return c.decision;};
    // Assistance off.
    EXPECT_EQ(tick(0),"standby (assistance off)");
    // ACC on, nothing to do.
    c.w.acc=c.w.lcc=true;c.w.speed=c.w.set_speed=25;c.w.left=straight(3.5);
    EXPECT_EQ(tick(.1),"keep lane");
    // The driver signals left: a lane change, then the executor runs it.
    c.w.signal=-1;c.signal_since=.2;
    EXPECT_EQ(tick(.2),"lane change left (driver): signalling");
    EXPECT_EQ(c.fsm.state(),LaneChangeFsm::Signal);
    tick(3.3);
    EXPECT_EQ(c.fsm.state(),LaneChangeFsm::WaitGap);
    c.w.change_active=-1;tick(3.4);
    EXPECT_EQ(c.fsm.state(),LaneChangeFsm::Execute);
    c.w.change_active=0;tick(6);
    EXPECT_EQ(c.fsm.state(),LaneChangeFsm::Done);
    c.w.signal=0;tick(6.1);
    EXPECT_EQ(c.fsm.state(),LaneChangeFsm::Idle);
    EXPECT_EQ(c.decision,"keep lane");
    // A closing lead 1.5 s away pre-empts everything.
    c.w.lead=true;c.w.lead_gap=15;c.w.lead_speed=15;
    EXPECT_EQ(tick(7),"emergency: lead 1.5 s away");
    // A slow lead for 8 s with the left lane free: overtake (shadow: would start).
    c.w.lead_gap=60;c.w.lead_speed=20;
    for(double t=8;t<16.3;t+=.1) tick(t);
    EXPECT_EQ(c.fsm.side(),-1);
    for(double t=16.3;t<20;t+=.1) tick(t);
    EXPECT_EQ(c.fsm.state(),LaneChangeFsm::WaitGap);
    EXPECT_EQ(c.decision,"lane change left (overtake): gap free, would start");
}
