#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>

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
}
