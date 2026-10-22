#pragma once
// Render-thread interval setting for the snowymoon proxy (dxgi.dll) next to sensor
// camera renders. Static analysis only (research/snowymoon/sensor_coexistence.md,
// 2026-10-22); not verified in a game session. Off by default. The proxy code is
// never patched: this file only reads and writes the proxy data.
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace ot::proxy {
// Every proxy-internal number lives here. RVAs are relative to the proxy image
// (dxgi.dll, base 0x180000000 in the analysis) of the one pinned build below; any
// other build is rejected by the sha256 before a byte is written.
struct Layout {
    // sensor_coexistence.md section 4.2 item 2.
    static constexpr const char* sha256="d819c16805a9a0ca17f34ef46cc445c5847f32308d10e69bf08f3c1b4779f4dc";
    // Section 3.1 and 3.3 item 1: the draw bodies per-frame re-copy, reproduced in Gate::enter.
    static constexpr uint32_t aa_copy=0xC7382C;      // u32, copy of aa_source (0 = effects off, 1..3 mode)
    static constexpr uint32_t frame_copy=0xC73830;   // u32, frame number of the last re-copy
    static constexpr uint32_t frame_source=0xC288D8; // u32, frame number, incremented by the frame bracket
    static constexpr uint32_t aa_source=0xA3D840;    // u32, configuration value (restore value, 3.3 item 2)
    static constexpr uint32_t flag_a_source=0xC7307F, flag_a=0xC7367D; // u8, copied with the re-copy
    static constexpr uint32_t flag_b_source=0xC730B6, flag_b=0xC7367E; // u8, copied with the re-copy
    // Section 3.3 item 5 and 4.1: the scene-pass ring the render thread consumes in order.
    static constexpr uint32_t ring_keys=0xC73848;    // u64 key per entry (assumed to equal the pass pointer, unverified)
    static constexpr uint32_t ring_skip=0xC7392C;    // u8 skip flag per entry (1 = consumer skips)
    static constexpr uint32_t ring_stride=0xF0;
    static constexpr uint32_t ring_capacity=20;
    static constexpr uint32_t ring_count=0xC7673C;   // i32 entries recorded this frame
    // Section 4.2 item 4: values that must hold.
    static constexpr uint32_t installed=0xC737AC;    // u32 == 1
    static constexpr uint32_t version=0xC7381C;      // u32 == 0x3D (1.61)
    static constexpr uint32_t state_machine=0xB10FE8;// i32 <= 4 (4 = idle)
    static constexpr uint32_t expected_version=0x3D;
    // Section 4.2 item 3: .text bytes at these RVAs (32 bytes each).
    static constexpr uint32_t text_draw_body=0x9FE10, text_om_thunk=0x17580;
    static constexpr std::array<uint8_t,32> draw_body_bytes={
        0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,
        0x41,0x56,0x41,0x57,0x48,0x83,0xEC,0x30,0x8B,0x05,0xAA,0x8A,0xB8,0x00,0x41,0x8B};
    static constexpr std::array<uint8_t,32> om_thunk_bytes={
        0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xEC,0x20,0x48,
        0x8B,0x05,0x8A,0x08,0xC6,0x00,0x49,0x8B,0xF9,0x8B,0xDA,0x49,0x8B,0xF0,0x8B,0xD3};
};

struct Environment {
    uint8_t* base=nullptr;   // loaded proxy image; null when dxgi.dll is not loaded
    bool in_game_directory=false;
    std::string sha256;      // of the file the module was loaded from
};
// (address, size, write): the range is committed and readable (and writable if write).
using AccessCheck=std::function<bool(const void*,size_t,bool)>;
// Empty string when every pre-check of sensor_coexistence.md section 4.2 holds, else the reason.
std::string precheck(const Environment& environment,const AccessCheck& access);

enum class Owner { main,sensor };
class Gate {
public:
    struct Counters {
        std::atomic<uint64_t> enters{0},exits{0},binds_sensor{0},binds_main{0},lookup_miss{0},
            ring_flagged{0},ring_invalid{0},ring_max{0},nonidle_entries{0},recopies{0};
    };
    // Control thread. On failure nothing is written and reason() says why.
    bool arm(const Environment& environment,const AccessCheck& access);
    // Stop accepting binds. The caller must have waited for the render thread to
    // leave bind() before restore().
    void stop() noexcept {armed_.store(false);}
    void restore() noexcept;
    void disarm() noexcept {stop();restore();}
    bool armed() const noexcept {return armed_.load();}
    // Render thread, at every OMSetRenderTargets bind (game 0x2B3203). pass is the
    // sensor pass pointer for the ring match, 0 for main. miss: no named pass found.
    void bind(Owner owner,uintptr_t pass,bool miss) noexcept;
    const Counters& counters() const noexcept {return counters_;}
    std::string reason() const {return tripped_.load()?"proxy state changed during the run; gate stopped":reason_;}
    bool in_sensor() const noexcept {return in_sensor_.load();}
private:
    void enter(uintptr_t pass) noexcept;
    void leave() noexcept;
    void flag_ring(uintptr_t pass,uint32_t frame) noexcept;
    template<class T> T read(uint32_t rva) const noexcept;
    template<class T> void write(uint32_t rva,T value) noexcept;
    std::atomic<bool> armed_{false},in_sensor_{false},tripped_{false};
    uint8_t* base_=nullptr;
    std::string reason_="not enabled";
    // Render thread only: the last ring scan, to scan again only when something changed.
    uintptr_t last_pass_=0;uint32_t last_frame_=0;int32_t last_count_=-1;
    Counters counters_;
};
}
