#pragma once
#include <cstdint>

// Optional resident-loader capability. No C++ ownership crosses DLL boundaries.
enum class OtDriveAction:uint32_t {status,arm,command,disarm};
enum class OtDriveReason:uint32_t {disabled,disarmed,armed,command,expired,manual,paused,panic,disconnected,unloaded,input_unavailable,profile_unsupported};
struct OtDriveRequest {
    OtDriveAction action{};
    const char* owner{};
    uint64_t epoch{},sequence{},deadline_ms{};
    float steering{},throttle{},brake{};
};
struct OtDriveStatus {
    uint32_t available{},permitted{},profile_supported{},armed{},active{};
    OtDriveReason reason=OtDriveReason::disabled;
    uint64_t epoch{},sequence{},deadline_ms{},command_window_ms{};
    float steering{},throttle{},brake{};
    float manual_steering{},manual_throttle{},manual_brake{};
    char owner[129]{},error[256]{};
};
struct OtDriveHost {
    uint32_t abi,size;
    void* context;
    void (*configure)(void*,bool,const char*);
    bool (*request)(void*,const OtDriveRequest*,OtDriveStatus*);
    void (*release)(void*,OtDriveReason);
};
