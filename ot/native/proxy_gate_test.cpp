// Unit test of the proxy interval gate on a fake proxy memory block. No game, no
// dxgi.dll. Build: cmake --build build --target ot_proxy_gate_test, run dist or build output.
#include "proxy_gate.hpp"
#include <cstdio>
#include <cstring>
#include <vector>

using namespace ot::proxy;
using L=Layout;
namespace {
int failures=0;
#define CHECK(c) do{ if(!(c)){std::printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c);++failures;} }while(0)

struct Image {
    std::vector<uint8_t> memory=std::vector<uint8_t>(0xCD1000,0);
    template<class T> void set(uint32_t rva,T v) {std::memcpy(memory.data()+rva,&v,sizeof v);}
    template<class T> T get(uint32_t rva) const {T v;std::memcpy(&v,memory.data()+rva,sizeof v);return v;}
    Image() {
        std::memcpy(memory.data()+L::text_draw_body,L::draw_body_bytes.data(),32);
        std::memcpy(memory.data()+L::text_om_thunk,L::om_thunk_bytes.data(),32);
        set<uint32_t>(L::installed,1);set<uint32_t>(L::version,L::expected_version);
        set<uint32_t>(L::aa_source,2);set<uint32_t>(L::aa_copy,2);
        set<int32_t>(L::state_machine,4);set<uint32_t>(L::frame_source,10);set<uint32_t>(L::frame_copy,10);
    }
    Environment environment() {return {memory.data(),true,L::sha256};}
    void ring(int index,uint64_t key) {set<uint64_t>(L::ring_keys+index*L::ring_stride,key);}
    bool skip(int index) const {return get<uint8_t>(L::ring_skip+index*L::ring_stride)!=0;}
};
const AccessCheck allow=[](const void*,size_t,bool) {return true;};

void precheck_branches() {
    Image image;
    CHECK(precheck(image.environment(),allow).empty());
    {auto e=image.environment();e.base=nullptr;CHECK(!precheck(e,allow).empty());}
    {auto e=image.environment();e.in_game_directory=false;CHECK(!precheck(e,allow).empty());}
    {auto e=image.environment();e.sha256="d819c168";CHECK(!precheck(e,allow).empty());}
    {auto e=image.environment();e.sha256.clear();CHECK(!precheck(e,allow).empty());}
    for(const auto rva:{L::text_draw_body,L::text_om_thunk,L::text_draw_body+31,L::text_om_thunk+31}) {
        Image bad;bad.memory[rva]^=0xFF;CHECK(!precheck(bad.environment(),allow).empty());
    }
    {Image bad;bad.set<uint32_t>(L::installed,0);CHECK(!precheck(bad.environment(),allow).empty());}
    {Image bad;bad.set<uint32_t>(L::version,0x3C);CHECK(!precheck(bad.environment(),allow).empty());}
    {Image bad;bad.set<uint32_t>(L::aa_source,7);CHECK(!precheck(bad.environment(),allow).empty());}
    {Image ok;ok.set<uint32_t>(L::aa_source,4);ok.set<uint32_t>(L::aa_copy,4);CHECK(precheck(ok.environment(),allow).empty());}
    {Image bad;bad.set<int32_t>(L::state_machine,5);CHECK(!precheck(bad.environment(),allow).empty());}
    {Image ok;ok.set<int32_t>(L::state_machine,0);CHECK(precheck(ok.environment(),allow).empty());}
    // A non-writable page (or unreadable text) refuses.
    CHECK(!precheck(image.environment(),[](const void*,size_t,bool write) {return !write;}).empty());
    CHECK(!precheck(image.environment(),[](const void*,size_t,bool) {return false;}).empty());
    const auto base=image.memory.data();
    CHECK(!precheck(image.environment(),[&](const void* p,size_t,bool write) {
        return !(write && p==base+L::ring_skip);}).empty());
    // arm() writes nothing on a failed pre-check.
    Image bad;bad.memory[L::text_om_thunk]^=1;const auto before=bad.memory;
    Gate gate;CHECK(!gate.arm(bad.environment(),allow));CHECK(!gate.armed());CHECK(!gate.reason().empty());
    CHECK(bad.memory==before);
}
void unarmed_is_inert() {
    Image image;const auto before=image.memory;
    Gate gate;gate.bind(Owner::sensor,0x1000,false);gate.bind(Owner::main,0,true);
    CHECK(image.memory==before);CHECK(gate.counters().binds_sensor==0);
}
void zero_then_restore_configured_value() {
    Image image;Gate gate;CHECK(gate.arm(image.environment(),allow));
    image.set<uint32_t>(L::aa_source,3);image.set<uint32_t>(L::aa_copy,3);
    gate.bind(Owner::sensor,0x1000,false);
    CHECK(image.get<uint32_t>(L::aa_copy)==0);CHECK(gate.in_sensor());
    gate.bind(Owner::sensor,0x1000,false);CHECK(image.get<uint32_t>(L::aa_copy)==0);CHECK(gate.counters().enters==1);
    gate.bind(Owner::main,0,true);
    CHECK(image.get<uint32_t>(L::aa_copy)==3); // the configured mode, not 1
    CHECK(!gate.in_sensor());CHECK(gate.counters().exits==1);CHECK(gate.counters().lookup_miss==1);
    const auto after=image.memory;gate.bind(Owner::main,0,true);CHECK(image.memory==after); // main bind outside a span writes nothing
}
void recopy_first_so_the_proxy_does_not_overwrite() {
    Image image;Gate gate;CHECK(gate.arm(image.environment(),allow));
    // New frame: the proxy would copy on its next draw. The gate does it first.
    image.set<uint32_t>(L::frame_source,11);image.set<uint32_t>(L::frame_copy,10);
    image.set<uint32_t>(L::aa_copy,99);image.set<uint32_t>(L::aa_source,2);
    image.set<uint8_t>(L::flag_a_source,7);image.set<uint8_t>(L::flag_b_source,9);
    gate.bind(Owner::sensor,0x1000,false);
    CHECK(image.get<uint32_t>(L::frame_copy)==11);
    CHECK(image.get<uint8_t>(L::flag_a)==7);CHECK(image.get<uint8_t>(L::flag_b)==9);
    CHECK(image.get<uint32_t>(L::aa_copy)==0);CHECK(gate.counters().recopies==1);
    // A proxy draw now: frame equal, so no re-copy; the zero stays.
    CHECK(image.get<uint32_t>(L::frame_copy)==image.get<uint32_t>(L::frame_source));
    // Frame changes during the span: re-copy, then zero again.
    image.set<uint32_t>(L::frame_source,12);
    gate.bind(Owner::sensor,0x1000,false);
    CHECK(image.get<uint32_t>(L::aa_copy)==0);CHECK(image.get<uint32_t>(L::frame_copy)==12);CHECK(gate.counters().enters==1);
    gate.bind(Owner::main,0,false);CHECK(image.get<uint32_t>(L::aa_copy)==2);
}
void ring_flags() {
    Image image;Gate gate;CHECK(gate.arm(image.environment(),allow));
    image.set<int32_t>(L::ring_count,3);image.ring(0,0xAAAA);image.ring(1,0x1000);image.ring(2,0xBBBB);
    image.ring(3,0x1000); // beyond the count: not an entry of this frame
    gate.bind(Owner::sensor,0x1000,false);
    CHECK(!image.skip(0));CHECK(image.skip(1));CHECK(!image.skip(2));CHECK(!image.skip(3));
    CHECK(gate.counters().ring_flagged==1);CHECK(gate.counters().ring_max==3);
    gate.bind(Owner::sensor,0x1000,false);CHECK(gate.counters().ring_flagged==1); // idempotent
    // The entry appears after the first scan: a new count rescans.
    image.set<int32_t>(L::ring_count,4);
    gate.bind(Owner::sensor,0x1000,false);CHECK(image.skip(3));CHECK(gate.counters().ring_flagged==2);
    // A sensor pass without an entry flags nothing; pass 0 never scans.
    gate.bind(Owner::sensor,0x2000,false);gate.bind(Owner::sensor,0,false);CHECK(gate.counters().ring_flagged==2);
    CHECK(!image.skip(0) && !image.skip(2));
    // Invalid counts are counted and not scanned.
    image.set<int32_t>(L::ring_count,21);gate.bind(Owner::sensor,0x3000,false);
    image.set<int32_t>(L::ring_count,-1);gate.bind(Owner::sensor,0x3001,false);
    CHECK(gate.counters().ring_invalid==2);
    // Capacity edge: the last entry (index 19) is reachable.
    image.set<int32_t>(L::ring_count,20);image.ring(19,0x4000);
    gate.bind(Owner::sensor,0x4000,false);CHECK(image.skip(19));
}
void nonidle_state_counted() {
    Image image;Gate gate;CHECK(gate.arm(image.environment(),allow));
    image.set<int32_t>(L::state_machine,2);gate.bind(Owner::sensor,1,false);
    gate.bind(Owner::main,0,false);
    image.set<int32_t>(L::state_machine,4);gate.bind(Owner::sensor,1,false);
    CHECK(gate.counters().enters==2);CHECK(gate.counters().nonidle_entries==1);
}
void disarm_restores_open_span() {
    Image image;Gate gate;CHECK(gate.arm(image.environment(),allow));
    gate.bind(Owner::sensor,1,false);CHECK(image.get<uint32_t>(L::aa_copy)==0);
    gate.stop();gate.bind(Owner::sensor,1,false);CHECK(gate.counters().binds_sensor==1); // stopped: no more binds
    gate.restore();CHECK(image.get<uint32_t>(L::aa_copy)==2);
    gate.restore();CHECK(gate.counters().exits==1); // second restore does nothing
    // Re-arming resets the counters.
    CHECK(gate.arm(image.environment(),allow));CHECK(gate.counters().binds_sensor==0);
}
void state_change_trips_the_gate() {
    Image image;Gate gate;CHECK(gate.arm(image.environment(),allow));
    gate.bind(Owner::sensor,1,false);CHECK(image.get<uint32_t>(L::aa_copy)==0);
    image.set<uint32_t>(L::installed,0);
    gate.bind(Owner::sensor,1,false);
    CHECK(!gate.armed());CHECK(!gate.in_sensor());CHECK(image.get<uint32_t>(L::aa_copy)==2);CHECK(!gate.reason().empty());
    gate.bind(Owner::sensor,1,false);CHECK(image.get<uint32_t>(L::aa_copy)==2);
}
void trace_reads_only() {
    Image image;Gate gate;CHECK(gate.arm(image.environment(),allow,true));CHECK(gate.tracing());
    image.set<int32_t>(L::ring_count,3);image.set<int32_t>(L::ring_cursor,1);
    for(int i=0;i<3;++i) image.set<float>(L::ring_position+i*L::ring_stride,10.0f*i);
    const uint32_t record=L::records+2*L::record_stride;
    image.set<uint64_t>(L::current_record,reinterpret_cast<uint64_t>(image.memory.data()+record));
    image.set<uint32_t>(record+0x24,1280);image.set<uint32_t>(record+0x28,720);
    const auto before=image.memory;
    gate.bind(Owner::sensor,0x1000,false);gate.bind(Owner::sensor,0x1000,false); // one row: same owner, same frame
    gate.bind(Owner::main,0,true);
    image.set<uint32_t>(L::frame_source,11);image.set<int32_t>(L::ring_cursor,-1);image.set<uint64_t>(L::current_record,0);
    const auto changed=image.memory;
    gate.bind(Owner::main,0,true); // frame change: a row
    gate.bind(Owner::main,0,true); // nothing changed: no row
    image.set<int32_t>(L::ring_cursor,0);gate.bind(Owner::main,0,true); // cursor moved: a row
    CHECK(image.memory==changed);CHECK(!gate.in_sensor());
    std::vector<Gate::TraceRow> rows(Gate::trace_capacity);
    rows.resize(gate.trace_rows(rows.data(),rows.size()));
    CHECK(rows.size()==4);
    if(rows.size()==4) {
        CHECK(rows[0].owner==1 && rows[0].cursor==1 && rows[0].count==3 && rows[0].record==2);
        CHECK(rows[0].width==1280 && rows[0].height==720 && rows[0].position[0]==10.0f && rows[0].last[0]==20.0f);
        CHECK(rows[1].owner==0 && rows[1].binds==2 && rows[1].frame==10);
        CHECK(rows[2].frame==11 && rows[2].cursor==-1 && rows[2].record==-1 && rows[2].position[0]==0.0f);
        CHECK(rows[3].cursor==0 && rows[3].position[0]==0.0f && rows[3].binds==2);
    }
    CHECK(gate.counters().binds_sensor==2 && gate.counters().enters==0);
    (void)before;
}
}

int main() {
    precheck_branches();unarmed_is_inert();zero_then_restore_configured_value();
    recopy_first_so_the_proxy_does_not_overwrite();ring_flags();nonidle_state_counted();
    disarm_restores_open_span();state_change_trips_the_gate();trace_reads_only();
    std::puts(failures?"proxy gate test FAILED":"proxy gate test passed");
    return failures?1:0;
}
