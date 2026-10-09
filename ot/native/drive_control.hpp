#pragma once
#include "drive_api.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>

namespace ot {
// The resident input callback and command worker share only this small state.
// Values are supplied by the native boundary after decoding and range checks.
class DriveControl {
public:
    void configure(bool available,bool permitted,bool supported,const char* error="") {
        std::lock_guard lock(mutex_);release_locked(OtDriveReason::disabled);
        status_.available=available;status_.permitted=permitted;status_.profile_supported=supported;
        copy(status_.error,error);
    }
    bool request(const OtDriveRequest& request,uint64_t now,OtDriveStatus& result) {
        std::lock_guard lock(mutex_);expire(now);
        bool accepted=true;
        if(request.action==OtDriveAction::arm) {
            if(!status_.available || !status_.permitted || !status_.profile_supported || !source_available_ || paused_ || status_.armed ||
               manual_active_) accepted=false;
            else {++status_.epoch;copy(status_.owner,request.owner);status_.armed=1;status_.reason=OtDriveReason::armed;status_.deadline_ms=now+200;status_.sequence=0;}
        } else if(request.action==OtDriveAction::command || request.action==OtDriveAction::disarm) {
            if(!status_.armed || request.epoch!=status_.epoch || std::strcmp(status_.owner,request.owner)) accepted=false;
            else if(request.action==OtDriveAction::disarm) release_locked(OtDriveReason::disarmed);
            else if(request.sequence<=status_.sequence || request.deadline_ms<=now || request.deadline_ms>now+200) accepted=false;
            else {status_.sequence=request.sequence;status_.deadline_ms=request.deadline_ms;status_.steering=request.steering;status_.throttle=request.throttle;status_.brake=request.brake;status_.active=1;status_.reason=OtDriveReason::command;}
        }
        result=snapshot_locked(now);return accepted;
    }
    std::array<float,3> frame(uint64_t now,bool source_available,const std::array<float,3>& manual,bool manual_active) {
        std::lock_guard lock(mutex_);expire(now);
        source_available_=source_available;
        manual_active_=manual_active;
        status_.manual_steering=manual[0];status_.manual_throttle=manual[1];status_.manual_brake=manual[2];
        if(status_.armed && !source_available) release_locked(OtDriveReason::input_unavailable);
        if(status_.armed && manual_active_) release_locked(OtDriveReason::manual);
        return status_.active?std::array<float,3>{-status_.steering,status_.throttle,status_.brake}:std::array<float,3>{};
    }
    void release(OtDriveReason reason) {std::lock_guard lock(mutex_);release_locked(reason);}
    void paused(bool value) {std::lock_guard lock(mutex_);paused_=value;if(value) release_locked(OtDriveReason::paused);}
private:
    template<size_t N> static void copy(char(&target)[N],const char* source) {
        const auto count=std::min(std::strlen(source),N-1);std::memcpy(target,source,count);target[count]=0;
    }
    void expire(uint64_t now) {if(status_.armed && now>=status_.deadline_ms) release_locked(OtDriveReason::expired);}
    void release_locked(OtDriveReason reason) {
        status_.armed=status_.active=0;status_.deadline_ms=0;status_.steering=status_.throttle=status_.brake=0;
        status_.reason=reason;status_.owner[0]=0;
    }
    OtDriveStatus snapshot_locked(uint64_t now) const {auto result=status_;result.command_window_ms=status_.armed?now+200:0;return result;}
    std::mutex mutex_;
    OtDriveStatus status_;
    bool source_available_=false,paused_=true,manual_active_=false;
};
// SDK asks for each axis separately. Only first_in_frame replaces this snapshot.
class DriveFrame {
public:
    void begin(std::array<float,3> values) {values_=values;next_=0;}
    bool next(uint32_t& index,float& value) {if(next_==3) return false;index=next_;value=values_[next_++];return true;}
private:
    std::array<float,3> values_{};uint32_t next_=3;
};
}
