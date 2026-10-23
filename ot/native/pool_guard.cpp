#include "pool_guard.hpp"
#include "frame_arena.hpp"
#include "uniform_slice.hpp"
#include <intrin.h>
#include <memory>
#include <vector>
#include <stdexcept>

namespace ot {
namespace {
// 1.61.1.9 sites carried from the 1.61.1.1 ones in docs/28 §2.3/§2.6 with
// research/rebase_rvas.py (all "same": only moves, no changed offsets).
constexpr uintptr_t chunk_get_rva=0x166BD0,frame_tag_rva=0x304E848;
// 40 bytes each: the shorter prologues of segment/view_register/srv_cache/bundle
// match elsewhere in .text (find_target refuses ambiguous signatures).
struct Site {const char* name;uintptr_t rva;std::array<uint8_t,40> signature;};
constexpr std::array<Site,PoolGuard::hook_count> sites={{
    {"frame_allocate 0x1E05B0",0x1E0650,{0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x44,0x8B,0x0D,0xE7,0xE1,0xE6,0x02,0x48,0x8B,0xD9,0x4C,0x8B,0x05,0xED,0x22,0x4A,0x03,0x48,0xB8,0x00,0x00,0x00,0x00,0xFA,0xFA,0xFA,0xFA,0x4C,0x0B,0xC8}},
    {"resource_build_bundle 0x2E5040",0x2E50B0,{0x4C,0x8B,0xDC,0x4D,0x89,0x4B,0x20,0x4D,0x89,0x43,0x18,0x49,0x89,0x53,0x10,0x49,0x89,0x4B,0x08,0x55,0x56,0x49,0x8D,0xAB,0x28,0xFF,0xFF,0xFF,0x48,0x81,0xEC,0xC8,0x01,0x00,0x00,0x4D,0x8B,0x11,0x48,0x8B}},
    {"uniform_segment 0x2E6AF0",0x2E6B60,{0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xEC,0x40,0x48,0x8D,0xB9,0x68,0x14,0x0F,0x00,0x48,0x8B,0xF1,0x48,0x8B,0x57,0x10,0x48,0xFF,0xC2,0x48,0x3B,0x57,0x18,0x76,0x13,0x48,0x8B}},
    {"pool_maintain [0x120] 0x2E8FE0",0x2E9050,{0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0x91,0x38,0x14,0x0F,0x00,0x48,0x8D,0x99,0x40,0x14,0x0F,0x00,0x48,0x8B,0xF9,0x4C,0x8D,0x43,0x20,0x33,0xC0,0x48,0x85,0xDB,0x4C,0x0F,0x44,0xC0}},
    {"buffer_record_acquire 0x2E8090",0x2E8100,{0x41,0x54,0x41,0x56,0x41,0x57,0x48,0x81,0xEC,0x50,0x01,0x00,0x00,0x4C,0x8B,0x35,0x0C,0xA8,0x39,0x03,0x45,0x8B,0xF9,0x45,0x8B,0xD8,0x4C,0x8B,0xE2,0x45,0x8B,0xD0,0x49,0x8D,0x8E,0x20,0x0A,0x00,0x00,0x4C}},
    {"buffer_record_release 0x2E8440",0x2E84B0,{0x40,0x53,0x48,0x83,0xEC,0x20,0x8B,0xC2,0x8B,0xDA,0xBA,0xFF,0xFF,0xFF,0xFF,0x41,0xB8,0xFF,0xFF,0x00,0x00,0x3B,0xC2,0x0F,0x44,0xC2,0x8B,0xD0,0x48,0x8D,0x04,0x80,0x66,0x44,0x01,0x84,0xC1,0xB8,0x14,0x0F}},
    {"srv_cache 0x2B94B0",0x2B9520,{0x40,0x57,0x48,0x83,0xEC,0x40,0xB8,0xFF,0xFF,0x00,0x00,0x45,0x8B,0xD0,0x66,0x3B,0xD0,0x48,0x8B,0xF9,0xB8,0xFF,0xFF,0xFF,0xFF,0x74,0x03,0x0F,0xB7,0xC2,0x48,0x81,0xC1,0x58,0xFE,0x63,0x03,0x44,0x8B,0xC0}},
    {"sampler_cache 0x2BEA70",0x2BEAE0,{0x4C,0x8B,0xDC,0x53,0x55,0x57,0x48,0x81,0xEC,0x80,0x00,0x00,0x00,0x0F,0xB6,0xDA,0x48,0x8D,0xB9,0x30,0xFE,0x63,0x03,0x48,0x8B,0xE9,0x48,0x3B,0x5F,0x10,0x0F,0x83,0x1B,0x02,0x00,0x00,0x48,0x8B,0x47,0x08}},
    {"frame_view_register 0x2AE640",0x2AE6B0,{0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0xB9,0xF0,0x99,0x99,0x03,0x48,0x8B,0xF2,0x48,0x81,0xC7,0x88,0x02,0x00,0x00,0x48,0x8B,0x57,0x10,0x48,0xFF,0xC2,0x48}},
    // Lazily builds a render queue set's queue_data array (+0x228 count grows one by
    // one) the first time a pass of the frame asks; scene/other pass callbacks
    // 0x1473E60, 0x151BB90, 0x160EB80 then index it. Unlocked, a second pass read
    // the array while the first built it (2026-10-22 assert "2 >= 2", docs/28 §11.7).
    // Serialized per set: it only writes the set and calls draw_items (pool lock).
    {"render_queue_set_prepare 0x160C990",0x160E7C0,{0x40,0x55,0x41,0x55,0x48,0x8D,0xAC,0x24,0x38,0xF0,0xFF,0xFF,0xB8,0xC8,0x10,0x00,0x00,0xE8,0x5A,0x26,0x77,0x00,0x48,0x2B,0xE0,0x48,0x83,0xB9,0x28,0x02,0x00,0x00,0x00,0x4C,0x8B,0xE9,0x0F,0x85,0xB3,0x04}},
    // Draw item emission shared by pass callbacks 0x1465260 and 0x166D2F0 (and
    // 0x160C990): fills per-item caches (+0x120 mask, +0x14 shader id) on first
    // use and bumps resource refcounts (+0x68/+0x6C) without atomics.
    {"draw_items 0x2DEC80",0x2DECF0,{0x48,0x89,0x54,0x24,0x10,0x48,0x89,0x4C,0x24,0x08,0x55,0x53,0x57,0x41,0x54,0x48,0x8D,0x6C,0x24,0xF8,0x48,0x81,0xEC,0x08,0x01,0x00,0x00,0x48,0x8B,0x05,0x0E,0x3C,0x3A,0x03,0x49,0x8B,0xF9,0x48,0x89,0x45}},
    // Resource readiness check: non-atomic refcounts, may queue the object on its
    // manager (+0x87A8) or release it (0x23BAE0). Called directly from the scene
    // path (0x160B8F0) and 0x147AA70; a concurrent run crashed in it (§11.7).
    {"resource_ready 0x32E4D0",0x32E540,{0x40,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0xF9,0x48,0x85,0xC9,0x75,0x08,0x32,0xC0,0x48,0x83,0xC4,0x20,0x5F,0xC3,0x8B,0x41,0x44,0xC1,0xE8,0x03,0xA8,0x01,0x74,0x08,0xB0,0x01,0x48,0x83,0xC4,0x20,0x5F,0xC3}},
    // Draw batch: with a sorting queue it quicksorts the caller's r_item_t array
    // in place (0x2E65E0) before reading it. A render queue set's item array is
    // drawn by several passes, so concurrent batches on one array read items
    // half swapped (2026-10-22 21:16: material id 0xFFFF in 0x2E50B0, §11.11).
    {"draw_batch 0x2E5FF0",0x2E6060,{0x48,0x89,0x54,0x24,0x10,0x48,0x89,0x4C,0x24,0x08,0x55,0x53,0x56,0x57,0x41,0x56,0x41,0x57,0x48,0x8D,0x6C,0x24,0xE1,0x48,0x81,0xEC,0xF8,0x00,0x00,0x00,0x48,0x8B,0x5D,0x7F,0x45,0x8B,0xF9,0x4C,0x8B,0x35}},
    // r_cmd_storage_t::allocate(storage, dwords): a part of a split batch
    // allocates in its own buffer instead of the pass storage (§11.18).
    {"cmd_storage_allocate 0x28CEC0",0x28CEC0,{0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9,0x48,0x8B,0xFA,0x48,0x8B,0x89,0x20,0x03,0x00,0x00,0x48,0x85,0xC9,0x74,0x27,0x4C,0x8B,0x41,0x08,0x4D,0x8D,0x0C,0x10,0x49,0x81,0xF9,0x00}},
}};
// The sort in 0x2E6060 (sort == 1): 0x2E60E9 lea rdx,[0x144A80] (compare),
// 0x2E60FE call 0x2E65E0(items + first, count, {&flag, compare}).
constexpr uintptr_t sort_rva=0x2E65E0,compare_rva=0x144A80,sort_lea_rva=0x2E60E9,sort_call_rva=0x2E60FE;

// The instructions right after the three loads of [device+0xF1490] in 0x2E50B0
// (docs/28 §11.10): 0x2E52BA mov rcx (range size at [rax+0x10]), 0x2E52FF
// mov rax (cursor store), 0x2E530E mov rcx (id, GPU and CPU reads). No branch
// lands inside the patched bytes.
constexpr std::array<uintptr_t,3> slice_rvas={0x2E52C1,0x2E5306,0x2E5315};
constexpr std::array<std::array<uint8_t,40>,3> slice_signatures={{
    {0x48,0x8B,0xF0,0x8B,0x58,0x10,0x48,0x85,0xC9,0x74,0x28,0x48,0x8B,0x51,0x08,0x48,0x8B,0x49,0x18,0x4C,0x8D,0x82,0xFF,0x00,0x00,0x00,0x4C,0x03,0xC1,0x49,0x81,0xE0,0x00,0xFF,0xFF,0xFF,0x4C,0x2B,0xC2,0x49},
    {0x49,0x8D,0x0C,0x98,0x48,0x89,0x48,0x18,0x48,0x8B,0x8F,0x90,0x14,0x0F,0x00,0x8B,0x01,0x4C,0x8B,0x79,0x08,0x48,0x8B,0x49,0x10,0x4D,0x03,0xF8,0x49,0x03,0xC8,0x89,0x45,0x20,0x48,0x83,0x7E,0x28,0x00,0x8B},
    {0x8B,0x01,0x4C,0x8B,0x79,0x08,0x48,0x8B,0x49,0x10,0x4D,0x03,0xF8,0x49,0x03,0xC8,0x89,0x45,0x20,0x48,0x83,0x7E,0x28,0x00,0x8B,0x46,0x10,0x48,0x89,0x4D,0xA0,0x48,0x8B,0x8D,0xF0,0x00,0x00,0x00,0x48,0x89},
}};
constexpr uintptr_t segment_offset=0xF1490;
constexpr uint64_t slice_bytes=0x10000;

struct HookCounters {std::atomic<uint64_t> locked{0},nested{0},wait_ticks{0},hold_ticks{0},shared{0};};
std::array<HookCounters,PoolGuard::hook_count> counters;
HookCounters ring_counters,scope_counters;
std::array<SRWLOCK,64> object_locks{},array_locks{},set_locks{}; // zero is SRWLOCK_INIT
std::array<std::atomic<uintptr_t>,PoolGuard::hook_count> originals{};
std::atomic<uintptr_t> base{0};
SRWLOCK pool_lock=SRWLOCK_INIT,material_lock=SRWLOCK_INIT;
std::atomic<uint64_t> arena_chunks{0},arena_allocations{0},arena_failures{0},windows{0};
thread_local int depth=0;
thread_local bool executor=false,arena_thread=false;
// Set while this thread holds an object or array lock: a nested entry calls
// through instead of taking a second SRW lock it may already hold (2026-10-22
// 21:45 hang: one executor waited on its own object lock, §11.12).
thread_local bool in_bundle=false,in_batch=false,in_prepare=false,in_material=false;
thread_local uniform::Segment slice{};
thread_local bool slice_valid=false,slice_now=false;
thread_local arena::State arena_state;
thread_local bool tallying=false;
thread_local PoolGuard::BatchTally tally{};
thread_local std::array<uintptr_t,64> tally_arrays{};

// Short sections (draw_items holds ~4 us a call): spin a little before
// sleeping, so a waiter does not pay a kernel wake for every hand-over.
void acquire_spinning(SRWLOCK& lock) noexcept {
    for(int i=0;i<4000;++i) {
        if(TryAcquireSRWLockExclusive(&lock)) return;
        _mm_pause();
    }
    AcquireSRWLockExclusive(&lock);
}
using Allocate=uintptr_t(*)(uint64_t,uint64_t);
using ChunkGet=void*(*)(uint64_t);
uintptr_t frame_allocate(uint64_t size,uint64_t align) {
    if(!arena_thread) return reinterpret_cast<Allocate>(originals[0].load(std::memory_order_relaxed))(size,align);
    const auto b=base.load(std::memory_order_relaxed);
    const uint64_t tag=*reinterpret_cast<const volatile uint32_t*>(b+frame_tag_rva)|arena::tag_mark;
    const auto get=reinterpret_cast<ChunkGet>(b+chunk_get_rva);
    return reinterpret_cast<uintptr_t>(arena::allocate(arena_state,size,align,tag,get));
}
// Forwards eight integer arguments: the hooked functions take at most eight
// (0x2E8090) and no floating-point ones (Ghidra prototypes); a function with
// fewer simply ignores the extra registers and caller stack slots.
using Pooled=uintptr_t(*)(uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t);
template<size_t I> uintptr_t pooled(uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d,uintptr_t e,uintptr_t f,uintptr_t g,uintptr_t h) {
    const auto original=reinterpret_cast<Pooled>(originals[I].load(std::memory_order_relaxed));
    if(!executor) return original(a,b,c,d,e,f,g,h);
    auto& count=counters[I];
    if(depth) {count.nested.fetch_add(1,std::memory_order_relaxed);return original(a,b,c,d,e,f,g,h);}
    const auto start=qpc_now();
    AcquireSRWLockExclusive(&pool_lock);
    const auto entered=qpc_now();
    ++depth;
    const auto result=original(a,b,c,d,e,f,g,h);
    --depth;
    const auto left=qpc_now();
    ReleaseSRWLockExclusive(&pool_lock);
    count.locked.fetch_add(1,std::memory_order_relaxed);
    count.wait_ticks.fetch_add(entered-start,std::memory_order_relaxed);
    count.hold_ticks.fetch_add(left-entered,std::memory_order_relaxed);
    return result;
}
// 0x2E50B0(device, out, context, item): one item's uniform ranges and resource
// bindings. Serialized per material object; the ring section inside takes the
// pool lock (ring_enter/ring_leave).
uintptr_t bundle(uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d,uintptr_t e,uintptr_t f,uintptr_t g,uintptr_t h) {
    const auto original=reinterpret_cast<Pooled>(originals[1].load(std::memory_order_relaxed));
    if(!executor || depth) return original(a,b,c,d,e,f,g,h);
    if(in_bundle) {counters[1].nested.fetch_add(1,std::memory_order_relaxed);return original(a,b,c,d,e,f,g,h);}
    const auto object=*reinterpret_cast<const uintptr_t*>(d+0x10);
    auto& lock=object_locks[((object>>6)^(object>>16))&(object_locks.size()-1)];
    auto& count=counters[1];
    const auto start=qpc_now();
    AcquireSRWLockExclusive(&lock);
    const auto entered=qpc_now();
    in_bundle=true;
    const auto result=original(a,b,c,d,e,f,g,h);
    in_bundle=false;
    const auto left=qpc_now();
    ReleaseSRWLockExclusive(&lock);
    count.locked.fetch_add(1,std::memory_order_relaxed);
    count.wait_ticks.fetch_add(entered-start,std::memory_order_relaxed);
    count.hold_ticks.fetch_add(left-entered,std::memory_order_relaxed);
    return result;
}
// After the first load: make room in this thread's slice for the range
// (size [rax+0x10] dwords), carving a new slice under the pool lock when it
// does not fit, so the engine never switches segments for an executor.
// 0x2E6060(.., .., items, sort, first, count): one batch of a render item
// array. Locked per array (locks picked by its address); different arrays
// run at the same time. Only sort == 1 writes the array (0x2E65E0 in place);
// the rest of the batch reads it, so batches that do not sort share the lock
// (2026-10-23, docs/28 §11.17). Lock order: array lock, set lock (prepare), object lock
// (bundle) or material lock (draw_items), pool lock.
// §11.18: one part's commands, in chunks so returned pointers stay valid.
struct Output {
    struct Chunk {std::unique_ptr<uint32_t[]> data;size_t capacity=0,used=0;};
    std::vector<Chunk> chunks;
    size_t active=0;
    void reset() noexcept {for(auto& chunk:chunks) chunk.used=0;active=0;}
    uint32_t* take(size_t dwords) {
        while(active<chunks.size() && chunks[active].used+dwords>chunks[active].capacity) ++active;
        if(active==chunks.size()) {
            const auto capacity=std::max<size_t>(dwords,size_t{1}<<15);
            chunks.push_back({std::make_unique<uint32_t[]>(capacity),capacity,0});
        }
        auto& chunk=chunks[active];
        auto* p=chunk.data.get()+chunk.used;chunk.used+=dwords;
        return p;
    }
};
struct Job {
    std::atomic<int> state{0};               // 0 free, 1 building, 2 open, 3 closing
    std::atomic<uint64_t> next{uint64_t{1}<<30},done{0};
    uintptr_t a=0,b=0,c=0,g=0,h=0,storage=0;
    uint64_t first=0,count=0,parts=0;
    std::array<Output,PoolGuard::split_parts> outputs;
};
std::array<Job,PoolGuard::split_slots> jobs;
std::atomic<bool> split_enabled{false},split_available{false};
std::atomic<const PoolGuard::SplitObserver*> split_observer{nullptr};
std::atomic<uint64_t> split_batches{0},split_owner_parts{0},split_helper_parts{0},split_no_slot{0},split_dwords{0},split_wait_ticks{0};
thread_local uintptr_t redirect_storage=0;
thread_local Output* redirect_output=nullptr;
thread_local bool in_part=false;

// 0x28CEC0(storage, dwords): the pass storage of a part being recorded goes
// to the part's buffer.
uintptr_t cmd_allocate(uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d,uintptr_t e,uintptr_t f,uintptr_t g,uintptr_t h) {
    if(a==redirect_storage && redirect_output) return reinterpret_cast<uintptr_t>(redirect_output->take(b));
    return reinterpret_cast<Pooled>(originals[13].load(std::memory_order_relaxed))(a,b,c,d,e,f,g,h);
}
// Records items [first + count*p/parts, first + count*(p+1)/parts) of the job
// with the original batch (no sort, no lock: the owner holds the array lock).
void run_part(Job& job,size_t slot,uint64_t part,bool owner) {
    const auto from=job.first+job.count*part/job.parts,to=job.first+job.count*(part+1)/job.parts;
    auto& output=job.outputs[part];
    output.reset();
    const auto saved_storage=redirect_storage;
    auto* const saved_output=redirect_output;
    const bool saved_batch=in_batch,saved_part=in_part;
    redirect_storage=job.storage;redirect_output=&output;in_batch=true;in_part=true;
    const auto* observer=split_observer.load(std::memory_order_acquire);
    if(observer) observer->part_begin(slot,part);
    reinterpret_cast<Pooled>(originals[12].load(std::memory_order_relaxed))(job.a,job.b,job.c,0,from,to-from,job.g,job.h);
    if(observer) observer->part_end(slot,part);
    redirect_storage=saved_storage;redirect_output=saved_output;in_batch=saved_batch;in_part=saved_part;
    (owner?split_owner_parts:split_helper_parts).fetch_add(1,std::memory_order_relaxed);
    job.done.fetch_add(1,std::memory_order_release);
}
bool help_one() noexcept {
    for(size_t i=0;i<jobs.size();++i) {
        auto& job=jobs[i];
        if(job.state.load(std::memory_order_acquire)!=2) continue;
        const auto part=job.next.fetch_add(1,std::memory_order_acq_rel);
        if(part<job.parts) {run_part(job,i,part,false);return true;}
    }
    return false;
}
// The owner's draw batch (array lock held, executor of a concurrent frame):
// sorts as 0x2E6060 would, opens a job, records parts with any idle executor,
// then appends the parts' commands to the pass storage in item order.
uintptr_t split_batch(uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d,uintptr_t e,uintptr_t f,uintptr_t g,uintptr_t h) {
    const auto image=base.load(std::memory_order_relaxed);
    const auto original=reinterpret_cast<Pooled>(originals[12].load(std::memory_order_relaxed));
    if(e>=*reinterpret_cast<const uint64_t*>(c+0x10)) return original(a,b,c,d,e,f,g,h); // the engine's own bounds assert
    if(static_cast<int>(d)==1) {
        uint64_t flag=a&~uint64_t{0xFF};
        struct {uint64_t* flag;uintptr_t compare;} context{&flag,image+compare_rva};
        reinterpret_cast<void(*)(uintptr_t,uint64_t,void*)>(image+sort_rva)(*reinterpret_cast<const uintptr_t*>(c+8)+e*0x18,f,&context);
    }
    size_t slot=jobs.size();
    for(size_t i=0;i<jobs.size();++i) {
        int free=0;
        if(jobs[i].state.compare_exchange_strong(free,1,std::memory_order_acq_rel)) {slot=i;break;}
    }
    if(slot==jobs.size()) {split_no_slot.fetch_add(1,std::memory_order_relaxed);return original(a,b,c,0,e,f,g,h);}
    auto& job=jobs[slot];
    job.a=a;job.b=b;job.c=c;job.g=g;job.h=h;job.first=e;job.count=f;
    job.parts=std::min<uint64_t>(PoolGuard::split_parts,f/PoolGuard::part_items);
    job.storage=*reinterpret_cast<const uintptr_t*>(b+0xF0);
    job.done.store(0,std::memory_order_relaxed);
    const auto* observer=split_observer.load(std::memory_order_acquire);
    if(observer) observer->owner_begin(slot,job.parts);
    job.next.store(0,std::memory_order_release);
    job.state.store(2,std::memory_order_release);
    for(;;) {
        const auto part=job.next.fetch_add(1,std::memory_order_acq_rel);
        if(part>=job.parts) break;
        run_part(job,slot,part,true);
    }
    const auto waited=qpc_now();
    while(job.done.load(std::memory_order_acquire)<job.parts) if(!help_one()) YieldProcessor();
    split_wait_ticks.fetch_add(qpc_now()-waited,std::memory_order_relaxed);
    job.state.store(3,std::memory_order_release);
    job.next.store(uint64_t{1}<<30,std::memory_order_release);
    const auto allocate=reinterpret_cast<Pooled>(originals[13].load(std::memory_order_relaxed));
    uint64_t dwords=0;
    for(uint64_t part=0;part<job.parts;++part) {
        const auto& output=job.outputs[part];
        for(size_t i=0;i<=output.active && i<output.chunks.size();++i) {
            const auto& chunk=output.chunks[i];
            if(!chunk.used) continue;
            auto* destination=reinterpret_cast<uint32_t*>(allocate(job.storage,chunk.used,0,0,0,0,0,0));
            std::memcpy(destination,chunk.data.get(),chunk.used*sizeof(uint32_t));
            dwords+=chunk.used;
        }
    }
    if(observer) observer->owner_end(slot,job.parts);
    job.state.store(0,std::memory_order_release);
    split_batches.fetch_add(1,std::memory_order_relaxed);
    split_dwords.fetch_add(dwords,std::memory_order_relaxed);
    return 0;
}
uintptr_t locked_batch(uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d,uintptr_t e,uintptr_t f,uintptr_t g,uintptr_t h);
uintptr_t draw_batch(uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d,uintptr_t e,uintptr_t f,uintptr_t g,uintptr_t h) {
    if(!tallying || in_batch) return locked_batch(a,b,c,d,e,f,g,h);
    const auto start=qpc_now();
    tallying=false;
    const auto result=locked_batch(a,b,c,d,e,f,g,h);
    tallying=true;
    ++tally.calls;tally.ticks+=qpc_now()-start;tally.items+=f;tally.sorts+=static_cast<int>(d)==1;
    bool seen=false;
    for(uint64_t i=0;i<tally.arrays && i<tally_arrays.size();++i) seen|=tally_arrays[i]==c;
    if(!seen) {if(tally.arrays<tally_arrays.size()) tally_arrays[tally.arrays]=c;++tally.arrays;}
    return result;
}
uintptr_t locked_batch(uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d,uintptr_t e,uintptr_t f,uintptr_t g,uintptr_t h) {
    const auto original=reinterpret_cast<Pooled>(originals[12].load(std::memory_order_relaxed));
    if(!executor || depth) return original(a,b,c,d,e,f,g,h);
    if(in_batch) {counters[12].nested.fetch_add(1,std::memory_order_relaxed);return original(a,b,c,d,e,f,g,h);}
    auto& lock=array_locks[((c>>4)^(c>>12))&(array_locks.size()-1)];
    auto& count=counters[12];
    const bool sorts=static_cast<int>(d)==1;
    const auto start=qpc_now();
    if(sorts) AcquireSRWLockExclusive(&lock); else AcquireSRWLockShared(&lock);
    const auto entered=qpc_now();
    in_batch=true;
    const bool split=f>=PoolGuard::split_items && !in_part && split_enabled.load(std::memory_order_relaxed) &&
        split_available.load(std::memory_order_relaxed);
    const auto result=split?split_batch(a,b,c,d,e,f,g,h):original(a,b,c,d,e,f,g,h);
    in_batch=false;
    const auto left=qpc_now();
    if(sorts) ReleaseSRWLockExclusive(&lock); else ReleaseSRWLockShared(&lock);
    count.locked.fetch_add(1,std::memory_order_relaxed);
    if(!sorts) count.shared.fetch_add(1,std::memory_order_relaxed);
    count.wait_ticks.fetch_add(entered-start,std::memory_order_relaxed);
    count.hold_ticks.fetch_add(left-entered,std::memory_order_relaxed);
    return result;
}
// 0x160E7C0(set): builds the set's queue_data array once a frame. Serialized
// per set (locks picked by its address) instead of under the pool lock, which
// it held 1.3 ms a frame while [0x120] and the ring slices waited 4+ ms
// (2026-10-23, docs/28 §11.16). Inside it only draw_items (0x2DECF0) touches
// shared state, and that takes the pool lock: set lock, then pool lock.
// It was never called under the pool lock (nested 0); if it is, it calls
// through rather than take a set lock after the pool lock.
uintptr_t queue_prepare(uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d,uintptr_t e,uintptr_t f,uintptr_t g,uintptr_t h) {
    const auto original=reinterpret_cast<Pooled>(originals[9].load(std::memory_order_relaxed));
    if(!executor) return original(a,b,c,d,e,f,g,h);
    if(depth || in_prepare) {counters[9].nested.fetch_add(1,std::memory_order_relaxed);return original(a,b,c,d,e,f,g,h);}
    auto& lock=set_locks[((a>>4)^(a>>12))&(set_locks.size()-1)];
    auto& count=counters[9];
    const auto start=qpc_now();
    AcquireSRWLockExclusive(&lock);
    const auto entered=qpc_now();
    in_prepare=true;
    const auto result=original(a,b,c,d,e,f,g,h);
    in_prepare=false;
    const auto left=qpc_now();
    ReleaseSRWLockExclusive(&lock);
    count.locked.fetch_add(1,std::memory_order_relaxed);
    count.wait_ticks.fetch_add(entered-start,std::memory_order_relaxed);
    count.hold_ticks.fetch_add(left-entered,std::memory_order_relaxed);
    return result;
}
// draw_items 0x2DECF0 and resource_ready 0x32E540 (only called from it in
// practice) change material resource refcounts (+0x68/+0x6C), the resource
// manager lists (+0x87A8, release 0x23BAE0) and the material registry
// (0x235980/0x2DBD20). None of the other pool functions touch those, so they
// run under a lock of their own: draw_items held the pool lock 1.1 ms a frame
// while [0x120], the ring slices and the device slots waited behind it
// (2026-10-23, docs/28 §11.16). Order: set lock, material lock, pool lock.
template<size_t I> uintptr_t material(uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d,uintptr_t e,uintptr_t f,uintptr_t g,uintptr_t h) {
    const auto original=reinterpret_cast<Pooled>(originals[I].load(std::memory_order_relaxed));
    if(!executor) return original(a,b,c,d,e,f,g,h);
    auto& count=counters[I];
    // Under the pool lock (not seen) the order would invert: call through.
    if(depth || in_material) {count.nested.fetch_add(1,std::memory_order_relaxed);return original(a,b,c,d,e,f,g,h);}
    const auto start=qpc_now();
    acquire_spinning(material_lock);
    const auto entered=qpc_now();
    in_material=true;
    const auto result=original(a,b,c,d,e,f,g,h);
    in_material=false;
    const auto left=qpc_now();
    ReleaseSRWLockExclusive(&material_lock);
    count.locked.fetch_add(1,std::memory_order_relaxed);
    count.wait_ticks.fetch_add(entered-start,std::memory_order_relaxed);
    count.hold_ticks.fetch_add(left-entered,std::memory_order_relaxed);
    return result;
}
// 0x2B9520 (1.61.1.1 0x2B94B0): its hit test only reads the image object, and a
// miss calls 0x2B8AC0, which only reads it too and creates the view with the
// free-threaded D3D11 device (1.61.1.9 decompile), then 0x2AE6B0 registers the
// view under its own pool lock hook. So it needs no lock of its own; the hook
// stays to count calls (nested = unlocked calls).
uintptr_t srv_cache(uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d,uintptr_t e,uintptr_t f,uintptr_t g,uintptr_t h) {
    if(executor) counters[6].nested.fetch_add(1,std::memory_order_relaxed);
    return reinterpret_cast<Pooled>(originals[6].load(std::memory_order_relaxed))(a,b,c,d,e,f,g,h);
}
// 0x2BEAE0 (1.61.1.1 0x2BEA70): a hit is a non-null pointer in
// [device+0x363FE38][id]; a miss creates the sampler and stores it there. The
// pointer store is one aligned write, so executors read hits without the lock.
uintptr_t sampler_cache(uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d,uintptr_t e,uintptr_t f,uintptr_t g,uintptr_t h) {
    if(executor && !depth) {
        const auto id=b&0xFF;
        if(id<*reinterpret_cast<const volatile uint64_t*>(a+0x363FE40)) {
            const auto sampler=*reinterpret_cast<const volatile uintptr_t*>(*reinterpret_cast<const uintptr_t*>(a+0x363FE38)+id*8);
            if(sampler) {counters[7].nested.fetch_add(1,std::memory_order_relaxed);return sampler;}
        }
    }
    return pooled<7>(a,b,c,d,e,f,g,h);
}
void slice_first(safetyhook::Context& context) noexcept {
    if(!executor) return;
    const uint64_t bytes=uint64_t{*reinterpret_cast<const uint32_t*>(context.rax+0x10)}*4;
    if(!slice_valid || !uniform::fits(slice,bytes)) {
        const auto want=std::min(std::max(slice_bytes,((bytes+0xFF)&~uint64_t{0xFF})+0x100),uniform::segment_bytes);
        const auto device=context.rdi;
        const auto start=qpc_now();
        AcquireSRWLockExclusive(&pool_lock);
        const auto entered=qpc_now();
        ++depth;
        auto* real=*reinterpret_cast<uniform::Segment**>(device+segment_offset);
        if(!real || !uniform::fits(*real,want)) {
            reinterpret_cast<void(*)(uintptr_t)>(originals[2].load(std::memory_order_relaxed))(device);  // 0x2E6B60
            real=*reinterpret_cast<uniform::Segment**>(device+segment_offset);
        }
        slice=uniform::carve(*real,want);slice_valid=true;
        --depth;
        const auto left=qpc_now();
        ReleaseSRWLockExclusive(&pool_lock);
        ring_counters.locked.fetch_add(1,std::memory_order_relaxed);
        ring_counters.wait_ticks.fetch_add(entered-start,std::memory_order_relaxed);
        ring_counters.hold_ticks.fetch_add(left-entered,std::memory_order_relaxed);
    }
    ring_counters.nested.fetch_add(1,std::memory_order_relaxed);
    context.rcx=reinterpret_cast<uintptr_t>(&slice);slice_now=true;
}
void slice_store(safetyhook::Context& context) noexcept {
    if(slice_now) context.rax=reinterpret_cast<uintptr_t>(&slice);
}
void slice_read(safetyhook::Context& context) noexcept {
    if(!slice_now) return;
    context.rcx=reinterpret_cast<uintptr_t>(&slice);slice_now=false;
}
template<size_t... I> constexpr std::array<void*,sizeof...(I)> pooled_detours(std::index_sequence<I...>) {
    return {reinterpret_cast<void*>(&pooled<I>)...};
}
}

PoolGuard::~PoolGuard() {reset();}
void PoolGuard::install(uintptr_t image) {
    if(installed_) return;
    base.store(image);
    std::array<safetyhook::InlineHook,hook_count> hooks;
    const auto detours=pooled_detours(std::make_index_sequence<hook_count>{});
    for(size_t i=0;i<hook_count;++i) {
        const auto target=find_target(sites[i].signature,0,sites[i].rva);
        void* detour=i==0?reinterpret_cast<void*>(&frame_allocate):i==1?reinterpret_cast<void*>(&bundle):
                     i==6?reinterpret_cast<void*>(&srv_cache):i==7?reinterpret_cast<void*>(&sampler_cache):
                     i==9?reinterpret_cast<void*>(&queue_prepare):i==10?reinterpret_cast<void*>(&material<10>):
                     i==11?reinterpret_cast<void*>(&material<11>):i==12?reinterpret_cast<void*>(&draw_batch):
                     i==13?reinterpret_cast<void*>(&cmd_allocate):detours[i];
        auto hook=safetyhook::InlineHook::create(target,detour,safetyhook::InlineHook::StartDisabled);
        if(!hook) throw std::runtime_error(std::string("SafetyHook could not hook ")+sites[i].name);
        hooks[i]=std::move(*hook);
        originals[i].store(hooks[i].trampoline().address());
    }
    constexpr std::array<safetyhook::MidHookFn,3> slice_hooks={&slice_first,&slice_store,&slice_read};
    std::array<safetyhook::MidHook,3> ring;
    for(size_t i=0;i<ring.size();++i) {
        auto hook=safetyhook::MidHook::create(find_target(slice_signatures[i],0,slice_rvas[i]),slice_hooks[i],safetyhook::MidHook::StartDisabled);
        if(!hook) throw std::runtime_error("SafetyHook could not hook the uniform ring load "+std::to_string(i));
        ring[i]=std::move(*hook);
    }
    ring_=std::move(ring);
    hooks_=std::move(hooks);
    // Splitting re-does the batch's sort; only when the sort call is the inspected one.
    int32_t lea{},call{};
    std::memcpy(&lea,reinterpret_cast<const void*>(image+sort_lea_rva+3),4);
    std::memcpy(&call,reinterpret_cast<const void*>(image+sort_call_rva+1),4);
    split_available.store(image+sort_lea_rva+7+lea==image+compare_rva && image+sort_call_rva+5+call==image+sort_rva);
    installed_=true;
}
bool PoolGuard::enable() noexcept {
    bool ok=true;
    // Later loads before the first: the first sets slice_now for the other two.
    for(size_t i=ring_.size();i-->0;) if(ring_[i] && !ring_[i].enabled()) ok&=static_cast<bool>(ring_[i].enable());
    for(auto& hook:hooks_) if(hook && !hook.enabled()) ok&=static_cast<bool>(hook.enable());
    return ok;
}
bool PoolGuard::disable() noexcept {
    bool ok=true;
    for(auto& hook:hooks_) if(hook && hook.enabled()) ok&=static_cast<bool>(hook.disable());
    for(auto& hook:ring_) if(hook && hook.enabled()) ok&=static_cast<bool>(hook.disable());
    return ok;
}
bool PoolGuard::enabled() const noexcept {
    for(const auto& hook:hooks_) if(hook && hook.enabled()) return true;
    for(const auto& hook:ring_) if(hook && hook.enabled()) return true;
    return false;
}
void PoolGuard::reset() noexcept {
    for(auto& hook:hooks_) hook.reset();
    for(auto& hook:ring_) hook.reset();
    for(auto& original:originals) original.store(0);
    installed_=false;
}
CodeRanges PoolGuard::patched() const {
    CodeRanges ranges;
    for(const auto& hook:hooks_) if(hook) ranges.push_back({hook.target_address(),hook.target_address()+hook.original_bytes().size()});
    for(const auto& hook:ring_) if(hook) ranges.push_back({hook.target_address(),hook.target_address()+hook.original_bytes().size()});
    return ranges;
}
CodeRanges PoolGuard::code() const {
    CodeRanges ranges;
    for(const auto& hook:hooks_)
        if(hook) ranges.push_back({hook.trampoline().address(),hook.trampoline().address()+hook.trampoline().size()});
    for(const auto& hook:ring_) if(hook) {
        ranges.push_back({hook.stub().address(),hook.stub().address()+hook.stub().size()});
        ranges.push_back({hook.trampoline().address(),hook.trampoline().address()+hook.trampoline().size()});
    }
    return ranges;
}
void PoolGuard::count_window() noexcept {windows.fetch_add(1,std::memory_order_relaxed);}
bool PoolGuard::executor_thread() noexcept {return executor;}
void PoolGuard::set_split(bool enabled,const SplitObserver* observer) noexcept {
    split_observer.store(observer,std::memory_order_release);
    split_enabled.store(enabled,std::memory_order_release);
}
bool PoolGuard::help_once() noexcept {return executor && help_one();}
void PoolGuard::tally_begin() noexcept {tally={};tallying=true;}
PoolGuard::BatchTally PoolGuard::tally_end() noexcept {tallying=false;return tally;}
bool PoolGuard::holds_pool_lock() noexcept {return depth>0;}
PoolGuard::Scope::Scope() noexcept {
    if(!executor || depth) {if(executor) scope_counters.nested.fetch_add(1,std::memory_order_relaxed);return;}
    const auto start=qpc_now();
    AcquireSRWLockExclusive(&pool_lock);
    entered_=qpc_now();
    ++depth;locked_=true;
    scope_counters.wait_ticks.fetch_add(entered_-start,std::memory_order_relaxed);
}
PoolGuard::Scope::~Scope() {
    if(!locked_) return;
    --depth;
    const auto left=qpc_now();
    ReleaseSRWLockExclusive(&pool_lock);
    scope_counters.locked.fetch_add(1,std::memory_order_relaxed);
    scope_counters.hold_ticks.fetch_add(left-entered_,std::memory_order_relaxed);
}
// A slice lives one frame: the segment it came from may be retired after it.
void PoolGuard::executor_begin(bool arena) noexcept {executor=true;arena_thread=arena;slice_valid=false;slice_now=false;}
void PoolGuard::executor_end() noexcept {
    executor=false;arena_thread=false;in_bundle=in_batch=in_prepare=in_material=false;
    arena_chunks.fetch_add(arena_state.chunks,std::memory_order_relaxed);
    arena_allocations.fetch_add(arena_state.allocations,std::memory_order_relaxed);
    arena_failures.fetch_add(arena_state.failures,std::memory_order_relaxed);
    arena_state.chunks=arena_state.allocations=arena_state.failures=0;
}
json PoolGuard::status() const {
    const auto freq=static_cast<double>(qpc_frequency());
    json functions=json::array();
    uint64_t wait=0,hold=0;
    const auto entry=[&](const char* name,const HookCounters& c,const char* lock) {
        return json{{"name",name},{"lock",lock},{"locked",c.locked.load()},{"nested",c.nested.load()},{"shared",c.shared.load()},
            {"wait_ms",c.wait_ticks.load()*1000.0/freq},{"hold_ms",c.hold_ticks.load()*1000.0/freq}};
    };
    // lock_wait_ms/lock_hold_ms are the pool lock only; 0x2E50B0 and 9-12 report their own locks.
    for(size_t i=1;i<hook_count;++i) {
        const auto& c=counters[i];
        if(i!=1 && i<9) {wait+=c.wait_ticks.load();hold+=c.hold_ticks.load();}
        functions.push_back(entry(sites[i].name,c,i==1?"object":i==9?"set":i==10 || i==11?"material":i==12?"array":"pool"));
    }
    wait+=ring_counters.wait_ticks.load();hold+=ring_counters.hold_ticks.load();
    // locked = slices carved under the pool lock, nested = uniform ranges served from slices.
    functions.push_back(entry("uniform_ring_slices 0x2E52BA",ring_counters,"pool"));
    wait+=scope_counters.wait_ticks.load();hold+=scope_counters.hold_ticks.load();
    functions.push_back(entry("proxy_device_slots 7/9/10",scope_counters,"pool"));
    json split={{"available",split_available.load()},{"enabled",split_enabled.load()},{"batches",split_batches.load()},
        {"owner_parts",split_owner_parts.load()},{"helper_parts",split_helper_parts.load()},{"no_slot",split_no_slot.load()},
        {"dwords",split_dwords.load()},{"owner_wait_ms",split_wait_ticks.load()*1000.0/freq}};
    return {{"installed",installed_},{"enabled",enabled()},{"windows",windows.load()},{"split",std::move(split)},
        {"lock_wait_ms",wait*1000.0/freq},{"lock_hold_ms",hold*1000.0/freq},{"functions",std::move(functions)},
        {"arena",{{"chunks",arena_chunks.load()},{"allocations",arena_allocations.load()},{"failures",arena_failures.load()}}}};
}
void PoolGuard::clear_counters() noexcept {
    for(auto& c:counters) {c.locked=0;c.nested=0;c.wait_ticks=0;c.hold_ticks=0;c.shared=0;}
    for(auto* c:{&ring_counters,&scope_counters}) {c->locked=0;c->nested=0;c->wait_ticks=0;c->hold_ticks=0;}
    arena_chunks=0;arena_allocations=0;arena_failures=0;windows=0;
    for(auto* c:{&split_batches,&split_owner_parts,&split_helper_parts,&split_no_slot,&split_dwords,&split_wait_ticks}) c->store(0);
}
}
