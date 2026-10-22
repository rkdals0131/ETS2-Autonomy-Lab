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
    json status() const;
    void clear_counters() noexcept;
    static constexpr size_t hook_count=10;
private:
    std::array<safetyhook::InlineHook,hook_count> hooks_;
    bool installed_=false;
};
}
