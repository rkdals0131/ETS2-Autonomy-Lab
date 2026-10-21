#pragma once
// The lane change maneuver as a state machine (plan stage 2): what a lane change is doing,
// one state at a time, for the behavior tree's ChangeLane action and the status board.
//   Idle -> Signal: a lane change is wanted (driver's signal, route exit, overtake)
//   Signal -> WaitGap: the turn signal has been on long enough (min_signal_s; the driver's
//     own signal counts from when it went on)
//   WaitGap -> Execute: the executor moves over (drive_speed's lane change in progress)
//   Execute -> Done: the executor finished (no lane change in progress any more)
//   any -> Abort: the reason went away before Execute (signal off, target lane gone), or the
//     executor gave up
// In shadow mode the executor is drive_speed and only the driver's signal moves it; a route
// or overtake lane change waits in WaitGap with "would start" once the gap is free.
#include <array>
#include <string>

namespace planning {

class LaneChangeFsm {
public:
    enum State {Idle,Signal,WaitGap,Execute,Done,Abort,Count};
    static constexpr std::array<const char*,Count> names{"Idle","Signal","WaitGap","Execute","Done","Abort"};
    struct Input {
        double t;
        bool wanted;          // the reason still holds
        int side;             // -1 left, 1 right
        bool target_lane;     // a lane exists on that side
        std::string gap;      // gap_problem(): "" when free
        int active;           // executor: side in progress, 0 none
        bool driver;          // the driver's signal is the reason (its on time is signal_since)
        double signal_since;  // when that signal went on
    };
    double min_signal_s=3;

    State state() const {return state_;}
    int side() const {return side_;}
    const std::string& note() const {return note_;}
    void reset() {state_=Idle;side_=0;note_.clear();}

    // Once per planner tick, after the behavior tree said what it wants.
    State update(const Input& in) {
        // The executor moving over decides, whatever was planned.
        if(state_!=Execute && in.active!=0 && (state_==Idle || in.active==side_ || state_==Done || state_==Abort)) return go(Execute,in.active,"moving over");
        if(state_==Idle || state_==Done || state_==Abort) {
            // A finished or failed lane change holds until its reason goes (the driver's signal
            // stays on after the change); then Idle, and a new reason starts over.
            if(!in.wanted) {if(state_!=Idle) reset();return state_;}
            if(state_!=Idle) return state_;
            go(Signal,in.side,"turn signal on");since_=in.driver?in.signal_since:in.t;
        }
        if((state_==Signal || state_==WaitGap) && in.wanted && in.side!=side_) return go(Abort,side_,"the other side wanted");
        switch(state_) {
        case Signal:
            if(!in.wanted) return go(Abort,side_,"reason gone");
            if(!in.target_lane) return go(Abort,side_,"no lane on that side");
            if(in.t-since_>=min_signal_s) go(WaitGap,side_,in.gap.empty()?"gap free":"waiting: "+in.gap);
            else note_="signalling";
            break;
        case WaitGap:
            if(!in.wanted) return go(Abort,side_,"reason gone");
            if(!in.target_lane) return go(Abort,side_,"no lane on that side");
            note_=in.gap.empty()?"gap free, would start":"waiting: "+in.gap;
            break;
        case Execute:
            if(in.active==0) return go(Done,side_,"in the new lane");
            break;
        default: break;
        }
        return state_;
    }
private:
    State go(State s,int side,std::string note) {state_=s;side_=side;note_=std::move(note);return state_;}
    State state_=Idle;
    int side_=0;
    double since_=0;
    std::string note_;
};

} // namespace planning
