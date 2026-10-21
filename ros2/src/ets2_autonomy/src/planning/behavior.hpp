#pragma once
// The behavior tree's nodes (BehaviorTree.CPP 4) over a planning::World, and the per-tick
// bookkeeping between the tree and the lane change state machine. The tree itself is XML
// (config/behavior.xml), so its shape changes without a rebuild. Each tick: begin(), the tree
// ticks (conditions read the world; ChangeLane records which side it wants and why), end()
// steps the state machine once and writes the decision.
#include "planning/lane_change_fsm.hpp"
#include "planning/world.hpp"
#include <behaviortree_cpp/bt_factory.h>
#include <cstdio>
#include <string>

namespace planning {

struct Context {
    World w;
    LaneChangeFsm fsm;
    SlowLead slow;
    double signal_since=0;     // when the driver's current signal went on
    // Written by the tree in a tick; the reason of the lane change under way.
    int want_side=0;std::string want_reason,decision,active_reason;

    void begin() {want_side=0;want_reason.clear();decision.clear();}
    void end() {
        const auto& lane=w.side(want_side==0?fsm.side():want_side);
        // The driver turning the assistance's signal off says no to its lane change.
        const bool wanted=want_side!=0 && !(want_reason!="driver" && w.signal_cancelled);
        LaneChangeFsm::Input in{w.t,wanted,want_side,want_side!=0 && w.side(want_side).has_value(),
                                want_side!=0 && lane?gap_problem(w,*lane):"",w.change_active,want_reason=="driver",signal_since};
        fsm.update(in);
        if(wanted) active_reason=want_reason;
        if(decision.empty() && fsm.state()!=LaneChangeFsm::Idle)
            decision=std::string("lane change ")+(fsm.side()<0?"left":"right")+(want_reason.empty()?"":" ("+want_reason+")")+": "+fsm.note();
    }
};

inline const char* side_name(int s) {return s<0?"left":s>0?"right":"none";}

// The turn signal the planner wants when it commands (-1 left, 0 off, 1 right): its own lane
// change's side from the signal until the truck is over, else off (drive_speed never touches
// the driver's own signal, so off is safe to send all the time).
inline int turn_signal(const Context& c) {
    const auto s=c.fsm.state();
    const bool own=c.active_reason!="driver" && (s==LaneChangeFsm::Signal || s==LaneChangeFsm::WaitGap || s==LaneChangeFsm::Execute);
    return own?c.fsm.side():0;
}

// ChangeLane(side, reason): wants a lane change this tick; running while the state machine
// works on it, success when done, failure when aborted.
class ChangeLane : public BT::StatefulActionNode {
public:
    ChangeLane(const std::string& name,const BT::NodeConfig& config,Context& c):StatefulActionNode(name,config),c_(c) {}
    static BT::PortsList providedPorts() {return {BT::InputPort<int>("side"),BT::InputPort<std::string>("reason")};}
    BT::NodeStatus onStart() override {return want();}
    BT::NodeStatus onRunning() override {return want();}
    void onHalted() override {}
private:
    BT::NodeStatus want() {
        const int side=getInput<int>("side").value_or(0);
        if(side==0) return BT::NodeStatus::FAILURE;
        c_.want_side=side;c_.want_reason=getInput<std::string>("reason").value_or("");
        if(c_.fsm.side()!=side) return BT::NodeStatus::RUNNING;
        switch(c_.fsm.state()) {
        case LaneChangeFsm::Done: return BT::NodeStatus::SUCCESS;
        case LaneChangeFsm::Abort: return BT::NodeStatus::FAILURE;
        default: return BT::NodeStatus::RUNNING;
        }
    }
    Context& c_;
};

inline void register_nodes(BT::BehaviorTreeFactory& f,Context& c) {
    using BT::NodeStatus;
    const auto ok=[](bool b) {return b?NodeStatus::SUCCESS:NodeStatus::FAILURE;};
    f.registerSimpleCondition("AssistEngaged",[&c,ok](BT::TreeNode&) {return ok(c.w.acc || c.w.lcc);});
    f.registerSimpleCondition("CollisionImminent",[&c,ok](BT::TreeNode& n) {
        const double limit=n.getInput<double>("ttc_s").value_or(2.0);
        const double ttc=lead_ttc(c.w);
        if(ttc<limit) {char text[64];std::snprintf(text,sizeof text,"emergency: lead %.1f s away",ttc);c.decision=text;}
        return ok(ttc<limit);
    },{BT::InputPort<double>("ttc_s")});
    // The driver's own signal (one the assistance turned on is its plan, not the driver's intent).
    f.registerSimpleCondition("DriverSignal",[&c,ok](BT::TreeNode& n) {
        const bool driver=c.w.signal!=0 && c.w.signal_by!=2;
        if(driver) n.setOutput("side",c.w.signal);
        return ok(driver);
    },{BT::OutputPort<int>("side")});
    f.registerSimpleCondition("RouteExitLane",[&c,ok](BT::TreeNode& n) {
        const int side=route_side(c.w,n.getInput<double>("range_m").value_or(1500));
        if(side!=0) n.setOutput("side",side);
        return ok(side!=0);
    },{BT::InputPort<double>("range_m"),BT::OutputPort<int>("side")});
    f.registerSimpleCondition("SlowLead",[&c,ok](BT::TreeNode&) {return ok(c.slow.update(c.w));});
    f.registerSimpleCondition("LaneFree",[&c,ok](BT::TreeNode& n) {
        const int side=n.getInput<int>("side").value_or(-1);
        const auto& lane=c.w.side(side);
        return ok(lane && gap_problem(c.w,*lane).empty());
    },{BT::InputPort<int>("side")});
    // Leaves that only say what the planner would do (shadow): the executor is drive_speed.
    f.registerSimpleAction("EmergencyBrake",[](BT::TreeNode&) {return NodeStatus::SUCCESS;});
    f.registerSimpleAction("Standby",[&c](BT::TreeNode&) {c.decision="standby (assistance off)";return NodeStatus::SUCCESS;});
    f.registerSimpleAction("KeepLane",[&c](BT::TreeNode&) {if(c.decision.empty()) c.decision="keep lane";return NodeStatus::SUCCESS;});
    f.registerNodeType<ChangeLane>("ChangeLane",std::ref(c));
}

} // namespace planning
