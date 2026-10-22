#pragma once
// docs/28 §3.4 1: a per-thread copy of the engine's frame linear allocator
// 0x1E0650 (1.61.1.1 0x1E05B0). The engine bumps one global chunk without
// atomics; a worker recording passes concurrently bumps a chunk of its own,
// taken from the same chunk source (0x166BD0, under the engine's SRW lock) with
// the current frame tag, so the engine returns it with that frame's chunks
// (0x1E07B0 -> 0x166CF0 three frames later).
#include <cstdint>

namespace ot::arena {
constexpr uint64_t chunk_bytes=0x200000;
constexpr uint64_t tag_mark=0xFAFAFAFA00000000ull;
// Chunk record from 0x166BD0 (docs/28 §2.3): +0x10 owner tag, +0x18 data, +0x20 cursor.
struct Chunk {
    uint64_t link[2];
    uint64_t tag;
    uint8_t* data;
    uint64_t cursor;
};
static_assert(sizeof(Chunk)==0x28);
// One per thread. The tag is the one this thread asked for, not the chunk
// header: a returned chunk can be handed out again (to the main thread's
// global chunk too) under a newer tag, and the tag may jump by three when the
// device drains (0x2DE240), so any difference means "take a new chunk".
struct State {
    Chunk* chunk=nullptr;
    uint64_t tag=0;
    uint64_t chunks=0,allocations=0,failures=0;
};
// Same rules as the engine: align the cursor up to `align` (a power of two),
// fit within 2 MB, and on a new chunk start at the cursor rounded up to 16.
template<class GetChunk> void* allocate(State& state,uint64_t size,uint64_t align,uint64_t tag,GetChunk&& get_chunk) {
    if(state.chunk && state.tag==tag) {
        const auto cursor=(state.chunk->cursor+(align-1))&~(align-1);
        if(size<=chunk_bytes-cursor) {
            state.chunk->cursor=cursor+size;++state.allocations;
            return state.chunk->data+cursor;
        }
    }
    auto* chunk=static_cast<Chunk*>(get_chunk(tag));
    if(!chunk) {++state.failures;return nullptr;}
    ++state.chunks;
    state.chunk=chunk;state.tag=tag;
    const auto cursor=(chunk->cursor+0xF)&~uint64_t{0xF};
    if(size<=chunk_bytes-cursor) {
        chunk->cursor=cursor+size;++state.allocations;
        return chunk->data+cursor;
    }
    ++state.failures;
    return nullptr;
}
}
