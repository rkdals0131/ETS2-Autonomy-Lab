#pragma once
// docs/28 M3d-2: per-thread slices of the device's uniform ring. 0x2E50B0
// allocates each uniform range from the current segment [device+0xF1490] with
// offset = align256(gpu + cursor) - gpu and switches segments (0x2E6B60) when
// offset + bytes > 2 MB. An executor carves a slice from the real segment under
// the pool lock and hands 0x2E50B0 a view of it shaped like a segment, so the
// engine's own code allocates inside the slice without any lock.
#include <cstdint>

namespace ot::uniform {
constexpr uint64_t segment_bytes=0x200000;
// The fields 0x2E50B0 reads: +0 id, +8 GPU address, +0x10 CPU pointer, +0x18 cursor.
struct Segment {
    uint32_t id;
    uint32_t reserved;
    uintptr_t gpu;
    uintptr_t cpu;
    uint64_t cursor;
};
static_assert(sizeof(Segment)==0x20);
inline uint64_t offset_of(const Segment& segment) noexcept {
    return ((segment.gpu+0xFF+segment.cursor)&~uint64_t{0xFF})-segment.gpu;
}
// The engine's test, inverted: it switches segments when this is false.
inline bool fits(const Segment& segment,uint64_t bytes) noexcept {return offset_of(segment)+bytes<=segment_bytes;}
// Advances the real segment's cursor by `slice` bytes and returns the view: its
// base is shifted back by the 2 MB the engine assumes minus the slice and its
// cursor starts there, so gpu/cpu + offset are the real addresses and fits()
// turns false exactly at the slice end.
inline Segment carve(Segment& real,uint64_t slice) noexcept {
    const auto offset=offset_of(real);
    real.cursor=offset+slice;
    const auto lead=segment_bytes-slice;
    return {real.id,0,real.gpu+offset-lead,real.cpu+offset-lead,lead};
}
}
