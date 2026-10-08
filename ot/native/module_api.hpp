#pragma once
#include <scssdk_telemetry.h>
#include <cstdint>

// C ABI only: no STL objects or ownership of allocations crosses this boundary.
struct OtModuleApi {
    uint32_t abi;
    uint32_t size;
    const char* version; // Valid until the module is released.
    scs_result_t (SCSAPIFUNC* initialize)(scs_u32_t,const scs_telemetry_init_params_t*);
    // Called by the resident loader on the SDK thread, outside module callbacks.
    // 1 means all callbacks/workers/hooks are drained and FreeLibrary is safe.
    // 0 retains the DLL and permits a later stop retry, never a replacement load.
    int (SCSAPIFUNC* stop)();
};
using OtGetModuleApi = const OtModuleApi* (SCSAPIFUNC*)(uint32_t abi);
