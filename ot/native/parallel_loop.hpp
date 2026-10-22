#pragma once
// docs/28 M3b pieces that do not depend on the game: the engine's
// parallel_loop_task_t (§3.3), the ticket order that runs one pass at a time
// in the original order on any thread, and the alternate-mode comparator (§4.2).
#include "pass_stream.hpp"
#include <windows.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace ot::loop {
// Layout of parallel_loop_task_t as 0xC75570 builds it on its stack. The task
// manager links it through +0x08/+0x10, counts executor slots at +0x27 and
// running executors at +0x28, and each executor calls vtable slot 1
// (0x190D10): body(capture, task).
struct EngineTask {
    uintptr_t vtable;
    uintptr_t link[2];
    void* manager;
    uint32_t state;
    uint8_t reserved24;
    uint8_t waiting;
    uint8_t priority;
    uint8_t executors;
    uint8_t running;
    uint8_t reserved29[7];
    const char* name;
    void* capture;
    void (*body)(void* capture,void* task);
    uint8_t reserved48[0x40];
    uint32_t counter;
    uint8_t reserved8c[0x74];
};
static_assert(offsetof(EngineTask,manager)==0x18 && offsetof(EngineTask,state)==0x20 && offsetof(EngineTask,waiting)==0x25);
static_assert(offsetof(EngineTask,executors)==0x27 && offsetof(EngineTask,running)==0x28 && offsetof(EngineTask,name)==0x30);
static_assert(offsetof(EngineTask,capture)==0x38 && offsetof(EngineTask,body)==0x40 && offsetof(EngineTask,counter)==0x88);

// The manager is a table of functions: [0] submit(table, task) wakes
// executors-1 workers, [3] wait(table, task) lets the caller execute a free
// slot and then sleeps until every executor has returned.
using TaskCall=void(*)(void* table,EngineTask* task);
inline void run_parallel_loop(void** table,uintptr_t vtable,uint8_t executors,const char* name,void* capture,void(*body)(void*,void*)) {
    EngineTask task{};
    task.vtable=vtable;task.manager=table;task.priority=1;task.executors=executors?executors:1;
    task.name=name;task.capture=capture;task.body=body;
    reinterpret_cast<TaskCall>(table[0])(table,&task);
    reinterpret_cast<TaskCall>(table[3])(table,&task);
}

// Items are claimed by fetch_add; in ticket order the claimer of item i waits
// until item i-1 finished, so items run one at a time in index order whatever
// thread claimed them. In free order a single lock serializes the items.
struct Loop {
    size_t count=0;
    bool ticket=true;
    std::atomic<size_t> next{0},done{0};
    SRWLOCK lock=SRWLOCK_INIT;
    std::atomic<uint64_t> wait_ticks{0},hold_ticks{0};
};
inline uint64_t ticks() noexcept {LARGE_INTEGER value{};QueryPerformanceCounter(&value);return static_cast<uint64_t>(value.QuadPart);}
template<class Run> void run_items(Loop& loop,Run&& run) {
    for(size_t i;(i=loop.next.fetch_add(1))<loop.count;) {
        const auto start=ticks();
        if(loop.ticket) {
            for(unsigned spin=0;loop.done.load(std::memory_order_acquire)!=i;++spin) {
                if(spin<64) YieldProcessor();
                else SwitchToThread();
            }
        } else AcquireSRWLockExclusive(&loop.lock);
        const auto entered=ticks();
        loop.wait_ticks+=entered-start;
        run(i);
        loop.hold_ticks+=ticks()-entered;
        if(loop.ticket) loop.done.store(i+1,std::memory_order_release);
        else ReleaseSRWLockExclusive(&loop.lock);
    }
}

// §4.2: frames alternate between serial and parallel recording in the pattern
// serial, serial, parallel, parallel (mode_of). Each pass key's body summary is
// compared with the frame two back, which used the other mode (cross), and with
// the frame four back, which used the same mode (baseline); recording on workers
// is suspect only when cross differs more often than baseline. Both lags are even
// because the engine renders some passes differently on odd and even frames
// (vehicle environment maps veh_env_*, mirror draw_geometry: draws 42 vs 127,
// 2026-10-22); with a one-frame pattern that alone made cross 100% and baseline 0.
struct Rates {
    uint64_t compared=0;
    std::array<uint64_t,4> differ{};   // by passes::compare level 1-3 (index 0 unused)
    // Share of comparisons that differ at `level` or a coarser one (1 size, 2 token counts, 3 content).
    double rate(int level=3) const noexcept {
        uint64_t n=0;
        for(int i=1;i<=level;++i) n+=differ[i];
        return compared?static_cast<double>(n)/static_cast<double>(compared):0.0;
    }
};
class Alternate {
public:
    struct Difference {uint64_t key;int level;uint64_t frame;passes::Summary now,before;};
    // Per key: comparisons and structural (level 1-2) differences, cross and baseline.
    struct KeyRates {uint64_t key,cross,cross_structure,baseline,baseline_structure;};
    // 0 serial, 1 parallel. Frames must be consecutive integers.
    static int mode_of(uint64_t frame) noexcept {return static_cast<int>((frame>>1)&1);}
    void observe(uint64_t key,uint64_t frame,const passes::Summary& summary) {
        auto& history=keys_[key];
        const auto two=(frame-2)&3,four=frame&3;
        if(history.valid[two] && history.frame[two]+2==frame) count(cross_,key,frame,summary,history.last[two]);
        if(history.valid[four] && history.frame[four]+4==frame) count(baseline_,key,frame,summary,history.last[four]);
        history.last[four]=summary;history.frame[four]=frame;history.valid[four]=true;
    }
    const Rates& cross() const noexcept {return cross_;}
    const Rates& baseline() const noexcept {return baseline_;}
    const std::optional<Difference>& first_cross() const noexcept {return first_cross_;}
    const std::optional<Difference>& first_structure_cross() const noexcept {return first_structure_cross_;}
    std::vector<KeyRates> key_rates() const {
        std::vector<KeyRates> out;
        for(const auto& [key,h]:keys_) out.push_back({key,h.counts[0],h.counts[1],h.counts[2],h.counts[3]});
        return out;
    }
    // Suspect when cross exceeds both twice the baseline and the baseline plus a
    // margin, judged on all levels, on structure alone (levels 1-2) and on token
    // counts alone. In game the content level differs in ~77% of comparisons even
    // at a standstill (per-frame constants), which would hide a structural
    // difference in the all-levels rate.
    bool suspect(uint64_t minimum=1000) const noexcept {
        if(cross_.compared<minimum || baseline_.compared<minimum) return false;
        const auto exceeds=[](double c,double b,double margin){return c>2*b && c>b+margin;};
        const auto tokens=[](const Rates& r){return r.compared?static_cast<double>(r.differ[2])/static_cast<double>(r.compared):0.0;};
        return exceeds(cross_.rate(),baseline_.rate(),0.01) || exceeds(cross_.rate(2),baseline_.rate(2),0.01)
            || exceeds(tokens(cross_),tokens(baseline_),0.001);
    }
    void clear() {keys_.clear();cross_={};baseline_={};first_cross_.reset();first_structure_cross_.reset();}
private:
    // counts: cross compared, cross structural, baseline compared, baseline structural.
    // Slots by frame & 3, so the frames two and four back are both at hand.
    struct History {passes::Summary last[4];uint64_t frame[4]{};bool valid[4]{};uint64_t counts[4]{};};
    void count(Rates& rates,uint64_t key,uint64_t frame,const passes::Summary& now,const passes::Summary& before) {
        const bool cross=&rates==&cross_;
        auto& counts=keys_[key].counts;
        ++rates.compared;++counts[cross?0:2];
        if(const auto level=passes::compare(now,before)) {
            ++rates.differ[level];
            if(level<=2) ++counts[cross?1:3];
            if(cross && !first_cross_) first_cross_=Difference{key,level,frame,now,before};
            if(cross && level<=2 && !first_structure_cross_) first_structure_cross_=Difference{key,level,frame,now,before};
        }
    }
    std::unordered_map<uint64_t,History> keys_;
    Rates cross_,baseline_;
    std::optional<Difference> first_cross_,first_structure_cross_;
};
}
