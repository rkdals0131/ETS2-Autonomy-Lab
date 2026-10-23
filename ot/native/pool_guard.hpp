#pragma once
#include "ot.hpp"
#include "hook_support.hpp"
#include <array>
#include <safetyhook.hpp>

namespace ot {
// docs/28 M3c (§3.4 1-2): what lets pass bodies record at the same time.
// While an executor thread runs its share of a concurrent frame, the device
// pool functions run under one SRW lock (a thread already inside one of them
// calls through: 0x2E50B0 calls 0x2E6B60), and executors other than the main
// thread allocate frame memory from a chunk of their own instead of the
// engine's global one. Every other thread, the render thread included, calls
// straight through, so the engine's own locks never wait on ours.
// M3d-1: 0x2E50B0 (resource_build_bundle) is not under the pool lock as a
// whole. It runs under a lock picked by its item's material object (item+0x10,
// whose parameter provider cache 0x150AF70 rewrites per pass). M3d-2: its
// uniform ring reads go to a per-thread slice of the real segment
// (uniform_slice.hpp), handed in by three mid hooks after the loads of
// [device+0xF1490]; only carving a slice takes the pool lock. Lock order is
// always object lock, then pool lock: nothing that holds the pool lock calls
// 0x2E50B0 or its only caller 0x2E6060. The queue set prepare 0x160E7C0 runs
// under a lock picked by its set (set lock, then pool lock in draw_items).
class PoolGuard {
public:
    PoolGuard()=default;
    PoolGuard(const PoolGuard&)=delete;
    PoolGuard& operator=(const PoolGuard&)=delete;
    ~PoolGuard();
    // Creates the hooks disabled; throws when a site differs from the inspected build.
    void install(uintptr_t base);
    bool installed() const noexcept {return installed_;}
    bool enable() noexcept;
    bool disable() noexcept;
    bool enabled() const noexcept;
    // Releases the trampolines; only when no thread can be inside them.
    void reset() noexcept;
    // Patched sites (for the text gate) and hook code (for quiescence).
    CodeRanges patched() const;
    CodeRanges code() const;
    // Every executor of a concurrent frame, around its share of the loop;
    // arena for executors other than the main thread.
    static void executor_begin(bool arena) noexcept;
    static void executor_end() noexcept;
    // Main thread, once per concurrent frame (counter only).
    static void count_window() noexcept;
    static bool executor_thread() noexcept;
    static bool holds_pool_lock() noexcept;
    // Executors: the pool lock for the scope's lifetime unless this thread
    // holds it already (the proxy's device slots, multicore.md §5.3.1).
    class Scope {
    public:
        Scope() noexcept;
        ~Scope();
        Scope(const Scope&)=delete;
        Scope& operator=(const Scope&)=delete;
        bool locked() const noexcept {return locked_;}
    private:
        bool locked_=false;
        uint64_t entered_=0;
    };
    json status() const;
    void clear_counters() noexcept;
    static constexpr size_t hook_count=13;
private:
    std::array<safetyhook::InlineHook,hook_count> hooks_;
    std::array<safetyhook::MidHook,3> ring_;
    bool installed_=false;
};
}
