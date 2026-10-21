#include "../include/ipc_reader.hpp"
#include <cstring>
#include <memory>

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
// Local\OT_Overlay writer for otpy (docs/24 §4): one stroke; xz holds count SCS X,Z pairs.
extern "C" __declspec(dllexport) int ot_overlay_write(ot::OverlayBlock* block,const double* xz,uint32_t count,
    uint64_t stamp_us,uint64_t ttl_ms,float width_m,const float* rgba,double ground_y,int valid) noexcept {
    if(!ot::overlay_header_valid(block) || (count && !xz) || !rgba || count>ot::overlay_max_points) return -1;
    auto content=std::make_unique<ot::OverlayContent>();
    content->point_count=count;content->stamp_us=stamp_us;content->ttl_ms=ttl_ms;
    if(count>=2) {
        content->stroke_count=1;auto& k=content->strokes[0];
        k.first=0;k.count=count;k.width_m=width_m;k.min_px=0;std::memcpy(k.rgba,rgba,sizeof(k.rgba));
    }
    content->ground_y=ground_y;content->valid=valid!=0 && count>=2;
    std::memcpy(content->points,xz,sizeof(double)*2*count);
    ot::overlay_write(block,*content);
    return 0;
}
