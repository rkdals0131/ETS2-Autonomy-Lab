#include "../include/ipc_reader.hpp"
#include <cstring>

// Python validates the mapping ABI before calling these C adapters.
extern "C" __declspec(dllexport) int ot_state_copy(ot::Ring* ring,char* output,
    uint32_t capacity,uint64_t after,uint64_t* sequence) noexcept {
    if(!ring || !output || !sequence || capacity<ot::slot_bytes) return -1;
    try {return ot::read_state(ring,after,*sequence,[&](const char* data,uint32_t length){std::memcpy(output,data,length);});}
    catch(...) {return -1;}
}
extern "C" __declspec(dllexport) int ot_bundle_copy(void* ring,char* output,
    uint32_t capacity,uint64_t after,uint64_t* sequence) noexcept {
    if(!ring || !output || !sequence || !capacity || capacity>ot::bundle_max_bytes) return -1;
    try {return ot::read_bundle(ring,capacity,after,*sequence,[&](const char* data,uint32_t length){std::memcpy(output,data,length);});}
    catch(...) {return -1;}
}
