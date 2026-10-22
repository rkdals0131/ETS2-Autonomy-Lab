#pragma once
#include "ot.hpp"
#include "hook_support.hpp"
#include "proxy_gate.hpp"
#include <array>
#include <string>
#include <safetyhook.hpp>

namespace ot {
namespace proxy {
// The loaded dxgi.dll and whether it is the game-folder copy (with its sha256).
Environment current_environment();
// Committed and readable (writable when write) for the whole range.
bool accessible(const void* address,size_t size,bool write);
}
// research/snowymoon/multicore.md (plan B): the snowymoon proxy (the pinned
// build of proxy_gate.hpp) beside concurrent pass recording.
// - The scene key handler dxgi+0x1A890 and the composer leaf handler
//   dxgi+0x1A440 keep their state per thread on executors and log per pass
//   what the leaf would hand the proxy (camera_replay.hpp); after the
//   executors the join hands it over in queue order through the proxy's own
//   0x84670 (frame record) and 0x84460 (ring push).
// - ID3D11Device slots 7, 9 and 10 (the proxy's view creation, which inserts
//   into its unlocked resource map 0xC76810) take the pool lock on executors;
//   slots 3, 5, 12 and 15 are counted only.
// - In serial frames the main thread logs too while the proxy runs as usual,
//   and the join compares the logged hand-over with the proxy's own calls of
//   0x84670/0x84460 (the S2 check of multicore.md §8).
// Every hook calls straight through on other threads and outside logged passes.
class ProxyPasses {
public:
    ProxyPasses()=default;
    ProxyPasses(const ProxyPasses&)=delete;
    ProxyPasses& operator=(const ProxyPasses&)=delete;
    ~ProxyPasses();
    // Control thread. Empty when every pre-check of multicore.md §7 holds and
    // the hooks exist (disabled); otherwise why, and nothing is installed.
    std::string install(uintptr_t game_base,uintptr_t engine_device);
    bool installed() const noexcept {return installed_;}
    bool enable() noexcept;
    bool disable() noexcept;
    bool enabled() const noexcept;
    // Releases the trampolines; only when no thread can be inside them.
    void reset() noexcept;
    CodeRanges code() const;
    // Main thread, once a frame: the device slots are still ours and the proxy
    // has not stopped (0xC736B8).
    bool intact() const noexcept;
    // Main thread, around the passes of one join. concurrent: executors log;
    // otherwise the main thread logs beside the proxy for the comparison.
    void frame_begin(size_t passes,bool concurrent) noexcept;
    void frame_end() noexcept;
    // The thread about to record pass index of the current join.
    static void pass_begin(size_t index) noexcept;
    static void pass_end() noexcept;
    json status() const;
    void clear_counters() noexcept;
    // A gate.foreign site the proxy patches into the game (11 sites, docs/28 §11).
    static bool known_patch(uintptr_t rva,size_t size) noexcept;
private:
    std::array<safetyhook::InlineHook,4> hooks_;
    bool installed_=false,slots_swapped_=false;
    json install_report_=json::object();
};
}
