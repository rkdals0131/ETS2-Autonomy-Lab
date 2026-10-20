#pragma once
#include "drive_api.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>

namespace ot {
// What the game reads from the semantical device besides the three axes: one
// retarderN pulse (-1 none) and the engine brake held.
struct DriveAuxFrame {
    int32_t retarder_pulse=-1;
    bool engine_brake=false;
    bool operator==(const DriveAuxFrame&) const=default;
};
// The resident input callback and command worker share only this small state.
// Values are supplied by the native boundary after decoding and range checks.
class DriveControl {
public:
    // A retarderN input selects step N while it is held. The step is pulsed when the
    // command changes and again every kRetarderRefreshMs, so a step changed by hand
    // returns to the command; after the controller lets go, step 0 is pulsed once.
    static constexpr uint64_t kRetarderPulseMs=50,kRetarderRefreshMs=500;
    void configure(bool available,bool permitted,bool supported,const char* error="") {
        std::lock_guard lock(mutex_);release_locked(OtDriveReason::disabled);
        status_.available=available;status_.permitted=permitted;status_.profile_supported=supported;
        copy(status_.error,error);
    }
    bool request(const OtDriveRequest& request,uint64_t now,OtDriveStatus& result,uint32_t axes=ot_drive_all,uint32_t* actual_axes=nullptr,
                 const OtDriveAux* aux=nullptr,OtDriveAux* actual_aux=nullptr,uint32_t allowed_axes=ot_drive_all) {
        std::lock_guard lock(mutex_);expire(now);
        bool accepted=true;
        // The auxiliary brakes go with the pedals: whoever owns them owns the deceleration.
        const bool axes_ok=axes && axes<=allowed_axes && (!(axes&ot_drive_auxiliary) || (axes&ot_drive_pedals));
        if(request.action==OtDriveAction::arm) {
            if(!status_.available || !status_.permitted || !status_.profile_supported || !source_available_ || paused_ || status_.armed ||
               !axes_ok || (manual_axes_&axes)) accepted=false;
            else {axes_=axes;++status_.epoch;copy(status_.owner,request.owner);status_.armed=1;status_.reason=OtDriveReason::armed;status_.deadline_ms=now+200;status_.sequence=0;}
        } else if(request.action==OtDriveAction::command || request.action==OtDriveAction::disarm) {
            if(!status_.armed || request.epoch!=status_.epoch || std::strcmp(status_.owner,request.owner)) accepted=false;
            else if(request.action==OtDriveAction::disarm) release_locked(OtDriveReason::disarmed);
            else if(request.sequence<=status_.sequence || request.deadline_ms<=now || request.deadline_ms>now+200 || !axes_ok ||
                    ((axes&ot_drive_auxiliary) && (!aux || aux->retarder<0 || aux->retarder>ot_drive_retarder_steps))) accepted=false;
            else if(manual_axes_&axes) {release_locked(OtDriveReason::manual);accepted=false;}
            else {
                axes_=axes;status_.sequence=request.sequence;status_.deadline_ms=request.deadline_ms;status_.steering=(axes&ot_drive_steering)?request.steering:0;status_.throttle=(axes&ot_drive_pedals)?request.throttle:0;status_.brake=(axes&ot_drive_pedals)?request.brake:0;status_.active=1;status_.reason=OtDriveReason::command;
                if(axes&ot_drive_auxiliary) {aux_=*aux;aux_.engine_brake=aux->engine_brake!=0;aux_active_=true;}
                else if(aux_active_) end_aux_locked();
            }
        }
        result=snapshot_locked(now);if(actual_axes) *actual_axes=axes_;
        if(actual_aux) *actual_aux=aux_active_?aux_:OtDriveAux{-1,0};
        return accepted;
    }
    std::array<float,3> frame(uint64_t now,bool source_available,const std::array<float,3>& manual,uint32_t manual_axes) {
        std::lock_guard lock(mutex_);expire(now);
        source_available_=source_available;
        manual_axes_=manual_axes;
        status_.manual_steering=manual[0];status_.manual_throttle=manual[1];status_.manual_brake=manual[2];
        if(status_.armed && !source_available) release_locked(OtDriveReason::input_unavailable);
        if(status_.armed && (manual_axes_&axes_)) release_locked(OtDriveReason::manual);
        aux_frame_=aux_locked(now);
        return status_.active?std::array<float,3>{-status_.steering,status_.throttle,status_.brake}:std::array<float,3>{};
    }
    // The auxiliary inputs for the frame that the last frame() call computed.
    DriveAuxFrame aux_frame() {std::lock_guard lock(mutex_);return aux_frame_;}
    void release(OtDriveReason reason) {std::lock_guard lock(mutex_);release_locked(reason);}
    void paused(bool value) {std::lock_guard lock(mutex_);paused_=value;if(value) release_locked(OtDriveReason::paused);}
private:
    template<size_t N> static void copy(char(&target)[N],const char* source) {
        const auto count=std::min(std::strlen(source),N-1);std::memcpy(target,source,count);target[count]=0;
    }
    void expire(uint64_t now) {if(status_.armed && now>=status_.deadline_ms) release_locked(OtDriveReason::expired);}
    void end_aux_locked() {
        // Hand the retarder back off: a step left on would brake against the driver.
        if(aux_active_ && (aux_.retarder>0 || pulse_step_>0)) release_pulse_=true;
        aux_active_=false;aux_={};
    }
    void release_locked(OtDriveReason reason) {
        status_.armed=status_.active=0;status_.deadline_ms=0;status_.steering=status_.throttle=status_.brake=0;
        axes_=0;end_aux_locked();
        status_.reason=reason;status_.owner[0]=0;
    }
    DriveAuxFrame aux_locked(uint64_t now) {
        if(release_pulse_) {release_pulse_=false;pulse_step_=0;pulse_until_=now+kRetarderPulseMs;last_pulse_=now;}
        else if(aux_active_ && (aux_.retarder!=pulse_step_ || now-last_pulse_>=kRetarderRefreshMs)) {
            pulse_step_=aux_.retarder;pulse_until_=now+kRetarderPulseMs;last_pulse_=now;
        }
        DriveAuxFrame result;
        if(now<pulse_until_) result.retarder_pulse=pulse_step_;
        result.engine_brake=aux_active_ && aux_.engine_brake;
        return result;
    }
    OtDriveStatus snapshot_locked(uint64_t now) const {auto result=status_;result.command_window_ms=status_.armed?now+200:0;return result;}
    std::mutex mutex_;
    OtDriveStatus status_;
    bool source_available_=false,paused_=true;
    uint32_t axes_=0,manual_axes_=0;
    OtDriveAux aux_{};
    bool aux_active_=false,release_pulse_=false;
    int32_t pulse_step_=-1;
    uint64_t pulse_until_=0,last_pulse_=0;
    DriveAuxFrame aux_frame_{};
};
// SDK asks for each input separately. Only first_in_frame replaces this snapshot.
// Inputs 0..2 are the float axes, 3..8 the retarder0..5 buttons, 9 motorbrake.
class DriveFrame {
public:
    static constexpr uint32_t kInputs=10,kRetarderFirst=3,kMotorbrake=9;
    void begin(std::array<float,3> values,DriveAuxFrame aux={}) {
        for(size_t i=0;i<3;++i) values_[i]=values[i];
        for(uint32_t i=kRetarderFirst;i<kInputs;++i) values_[i]=0;
        if(aux.retarder_pulse>=0 && aux.retarder_pulse<=ot_drive_retarder_steps) values_[kRetarderFirst+aux.retarder_pulse]=1;
        values_[kMotorbrake]=aux.engine_brake?1.f:0.f;
        next_=0;
    }
    bool next(uint32_t& index,float& value) {if(next_==kInputs) return false;index=next_;value=values_[next_++];return true;}
    static bool is_button(uint32_t index) {return index>=kRetarderFirst;}
private:
    std::array<float,kInputs> values_{};uint32_t next_=kInputs;
};
}
