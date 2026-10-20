#pragma once
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace ot {
// ABI 1. Ownership is exchanged with Windows interlocked operations.
inline constexpr uint32_t ring_slots = 8, slot_bytes = 65536;
struct alignas(64) RingHeader {
    char magic[8];
    uint32_t abi, slots, bytes_per_slot, producer_pid;
    volatile LONG64 published, dropped;
    uint8_t reserved[24];
};
struct alignas(64) Slot {
    volatile LONG state; // 0 free, 1 writing, 2 ready, 3 reading
    uint32_t length;
    uint64_t sequence;
    uint8_t reserved[48];
    char payload[slot_bytes];
};
struct Ring { RingHeader header; Slot slots[ring_slots]; };
static_assert(sizeof(RingHeader)==64 && offsetof(Slot,payload)==64);

// OT_Bundles ABI 1 uses the same ownership states, with variable-sized slots.
// A payload is uint64 JSON byte length, UTF-8 manifest, then packed blobs.
// Unlike OT_State, a full queue drops the new bundle; ready data is retained.
inline constexpr uint32_t bundle_slots=3,bundle_max_bytes=256*1024*1024;
struct alignas(64) BundleSlot {
    volatile LONG state;
    uint32_t length;
    uint64_t sequence;
    uint8_t reserved[48];
};
static_assert(sizeof(BundleSlot)==64);
inline BundleSlot* bundle_slot(void* base,uint32_t capacity,uint32_t index) {
    return reinterpret_cast<BundleSlot*>(static_cast<char*>(base)+sizeof(RingHeader)+
        static_cast<size_t>(index)*(sizeof(BundleSlot)+capacity));
}

// Local\OT_Overlay ABI 1 (docs/24 §4): the planned path for the HUD decal. The
// core creates it; one writer (relay or otpy) and the core's mask worker share
// it. generation is odd while the writer copies, even when complete (seqlock).
inline constexpr uint32_t overlay_abi=1,overlay_max_points=256;
struct alignas(64) OverlayBlock {
    char magic[8];                 // "OTOVL01\0"
    uint32_t abi,point_count;
    volatile LONG64 generation;
    uint64_t stamp_us,ttl_ms;      // SDK paused simulation time of the source; ttl 0 never hides
    float width_m,rgba[4];
    uint8_t valid,reserved0[3];
    double ground_y;               // SCS world Y of the road at the path start; NaN when unknown
    uint8_t reserved[24];
    double points[overlay_max_points][2]; // SCS world X, Z (double: mm at 100 km)
    uint8_t tail[32];                      // explicit padding to the 64-byte alignment
};
static_assert(offsetof(OverlayBlock,generation)==16 && offsetof(OverlayBlock,width_m)==40 &&
              offsetof(OverlayBlock,valid)==60 && offsetof(OverlayBlock,ground_y)==64 && offsetof(OverlayBlock,points)==96 && sizeof(OverlayBlock)==4224);
struct OverlayContent {
    uint32_t point_count=0;
    uint64_t stamp_us=0,ttl_ms=0;
    float width_m=0,rgba[4]{};
    bool valid=false;
    double ground_y=std::numeric_limits<double>::quiet_NaN();
    double points[overlay_max_points][2]{};
};
inline bool overlay_header_valid(const OverlayBlock* block) {
    return block && std::memcmp(block->magic,"OTOVL01",8)==0 && block->abi==overlay_abi;
}
inline void overlay_initialize(OverlayBlock* block) {
    std::memset(block,0,sizeof(OverlayBlock));
    std::memcpy(block->magic,"OTOVL01",8);block->abi=overlay_abi;block->ground_y=std::numeric_limits<double>::quiet_NaN();
}
// Single writer. Interlocked exchanges are full barriers: no field store moves
// outside the odd window.
inline void overlay_write(OverlayBlock* block,const OverlayContent& c) {
    const auto previous=InterlockedCompareExchange64(&block->generation,0,0)&~LONG64{1};
    InterlockedExchange64(&block->generation,previous+1);
    block->point_count=std::min(c.point_count,overlay_max_points);
    block->stamp_us=c.stamp_us;block->ttl_ms=c.ttl_ms;block->width_m=c.width_m;
    std::memcpy(block->rgba,c.rgba,sizeof(c.rgba));block->valid=c.valid?1:0;block->ground_y=c.ground_y;
    std::memcpy(block->points,c.points,sizeof(double)*2*block->point_count);
    InterlockedExchange64(&block->generation,previous+2);
}
// 1: copied a new complete generation; 0: unchanged since `seen`; -1: the
// writer is copying or the copy tore (retry later); -2: invalid content.
inline int overlay_read(OverlayBlock* block,LONG64 seen,OverlayContent& out,LONG64& generation) {
    const auto first=InterlockedCompareExchange64(&block->generation,0,0);
    if(first==seen) return 0;
    if(first&1) return -1;
    out.point_count=block->point_count;out.stamp_us=block->stamp_us;out.ttl_ms=block->ttl_ms;out.width_m=block->width_m;
    std::memcpy(out.rgba,block->rgba,sizeof(out.rgba));out.valid=block->valid!=0;out.ground_y=block->ground_y;
    const auto count=std::min(out.point_count,overlay_max_points);
    std::memcpy(out.points,block->points,sizeof(double)*2*count);
    if(InterlockedCompareExchange64(&block->generation,0,0)!=first) return -1;
    generation=first;
    if(out.point_count>overlay_max_points || !std::isfinite(out.width_m) || out.width_m<0) return -2;
    for(uint32_t i=0;i<out.point_count;++i)
        if(!std::isfinite(out.points[i][0]) || !std::isfinite(out.points[i][1])) return -2;
    return 1;
}
}
