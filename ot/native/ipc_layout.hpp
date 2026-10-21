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

// Local\OT_Overlay ABI 2 (docs/24 §4): the HUD decal's marks. The core creates it;
// one writer (relay or otpy) and the core's mask worker share it. generation is odd
// while the writer copies, even when complete (seqlock). Up to 8 strokes, each a
// polyline in the shared point pool with its own width, colour and minimum on-screen
// half thickness (pixels at 1080 lines; 0: none), so marks of different kinds (a lane
// ribbon, a bar across it) never share a width (ABI 1 had one polyline).
inline constexpr uint32_t overlay_abi=2,overlay_max_points=256,overlay_max_strokes=8;
struct OverlayStroke {
    uint32_t first,count;          // points[first, first + count)
    float width_m,min_px;
    float rgba[4];
};
static_assert(sizeof(OverlayStroke)==32);
struct alignas(64) OverlayBlock {
    char magic[8];                 // "OTOVL02\0"
    uint32_t abi,point_count;
    volatile LONG64 generation;
    uint64_t stamp_us,ttl_ms;      // SDK paused simulation time of the source; ttl 0 never hides
    uint32_t stroke_count;
    uint8_t valid,reserved0[3];
    double ground_y;               // SCS world Y of the road at the marks; NaN when unknown
    uint8_t reserved[8];
    OverlayStroke strokes[overlay_max_strokes];
    double points[overlay_max_points][2]; // SCS world X, Z (double: mm at 100 km)
};
static_assert(offsetof(OverlayBlock,generation)==16 && offsetof(OverlayBlock,stroke_count)==40 &&
              offsetof(OverlayBlock,valid)==44 && offsetof(OverlayBlock,ground_y)==48 && offsetof(OverlayBlock,strokes)==64 &&
              offsetof(OverlayBlock,points)==320 && sizeof(OverlayBlock)==4416);
struct OverlayContent {
    uint32_t point_count=0,stroke_count=0;
    OverlayStroke strokes[overlay_max_strokes]{};
    uint64_t stamp_us=0,ttl_ms=0;
    bool valid=false;
    double ground_y=std::numeric_limits<double>::quiet_NaN();
    double points[overlay_max_points][2]{};
};
inline bool overlay_header_valid(const OverlayBlock* block) {
    return block && std::memcmp(block->magic,"OTOVL02",8)==0 && block->abi==overlay_abi;
}
inline void overlay_initialize(OverlayBlock* block) {
    std::memset(block,0,sizeof(OverlayBlock));
    std::memcpy(block->magic,"OTOVL02",8);block->abi=overlay_abi;block->ground_y=std::numeric_limits<double>::quiet_NaN();
}
// Single writer. Interlocked exchanges are full barriers: no field store moves
// outside the odd window.
inline void overlay_write(OverlayBlock* block,const OverlayContent& c) {
    const auto previous=InterlockedCompareExchange64(&block->generation,0,0)&~LONG64{1};
    InterlockedExchange64(&block->generation,previous+1);
    block->point_count=std::min(c.point_count,overlay_max_points);block->stroke_count=std::min(c.stroke_count,overlay_max_strokes);
    block->stamp_us=c.stamp_us;block->ttl_ms=c.ttl_ms;block->valid=c.valid?1:0;block->ground_y=c.ground_y;
    std::memcpy(block->strokes,c.strokes,sizeof(OverlayStroke)*block->stroke_count);
    std::memcpy(block->points,c.points,sizeof(double)*2*block->point_count);
    InterlockedExchange64(&block->generation,previous+2);
}
// 1: copied a new complete generation; 0: unchanged since `seen`; -1: the
// writer is copying or the copy tore (retry later); -2: invalid content.
inline int overlay_read(OverlayBlock* block,LONG64 seen,OverlayContent& out,LONG64& generation) {
    const auto first=InterlockedCompareExchange64(&block->generation,0,0);
    if(first==seen) return 0;
    if(first&1) return -1;
    out.point_count=block->point_count;out.stroke_count=block->stroke_count;out.stamp_us=block->stamp_us;out.ttl_ms=block->ttl_ms;
    out.valid=block->valid!=0;out.ground_y=block->ground_y;
    std::memcpy(out.strokes,block->strokes,sizeof(OverlayStroke)*std::min(out.stroke_count,overlay_max_strokes));
    const auto count=std::min(out.point_count,overlay_max_points);
    std::memcpy(out.points,block->points,sizeof(double)*2*count);
    if(InterlockedCompareExchange64(&block->generation,0,0)!=first) return -1;
    generation=first;
    if(out.point_count>overlay_max_points || out.stroke_count>overlay_max_strokes) return -2;
    for(uint32_t i=0;i<out.stroke_count;++i) {
        const auto& k=out.strokes[i];
        if(k.count<2 || k.first>out.point_count || k.count>out.point_count-k.first || !std::isfinite(k.width_m) || k.width_m<=0 ||
           !std::isfinite(k.min_px) || k.min_px<0 || !std::isfinite(k.rgba[0]) || !std::isfinite(k.rgba[1]) || !std::isfinite(k.rgba[2]) || !std::isfinite(k.rgba[3])) return -2;
    }
    for(uint32_t i=0;i<out.point_count;++i)
        if(!std::isfinite(out.points[i][0]) || !std::isfinite(out.points[i][1])) return -2;
    return 1;
}
}
