// The planner without ROS: the lane change state machine's transitions, and the behavior tree
// (config/behavior.xml) ticked over hand-made worlds.
#include "control/turn_signal.hpp"
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
    // Commanding, the planner wants its own lane change's signal; the driver's never.
    EXPECT_EQ(planning::turn_signal(c),-1);
    c.w.signal=-1;c.w.signal_by=2;tick(20);
    EXPECT_EQ(c.fsm.state(),LaneChangeFsm::WaitGap);              // its own signal is no driver intent
    c.w.signal_cancelled=true;c.w.signal=0;c.w.signal_by=0;tick(20.1);
    EXPECT_EQ(c.fsm.state(),LaneChangeFsm::Abort);                 // the driver said no
    EXPECT_EQ(planning::turn_signal(c),0);
}

TEST(TurnSignal,PressesTowardTheWantedStateAndLeavesTheDriversAlone) {
    TurnSignalControl ts;
    ts.update(0,-1,0);EXPECT_EQ(ts.left,1u);                       // on: one press of the left lever
    ts.update(.5,-1,0);EXPECT_EQ(ts.left,1u);                      // not again within a second
    ts.update(.6,-1,-1);EXPECT_EQ(ts.by,2);                        // the lever follows: the assistance's
    ts.update(3,-1,-1);EXPECT_EQ(ts.left,1u);
    ts.update(4,0,-1);EXPECT_EQ(ts.left,2u);                       // off: the left lever again
    ts.update(4.2,0,0);EXPECT_EQ(ts.by,0);
    // The driver's signal: never cancelled or switched.
    ts.update(10,0,1);EXPECT_EQ(ts.by,1);
    ts.update(12,0,1);ts.update(13,-1,1);EXPECT_EQ(ts.left,2u);EXPECT_EQ(ts.right,0u);
    ts.update(14,0,0);
    // A lever that does not follow: three presses, then left alone.
    for(double t=20;t<30;t+=.5) ts.update(t,1,0);
    EXPECT_EQ(ts.right,3u);
    // The driver turning the assistance's signal off: no press until the wanted state changes.
    TurnSignalControl c2;
    c2.update(0,1,0);c2.update(.3,1,1);EXPECT_EQ(c2.by,2);
    c2.update(5,1,0);EXPECT_TRUE(c2.cancelled);
    c2.update(7,1,0);EXPECT_EQ(c2.right,1u);
    c2.update(8,0,0);EXPECT_FALSE(c2.cancelled);
    c2.update(9,1,0);EXPECT_EQ(c2.right,2u);
    // Hazards are not touched.
    TurnSignalControl h;h.update(0,0,2);h.update(2,1,2);EXPECT_EQ(h.left+h.right,0u);
}
