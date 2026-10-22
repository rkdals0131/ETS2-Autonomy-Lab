#include "proxy_gate.hpp"
#include <algorithm>
#include <cstring>

namespace ot::proxy {
namespace {
using L=Layout;
bool bytes_equal(const uint8_t* base,uint32_t rva,const std::array<uint8_t,32>& expected) {
    return std::memcmp(base+rva,expected.data(),expected.size())==0;
}
template<class T> T load(const uint8_t* base,uint32_t rva) {T v;std::memcpy(&v,base+rva,sizeof v);return v;}
}
std::string precheck(const Environment& e,const AccessCheck& access) {
    if(!e.base) return "dxgi.dll is not loaded (no proxy; nothing to gate)";
    if(!e.in_game_directory) return "dxgi.dll is not the game bin/win_x64 copy";
    if(e.sha256!=L::sha256) return "dxgi.dll sha256 differs from the analyzed proxy ("+e.sha256+")";
    // Reads first: signatures and state values.
    const auto readable=[&](uint32_t rva,size_t size) {return access(e.base+rva,size,false);};
    if(!readable(L::text_draw_body,32) || !readable(L::text_om_thunk,32)) return "proxy .text is not readable";
    if(!bytes_equal(e.base,L::text_draw_body,L::draw_body_bytes)) return "proxy .text at 0x9FE10 differs";
    if(!bytes_equal(e.base,L::text_om_thunk,L::om_thunk_bytes)) return "proxy .text at 0x17580 differs";
    for(const auto rva:{L::installed,L::version,L::aa_source,L::state_machine,L::frame_source,L::ring_count,
                        L::flag_a_source,L::flag_b_source})
        if(!readable(rva,4)) return "proxy data is not readable";
    if(load<uint32_t>(e.base,L::installed)!=1) return "proxy is not installed (0xC737AC != 1)";
    if(load<uint32_t>(e.base,L::version)!=L::expected_version) return "proxy game version value is not 0x3D";
    if(load<uint32_t>(e.base,L::aa_source)>3) return "proxy AA setting is outside 0..3";
    if(load<int32_t>(e.base,L::state_machine)>4 || load<int32_t>(e.base,L::state_machine)<0) return "proxy draw state is outside 0..4";
    // Writes: every location the gate may write must be a writable data page.
    const auto writable=[&](uint32_t rva,size_t size) {return access(e.base+rva,size,true);};
    if(!writable(L::aa_copy,4) || !writable(L::frame_copy,4) || !writable(L::flag_a,1) || !writable(L::flag_b,1) ||
       !writable(L::ring_skip,(L::ring_capacity-1)*L::ring_stride+1) ||
       !access(e.base+L::ring_keys,(L::ring_capacity-1)*L::ring_stride+8,false))
        return "proxy data is not in writable pages";
    return {};
}
template<class T> T Gate::read(uint32_t rva) const noexcept {return load<T>(base_,rva);}
template<class T> void Gate::write(uint32_t rva,T value) noexcept {std::memcpy(base_+rva,&value,sizeof value);}

bool Gate::arm(const Environment& environment,const AccessCheck& access) {
    stop();restore();
    auto why=precheck(environment,access);
    if(!why.empty()) {reason_=std::move(why);tripped_=false;base_=nullptr;return false;}
    base_=environment.base;reason_.clear();tripped_=false;
    in_sensor_=false;last_pass_=0;last_frame_=0;last_count_=-1;
    for(auto* c:{&counters_.enters,&counters_.exits,&counters_.binds_sensor,&counters_.binds_main,&counters_.lookup_miss,
                 &counters_.ring_flagged,&counters_.ring_invalid,&counters_.ring_max,&counters_.nonidle_entries,&counters_.recopies})
        c->store(0);
    armed_.store(true);
    return true;
}
void Gate::restore() noexcept {
    if(base_ && in_sensor_.exchange(false)) {
        // Section 3.3 item 2: the configured value, not 1.
        write<uint32_t>(L::aa_copy,read<uint32_t>(L::aa_source));
        counters_.exits.fetch_add(1);
    }
}
void Gate::bind(Owner owner,uintptr_t pass,bool miss) noexcept {
    if(!armed_.load(std::memory_order_relaxed)) return;
    if(miss) counters_.lookup_miss.fetch_add(1,std::memory_order_relaxed);
    if(owner==Owner::sensor) {
        counters_.binds_sensor.fetch_add(1,std::memory_order_relaxed);
        enter(pass);
    } else {
        counters_.binds_main.fetch_add(1,std::memory_order_relaxed);
        leave();
    }
}
void Gate::enter(uintptr_t pass) noexcept {
    // A proxy no longer in the analyzed state: put the value back and stop.
    if(read<uint32_t>(L::installed)!=1 || read<uint32_t>(L::aa_source)>3) {
        tripped_.store(true);
        armed_.store(false);
        restore();
        return;
    }
    // Section 3.3 item 1: do the proxy re-copy first so a later draw does not undo the zero.
    const auto frame=read<uint32_t>(L::frame_source);
    if(read<uint32_t>(L::frame_copy)!=frame) {
        write<uint32_t>(L::frame_copy,frame);
        write<uint8_t>(L::flag_a,read<uint8_t>(L::flag_a_source));
        write<uint8_t>(L::flag_b,read<uint8_t>(L::flag_b_source));
        write<uint32_t>(L::aa_copy,read<uint32_t>(L::aa_source));
        counters_.recopies.fetch_add(1,std::memory_order_relaxed);
    }
    if(!in_sensor_.load()) {
        in_sensor_.store(true);
        counters_.enters.fetch_add(1,std::memory_order_relaxed);
        if(read<int32_t>(L::state_machine)!=4) counters_.nonidle_entries.fetch_add(1,std::memory_order_relaxed);
    }
    write<uint32_t>(L::aa_copy,0);
    flag_ring(pass,frame);
}
void Gate::leave() noexcept {
    if(in_sensor_.load()) restore();
}
void Gate::flag_ring(uintptr_t pass,uint32_t frame) noexcept {
    if(!pass) return;
    const auto count=read<int32_t>(L::ring_count);
    if(count<0 || count>static_cast<int32_t>(L::ring_capacity)) {counters_.ring_invalid.fetch_add(1,std::memory_order_relaxed);return;}
    if(pass==last_pass_ && frame==last_frame_ && count==last_count_) return;
    last_pass_=pass;last_frame_=frame;last_count_=count;
    auto previous=counters_.ring_max.load(std::memory_order_relaxed);
    while(previous<static_cast<uint64_t>(count) && !counters_.ring_max.compare_exchange_weak(previous,count,std::memory_order_relaxed)) {}
    for(int32_t i=0;i<count;++i) {
        if(read<uint64_t>(L::ring_keys+i*L::ring_stride)!=pass) continue;
        const auto skip=L::ring_skip+i*L::ring_stride;
        if(read<uint8_t>(skip)==0) {write<uint8_t>(skip,1);counters_.ring_flagged.fetch_add(1,std::memory_order_relaxed);}
    }
}
}
