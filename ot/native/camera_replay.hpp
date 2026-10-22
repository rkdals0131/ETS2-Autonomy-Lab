#pragma once
// research/snowymoon/multicore.md §5.2.3: what the snowymoon composer leaf
// handler (dxgi+0x1A440) hands the proxy per call, kept per pass so passes
// recorded at the same time can hand it over afterwards in the original
// order. Pure logic; parallel_passes_test.cpp checks it against a serial
// model of the handler.
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace ot::proxy::replay {
// One camera as 0x84670 (frame record) and 0x84460 (ring push) copy it: the
// scene key at the call (dxgi+0xC780E8), the position (object+0x98, 16
// bytes), the view (object+0x18), view times the translation by -position,
// and the matrix from transform_p.
struct Camera {
    uintptr_t key=0;
    std::array<float,4> position{};
    std::array<float,16> view{},relative{},projection{};
};
// The handler's 4x4 loops (0x1A4B3 and 0x1A6F0 on): out = view * T(-position),
// T row-major with the translation in the last column, each element summed
// from 0 in the handler's order so the floats match bit for bit.
inline void relative_view(const float* view,const float* position,float* out) noexcept {
    const float t[16]={1,0,0,-position[0], 0,1,0,-position[1], 0,0,1,-position[2], 0,0,0,1};
    for(int i=0;i<4;++i) {
        float row[4]={0,0,0,0};
        for(int k=0;k<4;++k) {
            const float v=view[i*4+k];
            for(int j=0;j<4;++j) row[j]=row[j]+v*t[k*4+j];
        }
        for(int j=0;j<4;++j) out[i*4+j]=row[j];
    }
}
// The handler's last-camera state (0xC73638 object, 0xC73640 m[11]) and its
// frame gate (0xCCCCD0 == frame number: the frame record is written).
struct State {
    uintptr_t object=0;
    float m11=0;
    bool won=false;
};
// Float != as the handler compares (NaN differs from everything).
inline bool differs(uintptr_t object,float m11,uintptr_t other_object,float other_m11) noexcept {
    return object!=other_object || m11!=other_m11;
}
inline bool candidate(float m11) noexcept {return 2.0f<std::fabs(m11);}

// One pass's calls. Block 1 (frame record) compares each call with the one
// before it; only the first call of a pass compares with what came before the
// pass, so the pass keeps its first call, its first later call that would
// write the record, and the state it leaves. Block 2 (ring push) does not
// depend on other passes and is kept as is.
struct PassLog {
    static constexpr size_t push_capacity=4;
    uint32_t calls=0;                 // block 1 calls with a camera object
    uintptr_t first_object=0,last_object=0;
    float first_m11=0,last_m11=0;
    bool first_candidate=false,has_internal=false,changed_after_first=false;
    Camera first,internal;
    uint32_t pushes=0,dropped=0;
    std::array<Camera,push_capacity> push{};
    void clear() noexcept {calls=0;first_candidate=has_internal=changed_after_first=false;pushes=dropped=0;}
    // Block 1 for a call whose object is not 0. make(camera) fills the camera
    // when this call may write the frame record.
    template<class Make> void note(uintptr_t object,float m11,Make&& make) {
        if(!calls) {
            first_object=last_object=object;first_m11=last_m11=m11;
            first_candidate=candidate(m11);
            if(first_candidate) make(first);
        } else if(differs(last_object,last_m11,object,m11)) {
            if(!has_internal && candidate(m11)) {make(internal);has_internal=true;}
            last_object=object;last_m11=m11;changed_after_first=true;
        }
        ++calls;
    }
    void add_push(const Camera& camera) noexcept {
        if(pushes<push_capacity) push[pushes++]=camera;
        else ++dropped;
    }
};
// Hands the passes' logs over in queue order: record(camera) where the handler
// would have written the frame record, push(camera) for every ring push.
// Returns the state the handler would have after the passes.
template<class Record,class Push>
State replay(const PassLog* logs,size_t count,State state,Record&& record,Push&& push) {
    for(size_t i=0;i<count;++i) {
        const auto& log=logs[i];
        if(log.calls) {
            const bool first_differs=differs(state.object,state.m11,log.first_object,log.first_m11);
            if(!state.won) {
                const Camera* chosen=first_differs && log.first_candidate?&log.first:log.has_internal?&log.internal:nullptr;
                if(chosen) {record(*chosen);state.won=true;}
            }
            // The handler stores only on a difference, so an equal first call keeps the old bits.
            if(log.changed_after_first) {state.object=log.last_object;state.m11=log.last_m11;}
            else if(first_differs) {state.object=log.first_object;state.m11=log.first_m11;}
        }
        for(uint32_t p=0;p<log.pushes;++p) push(log.push[p]);
    }
    return state;
}
inline bool same(const Camera& a,const Camera& b) noexcept {
    return a.key==b.key && !std::memcmp(a.position.data(),b.position.data(),sizeof a.position) &&
        !std::memcmp(a.view.data(),b.view.data(),sizeof a.view) && !std::memcmp(a.relative.data(),b.relative.data(),sizeof a.relative) &&
        !std::memcmp(a.projection.data(),b.projection.data(),sizeof a.projection);
}
}
