#pragma once
// The turn signal as the assistance sets it (plan stage 3): the state the planner wants
// (-1 left, 0 off, 1 right; /ets2/planning/turn_signal) against the lever state the SDK
// reports, turned into lever presses (the game's lblinker/rblinker toggle; DriveCommand
// counts them), and who set the current state: a change within attribute_s of an own press
// is the assistance's, any other the driver's.
// - Presses at most every retry_s and max_presses times per wanted state, so a press the
//   game did not take is repeated but a lever that does not follow is left alone.
// - The driver's signal is never cancelled or switched: with the driver's lever on, nothing
//   is pressed until it is off again.
// - The driver cancelling the assistance's signal (off without an own press) stands until the
//   wanted state changes (cancelled; the planner reads it as the driver saying no).
// - Hazards (both on) are never touched.
#include <cstdint>
#include <optional>

struct TurnSignalControl {
    double retry_s=1,attribute_s=1;
    int max_presses=3;
    uint32_t left=0,right=0;   // presses so far (DriveCommand left/right_signal_presses)
    int by=0;                  // who set the current state: 0 off, 1 the driver, 2 the assistance
    bool cancelled=false;      // the driver turned the assistance's signal off

    // t: s; wanted: nullopt without a (current) request; lever: -1, 0, 1, or 2 for hazards.
    void update(double t,std::optional<int> wanted,int lever) {
        if(lever!=lever_) {
            const bool own=t-pressed_<=attribute_s;
            if(lever==0 && by==2 && !own) cancelled=true;
            by=lever==0?0:own?2:1;lever_=lever;
        }
        if(!wanted || lever==2) {tries_=0;return;}
        if(*wanted!=wanted_) {wanted_=*wanted;tries_=0;cancelled=false;}
        if(lever==*wanted || (lever!=0 && by==1) || (cancelled && *wanted!=0)) return;
        if(tries_>=max_presses || t-pressed_<retry_s) return;
        // On: press that side. Off: press the side that is on (each lever toggles its side).
        const int side=*wanted!=0?*wanted:lever;
        ++(side<0?left:right);pressed_=t;++tries_;
    }
private:
    int lever_=0,wanted_=0,tries_=0;
    double pressed_=-1e9;
};
