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
// Auxiliary brakes, owned with ot_drive_auxiliary: retarder step 0..5 (the game's
// semantical retarder0..5 mixes) and the engine brake held (semantical motorbrake).
struct OtDriveAux {
    int32_t retarder{};
    uint32_t engine_brake{};
};
struct OtDriveHost {
    uint32_t abi,size;
    void* context;
    void (*configure)(void*,bool,const char*);
    bool (*request)(void*,const OtDriveRequest*,OtDriveStatus*);
    void (*release)(void*,OtDriveReason);
    // Optional ABI-1 tail. Prefix request retains all-axis ownership for old cores.
    bool (*request_axes)(void*,const OtDriveRequest*,uint32_t,OtDriveStatus*,uint32_t*);
    // Optional ABI-1 tail: request_axes plus the auxiliary brakes (axes up to 7). The
    // aux argument may be null; the last argument receives the commanded aux state.
    bool (*request_aux)(void*,const OtDriveRequest*,uint32_t,const OtDriveAux*,OtDriveStatus*,uint32_t*,OtDriveAux*);
};
inline constexpr uint32_t ot_drive_steering=1,ot_drive_pedals=2,ot_drive_all=3,ot_drive_auxiliary=4,ot_drive_all_aux=7;
inline constexpr int32_t ot_drive_retarder_steps=5;
