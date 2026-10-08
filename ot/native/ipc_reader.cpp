#include "ipc_layout.hpp"
#include <cstring>

// Loaded only in the Python client. No game access, threads, or hooks.
// Python validates the mapping ABI once when opening it. All slot metadata
// is read only after acquiring the slot; the producer cannot overwrite it.
extern "C" __declspec(dllexport) int ot_state_copy(ot::Ring* ring, char* output,
    uint32_t capacity, uint64_t after, uint64_t* sequence) noexcept {
    if(!ring || !output || !sequence || capacity<ot::slot_bytes) return -1;
    int bytes=0;
    *sequence=after;
    for(auto& slot:ring->slots) {
        if(InterlockedCompareExchange(&slot.state,3,2)!=2) continue;
        if(slot.length>ot::slot_bytes) {InterlockedExchange(&slot.state,0);return -1;}
        if(slot.sequence>*sequence) {
            std::memcpy(output,slot.payload,slot.length);
            bytes=static_cast<int>(slot.length);
            *sequence=slot.sequence;
        }
        InterlockedExchange(&slot.state,0);
    }
    return bytes;
}
