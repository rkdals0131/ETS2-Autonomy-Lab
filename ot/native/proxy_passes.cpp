#include "proxy_passes.hpp"
#include "camera_replay.hpp"
#include "pool_guard.hpp"
#include <algorithm>
#include <cstdio>

namespace ot {
namespace proxy {
bool accessible(const void* address,size_t size,bool write) {
    constexpr DWORD readable=PAGE_READONLY|PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY;
    // Image data pages nobody has written yet are copy-on-write (2026-10-23: the
    // proxy ring's later pages); writing them is what the proxy itself does.
    constexpr DWORD writable=PAGE_READWRITE|PAGE_EXECUTE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_WRITECOPY;
    auto p=static_cast<const uint8_t*>(address);const auto end=p+size;
    while(p<end) {
        MEMORY_BASIC_INFORMATION info{};
        if(!VirtualQuery(p,&info,sizeof info) || info.State!=MEM_COMMIT || (info.Protect&(PAGE_GUARD|PAGE_NOACCESS)) ||
           !(info.Protect&(write?writable:readable))) return false;
        p=static_cast<const uint8_t*>(info.BaseAddress)+info.RegionSize;
    }
    return true;
}
Environment current_environment() {
    Environment environment;
    const auto proxy_module=GetModuleHandleW(L"dxgi.dll");
    if(!proxy_module) return environment;
    environment.base=reinterpret_cast<uint8_t*>(proxy_module);
    std::wstring path(32768,L'\0');
    const auto length=GetModuleFileNameW(proxy_module,path.data(),static_cast<DWORD>(path.size()));
    if(!length || length>=path.size()) return environment;
    path.resize(length);
    std::error_code error;
    environment.in_game_directory=fs::equivalent(fs::path(path).parent_path(),module_path(nullptr).parent_path(),error) && !error;
    if(!environment.in_game_directory) return environment;
    try {environment.sha256=sha256_file(path);} catch(const std::exception& e) {environment.sha256=std::string("unreadable: ")+e.what();}
    return environment;
}
}
namespace {
namespace rr=proxy::replay;
using L=proxy::Layout;
// dxgi RVAs of the pinned build (multicore.md §3, §6).
constexpr uint32_t scene_handler=0x1A890,leaf_handler=0x1A440,ring_push=0x84460,record_write=0x84670,upload_handler=0x1A8C0;
constexpr uint32_t scene_key=0xC780E8,last_object=0xC73638,last_m11=0xC73640,record_frame=0xCCCCD0;
constexpr uint32_t scene_original=0xC73630,leaf_original=0xC735D8,transform_pointer=0xC73668;
constexpr uint32_t transform_offset=0xC77FF0,game_base_copy=0xC73670,stopped=0xC736B8;
// First bytes of each hooked or called function in the file; .text is not
// rebased, so memory must hold the same bytes.
constexpr std::array<uint8_t,36> scene_bytes={0x48,0x83,0xEC,0x28,0x48,0x8B,0x05,0x95,0x8D,0xC5,0x00,0x48,0x89,0x0D,0x46,0xD8,0xC5,0x00,
    0xFF,0xD0,0x48,0xC7,0x05,0x39,0xD8,0xC5,0x00,0x00,0x00,0x00,0x00,0x48,0x83,0xC4,0x28,0xC3};
constexpr std::array<uint8_t,32> leaf_bytes={0x48,0x8B,0xC4,0x48,0x89,0x58,0x10,0x48,0x89,0x70,0x18,0x55,0x48,0x8D,0x6C,0x24,
    0x80,0x48,0x81,0xEC,0x80,0x01,0x00,0x00,0x0F,0x29,0x70,0xE8,0x48,0x8B,0xDA,0x44};
constexpr std::array<uint8_t,32> push_bytes={0x48,0x89,0x4C,0x24,0x08,0x55,0x57,0x41,0x56,0x48,0x83,0xEC,0x20,0x4C,0x63,0x15,
    0xC8,0x22,0xBF,0x00,0x33,0xC9,0x4C,0x8B,0x1D,0x6B,0x3C,0xBF,0x00,0x4D,0x8B,0xF1};
constexpr std::array<uint8_t,24> record_bytes={0x48,0x8B,0x05,0x71,0x3A,0xBF,0x00,0x33,0xC9,0x48,0x89,0x05,0x08,0x81,0xC4,0x00,
    0x48,0x8B,0x44,0x24,0x28,0x89,0x0D,0xF5};
// ID3D11Device slots the proxy replaced (README §4.3) and its handlers.
struct Slot {unsigned index;uint32_t handler;bool locked;const char* name;};
constexpr std::array<Slot,7> slots={{
    {3,0x16100,false,"CreateBuffer"},{5,0x16710,false,"CreateTexture2D"},{7,0x16200,true,"CreateShaderResourceView"},
    {9,0x164D0,true,"CreateRenderTargetView"},{10,0x16800,true,"CreateDepthStencilView"},
    {12,0x16B10,false,"CreateVertexShader"},{15,0x16F50,false,"CreatePixelShader"}}};
constexpr uintptr_t d3d_device_offset=0x363FD18; // dx11_device_t -> ID3D11Device (d3d_sites.py)
// The proxy's game .text patches (2026-10-22 gate.foreign, docs/28 §11).
constexpr std::array<std::pair<uintptr_t,size_t>,11> game_patches={{{0x2B0A30,5},{0x2B5FE0,5},{0x2BFEF0,5},{0x332F40,5},
    {0x338790,5},{0x3EB300,5},{0x405619,4},{0x40565C,4},{0x405789,4},{0x4208A0,5},{0x1475C90,14}}};
constexpr size_t log_capacity=1024; // ParallelPasses::queue_

using Leaf=uintptr_t(*)(uintptr_t,uintptr_t);
using Scene=uintptr_t(*)(uintptr_t,uintptr_t,uintptr_t,uintptr_t);
using Sink=void(*)(uintptr_t,const void*,const void*,const void*,const void*);
using Transform=void(*)(float*,uintptr_t,uintptr_t);
using Forward=uintptr_t(*)(uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t);
enum Hook : size_t {scene_hook,leaf_hook,push_hook,record_hook};
enum class LogMode : uint8_t {none,executor,shadow};

uint8_t* proxy_base=nullptr;
uintptr_t* device_vtable=nullptr;
std::array<std::atomic<uintptr_t>,4> trampolines{};
std::array<std::atomic<uintptr_t>,slots.size()> slot_originals{};
std::atomic<LogMode> frame_mode{LogMode::none};
thread_local LogMode log_mode=LogMode::none;
thread_local rr::PassLog* log_target=nullptr;
thread_local uintptr_t thread_key=0;
// Parts of split batches (docs/28 §11.18), one set per pool guard job slot.
struct SplitLogs {
    bool active=false;
    uintptr_t key=0;
    std::array<rr::PassLog,PoolGuard::split_parts> parts;
};
std::array<SplitLogs,PoolGuard::split_slots> split_logs;
struct Saved {LogMode mode;rr::PassLog* target;uintptr_t key;};
thread_local std::array<Saved,4> saved_logs;
thread_local size_t saved_depth=0;
std::array<rr::PassLog,log_capacity> logs;
size_t frame_passes=0;
rr::State shadow_start{};
// Shadow frames: the proxy's own 0x84670/0x84460 calls on the main thread.
struct Call {bool record;rr::Camera camera;};
std::vector<Call> actual,planned;
std::atomic<DWORD> capture_thread{0};

struct Local {uint64_t scene=0,leaf=0,keyed=0;};
thread_local Local local;
struct Counters {
    std::atomic<uint64_t> scene{0},leaf_executor{0},leaf_shadow{0},keyed{0},key_without_argument{0},
        replay_frames{0},records{0},pushes{0},dropped{0},max_pass_pushes{0},ring_max{0},
        shadow_frames{0},shadow_mismatch_frames{0},shadow_records{0},shadow_pushes{0};
};
Counters counters;
struct SlotCounters {std::atomic<uint64_t> other{0},locked{0},inside{0},outside{0};};
std::array<SlotCounters,slots.size()> slot_counters;
std::mutex mismatch_mutex;
std::string first_mismatch;

template<class T> T load(uint32_t rva) noexcept {T value;std::memcpy(&value,proxy_base+rva,sizeof value);return value;}
template<class T> void store(uint32_t rva,T value) noexcept {std::memcpy(proxy_base+rva,&value,sizeof value);}
std::string hex(uintptr_t value) {char text[24];std::snprintf(text,sizeof(text),"0x%llX",static_cast<unsigned long long>(value));return text;}
void raise(std::atomic<uint64_t>& maximum,uint64_t value) noexcept {
    for(auto seen=maximum.load();value>seen && !maximum.compare_exchange_weak(seen,value););
}
rr::State proxy_state() noexcept {
    return {load<uintptr_t>(last_object),load<float>(last_m11),load<uint32_t>(record_frame)==load<uint32_t>(L::frame_source)};
}

void fill(rr::Camera& camera,uintptr_t key,uintptr_t object,const float* matrix) noexcept {
    camera.key=key;
    std::memcpy(camera.position.data(),reinterpret_cast<const void*>(object+0x98),sizeof camera.position);
    std::memcpy(camera.view.data(),reinterpret_cast<const void*>(object+0x18),sizeof camera.view);
    rr::relative_view(camera.view.data(),camera.position.data(),camera.relative.data());
    std::memcpy(camera.projection.data(),matrix,sizeof camera.projection);
}
// What 0x1A440 does before its tail jump (multicore.md §5.2.1), into the pass
// log instead of the proxy: block 1 notes the camera, block 2 a ring push.
void observe(rr::PassLog& log,uintptr_t argument,uintptr_t key) noexcept {
    if(!argument) {if(key) counters.key_without_argument.fetch_add(1,std::memory_order_relaxed);return;}
    const auto transform=reinterpret_cast<Transform>(load<uintptr_t>(transform_pointer));
    const auto context=*reinterpret_cast<const uintptr_t*>(argument+0x18);
    const auto extra=(*reinterpret_cast<const uint8_t*>(argument+0x20)&4)?*reinterpret_cast<const uintptr_t*>(context+0x10):0;
    const auto view=*reinterpret_cast<const uintptr_t*>(argument+0x38);
    alignas(16) float m[16]{};
    transform(m,view,extra);
    const auto object=*reinterpret_cast<const uintptr_t*>(context);
    if(object) log.note(object,m[11],[&](rr::Camera& camera) {fill(camera,key,object,m);});
    if(!key) return;
    ++local.keyed;
    alignas(16) float p[16]{};
    transform(p,view,extra);
    if(0.0f<std::fabs(p[11]) && object) {rr::Camera camera;fill(camera,key,object,p);log.add_push(camera);}
}
// dxgi+0x1A440 (game 0x3EB300 through MinHook): executors log with their own
// scene key and jump to the game function as the handler does; the main
// thread of a serial frame logs with the proxy's key and runs the handler.
uintptr_t leaf(uintptr_t target,uintptr_t argument) {
    const auto mode=log_mode;
    if(mode==LogMode::none) return reinterpret_cast<Leaf>(trampolines[leaf_hook].load(std::memory_order_relaxed))(target,argument);
    ++local.leaf;
    if(mode==LogMode::shadow) {
        observe(*log_target,argument,load<uintptr_t>(scene_key));
        return reinterpret_cast<Leaf>(trampolines[leaf_hook].load(std::memory_order_relaxed))(target,argument);
    }
    if(thread_key) log_target->consumed=true;
    observe(*log_target,argument,thread_key);
    thread_key=0;                                         // 0x1A853
    return reinterpret_cast<Leaf>(load<uintptr_t>(leaf_original))(target,argument);
}
// dxgi+0x1A890 (the scene callback stub): the key lives in the thread while
// an executor runs the wrapped call.
uintptr_t scene(uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d) {
    if(log_mode!=LogMode::executor) return reinterpret_cast<Scene>(trampolines[scene_hook].load(std::memory_order_relaxed))(a,b,c,d);
    ++local.scene;
    thread_key=a;                                         // 0x1A89B
    const auto result=reinterpret_cast<Scene>(load<uintptr_t>(scene_original))(a,b,c,d);
    thread_key=0;                                         // 0x1A8A4
    return result;
}
void capture(bool record,const void* position,const void* view,const void* relative,const void* projection) {
    Call call{record,{}};
    call.camera.key=load<uintptr_t>(scene_key);
    std::memcpy(call.camera.position.data(),position,sizeof call.camera.position);
    std::memcpy(call.camera.view.data(),view,sizeof call.camera.view);
    std::memcpy(call.camera.relative.data(),relative,sizeof call.camera.relative);
    std::memcpy(call.camera.projection.data(),projection,sizeof call.camera.projection);
    actual.push_back(call);
}
void push_detour(uintptr_t a,const void* position,const void* view,const void* relative,const void* projection) {
    if(capture_thread.load(std::memory_order_relaxed)==GetCurrentThreadId()) capture(false,position,view,relative,projection);
    reinterpret_cast<Sink>(trampolines[push_hook].load(std::memory_order_relaxed))(a,position,view,relative,projection);
}
void record_detour(uintptr_t a,const void* position,const void* view,const void* relative,const void* projection) {
    if(capture_thread.load(std::memory_order_relaxed)==GetCurrentThreadId()) capture(true,position,view,relative,projection);
    reinterpret_cast<Sink>(trampolines[record_hook].load(std::memory_order_relaxed))(a,position,view,relative,projection);
}
// ID3D11Device slot wrappers. Forwards eight integer arguments (the slots take
// at most five, none floating point).
template<size_t I> uintptr_t slot_detour(uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d,uintptr_t e,uintptr_t f,uintptr_t g,uintptr_t h) {
    const auto original=reinterpret_cast<Forward>(slot_originals[I].load(std::memory_order_relaxed));
    auto& count=slot_counters[I];
    if(!PoolGuard::executor_thread()) {count.other.fetch_add(1,std::memory_order_relaxed);return original(a,b,c,d,e,f,g,h);}
    if(!slots[I].locked) {
        (PoolGuard::holds_pool_lock()?count.inside:count.outside).fetch_add(1,std::memory_order_relaxed);
        return original(a,b,c,d,e,f,g,h);
    }
    PoolGuard::Scope scope;
    (scope.locked()?count.locked:count.inside).fetch_add(1,std::memory_order_relaxed);
    return original(a,b,c,d,e,f,g,h);
}
template<size_t... I> std::array<uintptr_t,sizeof...(I)> slot_detours(std::index_sequence<I...>) {
    return {reinterpret_cast<uintptr_t>(&slot_detour<I>)...};
}
const auto detours=slot_detours(std::make_index_sequence<slots.size()>{});
bool write_slot(uintptr_t* entry,uintptr_t expected,uintptr_t value) noexcept {
    DWORD old{};
    if(!VirtualProtect(entry,sizeof *entry,PAGE_READWRITE,&old)) return false;
    const auto seen=InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(entry),static_cast<LONG64>(value),static_cast<LONG64>(expected));
    DWORD unused{};
    VirtualProtect(entry,sizeof *entry,old,&unused);
    return static_cast<uintptr_t>(seen)==expected;
}
// The address a game patch finally jumps to: E9 rel32 or FF25 [rip+rel] hops.
uintptr_t follow(uintptr_t address,int hops,std::string& path) {
    for(int hop=0;hop<hops;++hop) {
        std::array<uint8_t,6> code{};
        if(!copy_memory(address,code.data(),code.size())) break;
        if(code[0]==0xE9) {int32_t rel{};std::memcpy(&rel,&code[1],4);address=address+5+rel;}
        else if(code[0]==0xFF && code[1]==0x25) {int32_t rel{};std::memcpy(&rel,&code[2],4);if(!read_memory(address+6+rel,address)) break;}
        else break;
        path+=(path.empty()?"":" -> ")+hex(address);
    }
    return address;
}
}

ProxyPasses::~ProxyPasses() {reset();}
bool ProxyPasses::known_patch(uintptr_t rva,size_t size) noexcept {
    return std::find(game_patches.begin(),game_patches.end(),std::pair<uintptr_t,size_t>{rva,size})!=game_patches.end();
}
std::string ProxyPasses::install(uintptr_t game_base,uintptr_t engine_device) {
    if(installed_) return {};
    const auto environment=proxy::current_environment();
    if(auto why=proxy::precheck(environment,proxy::accessible);!why.empty()) return why;
    uint8_t* const dxgi=environment.base;
    json report=json::object();
    // multicore.md §7 item 2: the functions we hook or call.
    const auto same=[&](uint32_t rva,const auto& bytes) {
        return proxy::accessible(dxgi+rva,bytes.size(),false) && !std::memcmp(dxgi+rva,bytes.data(),bytes.size());
    };
    if(!same(scene_handler,scene_bytes)) return "proxy .text at 0x1A890 (scene key handler) differs";
    if(!same(leaf_handler,leaf_bytes)) return "proxy .text at 0x1A440 (composer leaf handler) differs";
    if(!same(ring_push,push_bytes)) return "proxy .text at 0x84460 (ring push) differs";
    if(!same(record_write,record_bytes)) return "proxy .text at 0x84670 (frame record) differs";
    // Item 3: proxy state.
    const auto read=[&](uint32_t rva,auto& value) {
        if(!proxy::accessible(dxgi+rva,sizeof value,false)) return false;
        std::memcpy(&value,dxgi+rva,sizeof value);return true;
    };
    uint32_t stop_value{};int32_t count{};uintptr_t transform{},offset{},image{},leaf_target{},scene_target{};
    if(!read(stopped,stop_value) || stop_value) return "proxy reports a stop (0xC736B8 != 0)";
    if(!read(L::ring_count,count) || count<0 || count>19) return "proxy ring count is outside 0..19";
    for(const auto rva:{scene_key,last_object,last_m11,record_frame})
        if(!proxy::accessible(dxgi+rva,8,true)) return "proxy state at "+hex(rva)+" is not writable";
    if(!read(transform_pointer,transform) || !read(transform_offset,offset) || !read(game_base_copy,image) || !read(leaf_original,leaf_target) ||
       !read(scene_original,scene_target)) return "proxy pointers are not readable";
    if(image!=game_base || transform!=image+offset) return "proxy transform_p pointer is not game base + 0xC77FF0";
    if(!leaf_target || !scene_target) return "proxy originals for 0x3EB300 or the scene callback are null";
    report["transform_p"]=hex(transform-game_base);
    report["leaf_original"]=hex(leaf_target);report["scene_original"]=hex(scene_target);
    // Item 4: the ID3D11Device slots hold the proxy's handlers.
    uintptr_t device{},* vtable{};
    if(!read_memory(engine_device+d3d_device_offset,device) || !device || !read_memory(device,vtable) || !vtable)
        return "ID3D11Device is not available at dx11_device_t+0x363FD18";
    for(const auto& slot:slots) {
        uintptr_t entry{};
        if(!read_memory(reinterpret_cast<uintptr_t>(vtable+slot.index),entry) || entry!=reinterpret_cast<uintptr_t>(dxgi+slot.handler))
            return "ID3D11Device slot "+std::to_string(slot.index)+" is not the proxy handler "+hex(slot.handler);
    }
    // Item 5: the game patches reach the handlers; the scene stub is dumped for §12 item 1.
    std::string path;
    if(follow(game_base+0x3EB300,3,path)!=reinterpret_cast<uintptr_t>(dxgi+leaf_handler)) return "game 0x3EB300 does not reach dxgi+0x1A440 ("+path+")";
    report["leaf_path"]=path;path.clear();
    if(follow(game_base+0x2B5FE0,3,path)!=reinterpret_cast<uintptr_t>(dxgi+upload_handler)) return "game 0x2B5FE0 does not reach dxgi+0x1A8C0 ("+path+")";
    report["upload_path"]=path;path.clear();
    const auto stub=follow(game_base+0x1475C90,1,path);
    report["scene_stub"]=hex(stub);
    std::array<uint8_t,0x400> code{};
    if(copy_memory(stub,code.data(),code.size())) {
        std::string head;
        for(size_t i=0;i<0x60;++i) {char b[4];std::snprintf(b,sizeof(b),"%02X",code[i]);head+=b;}
        report["scene_stub_head"]=head;
        json refs=json::array();
        const auto handler=reinterpret_cast<uintptr_t>(dxgi+scene_handler);
        for(size_t i=0;i+8<=code.size();++i) {uintptr_t value{};std::memcpy(&value,&code[i],8);if(value==handler) refs.push_back(hex(i));}
        for(size_t i=0;i+5<=code.size();++i) if(code[i]==0xE8 || code[i]==0xE9) {
            int32_t rel{};std::memcpy(&rel,&code[i+1],4);
            if(stub+i+5+rel==handler) refs.push_back(std::string(code[i]==0xE8?"call@":"jmp@")+hex(i));
        }
        report["scene_stub_handler_refs"]=std::move(refs);
    }
    // Hooks, disabled.
    std::array<safetyhook::InlineHook,4> hooks;
    const std::array<std::pair<uint32_t,void*>,4> targets={{{scene_handler,reinterpret_cast<void*>(&scene)},{leaf_handler,reinterpret_cast<void*>(&leaf)},
        {ring_push,reinterpret_cast<void*>(&push_detour)},{record_write,reinterpret_cast<void*>(&record_detour)}}};
    for(size_t i=0;i<hooks.size();++i) {
        auto hook=safetyhook::InlineHook::create(dxgi+targets[i].first,targets[i].second,safetyhook::InlineHook::StartDisabled);
        if(!hook) return "SafetyHook could not hook dxgi+"+hex(targets[i].first);
        hooks[i]=std::move(*hook);
    }
    proxy_base=dxgi;device_vtable=vtable;
    for(size_t i=0;i<hooks.size();++i) trampolines[i].store(hooks[i].trampoline().address());
    for(size_t i=0;i<slots.size();++i) slot_originals[i].store(reinterpret_cast<uintptr_t>(dxgi+slots[i].handler));
    hooks_=std::move(hooks);
    actual.reserve(64);planned.reserve(64);
    install_report_=std::move(report);
    installed_=true;
    log("parallel_passes: snowymoon proxy hooks installed (multicore.md plan B), transform_p "+install_report_.value("transform_p",std::string{}));
    return {};
}
bool ProxyPasses::enable() noexcept {
    if(!installed_) return false;
    bool ok=true;
    for(auto& hook:hooks_) if(hook && !hook.enabled()) ok&=static_cast<bool>(hook.enable());
    if(ok && !slots_swapped_) {
        for(size_t i=0;i<slots.size();++i) {
            if(write_slot(device_vtable+slots[i].index,slot_originals[i].load(),detours[i])) continue;
            for(size_t j=0;j<i;++j) write_slot(device_vtable+slots[j].index,detours[j],slot_originals[j].load());
            return false;
        }
        slots_swapped_=true;
    }
    return ok;
}
bool ProxyPasses::disable() noexcept {
    bool ok=true;
    if(slots_swapped_) {
        for(size_t i=0;i<slots.size();++i) ok&=write_slot(device_vtable+slots[i].index,detours[i],slot_originals[i].load());
        slots_swapped_=false;
    }
    frame_mode.store(LogMode::none);capture_thread.store(0);
    for(auto& hook:hooks_) if(hook && hook.enabled()) ok&=static_cast<bool>(hook.disable());
    return ok;
}
bool ProxyPasses::enabled() const noexcept {
    if(slots_swapped_) return true;
    for(const auto& hook:hooks_) if(hook && hook.enabled()) return true;
    return false;
}
void ProxyPasses::reset() noexcept {
    if(slots_swapped_) (void)disable();
    for(auto& hook:hooks_) hook.reset();
    for(auto& trampoline:trampolines) trampoline.store(0);
    installed_=false;
}
CodeRanges ProxyPasses::code() const {
    CodeRanges ranges;
    for(const auto& hook:hooks_) if(hook) ranges.push_back({hook.trampoline().address(),hook.trampoline().address()+hook.trampoline().size()});
    return ranges;
}
bool ProxyPasses::intact() const noexcept {
    if(!slots_swapped_) return false;
    if(load<uint32_t>(stopped)) return false;
    for(size_t i=0;i<slots.size();++i)
        if(device_vtable[slots[i].index]!=detours[i]) return false;
    return true;
}
void ProxyPasses::frame_begin(size_t passes,bool concurrent) noexcept {
    frame_passes=std::min(passes,log_capacity);
    for(size_t i=0;i<frame_passes;++i) logs[i].clear();
    if(concurrent) {frame_mode.store(LogMode::executor);return;}
    shadow_start=proxy_state();
    actual.clear();
    capture_thread.store(GetCurrentThreadId());
    frame_mode.store(LogMode::shadow);
}
void ProxyPasses::pass_begin(size_t index) noexcept {
    const auto mode=frame_mode.load();
    if(mode==LogMode::none || index>=log_capacity) return;
    log_mode=mode;log_target=&logs[index];thread_key=0;
}
void ProxyPasses::pass_end() noexcept {
    if(log_mode==LogMode::none) return;
    (log_mode==LogMode::executor?counters.leaf_executor:counters.leaf_shadow).fetch_add(local.leaf,std::memory_order_relaxed);
    counters.scene.fetch_add(local.scene,std::memory_order_relaxed);
    counters.keyed.fetch_add(local.keyed,std::memory_order_relaxed);
    local={};log_mode=LogMode::none;log_target=nullptr;thread_key=0;
}
namespace {
void split_owner_begin(size_t slot,size_t parts) noexcept {
    auto& split=split_logs[slot];
    split.active=log_mode==LogMode::executor && log_target;
    if(!split.active) return;
    split.key=thread_key;
    for(size_t p=0;p<parts && p<split.parts.size();++p) split.parts[p].clear();
}
void split_part_begin(size_t slot,size_t part) noexcept {
    auto& split=split_logs[slot];
    if(!split.active || saved_depth==saved_logs.size()) return;
    saved_logs[saved_depth++]={log_mode,log_target,thread_key};
    log_mode=LogMode::executor;log_target=&split.parts[part];thread_key=split.key;
}
void split_part_end(size_t slot,size_t) noexcept {
    if(!split_logs[slot].active || !saved_depth) return;
    const auto& previous=saved_logs[--saved_depth];
    log_mode=previous.mode;log_target=previous.target;thread_key=previous.key;
}
// Serially the scene key goes to the first leaf call after it is set: every
// part ran with the owner's key, so a part after the first consumer drops the
// push and the record key it would not have seen.
void split_owner_end(size_t slot,size_t parts) noexcept {
    auto& split=split_logs[slot];
    if(!split.active) return;
    bool consumed=false;
    for(size_t p=0;p<parts && p<split.parts.size();++p) {
        auto& part=split.parts[p];
        if(consumed && part.consumed) {part.pushes=0;part.first.key=0;part.consumed=false;}
        consumed|=part.consumed;
        log_target->append(part);
    }
    if(consumed) thread_key=0;
    split.active=false;
}
constexpr PoolGuard::SplitObserver split_hooks{&split_owner_begin,&split_part_begin,&split_part_end,&split_owner_end};
}
const PoolGuard::SplitObserver* ProxyPasses::split_observer() noexcept {return &split_hooks;}
void ProxyPasses::frame_end() noexcept {
    const auto mode=frame_mode.exchange(LogMode::none);
    if(mode==LogMode::none) return;
    uint64_t dropped=0,most=0;
    for(size_t i=0;i<frame_passes;++i) {dropped+=logs[i].dropped;most=std::max<uint64_t>(most,logs[i].pushes);}
    counters.dropped.fetch_add(dropped,std::memory_order_relaxed);
    raise(counters.max_pass_pushes,most);
    const auto pointers=[](const rr::Camera& c) {
        return std::array<const void*,4>{c.position.data(),c.view.data(),c.relative.data(),c.projection.data()};
    };
    if(mode==LogMode::executor) {
        // §5.2.3: the proxy's own record and push code, in queue order, on the main thread.
        const auto record=reinterpret_cast<Sink>(trampolines[record_hook].load());
        const auto push=reinterpret_cast<Sink>(trampolines[push_hook].load());
        const auto saved=load<uintptr_t>(scene_key);
        const auto frame=load<uint32_t>(L::frame_source);
        uint64_t records=0,pushes=0;
        const auto end=rr::replay(logs.data(),frame_passes,proxy_state(),
            [&](const rr::Camera& camera) {
                const auto p=pointers(camera);
                store(scene_key,camera.key);store(record_frame,frame);
                record(0,p[0],p[1],p[2],p[3]);++records;
            },
            [&](const rr::Camera& camera) {
                const auto p=pointers(camera);
                store(scene_key,camera.key);
                push(0,p[0],p[1],p[2],p[3]);++pushes;
            });
        store(last_object,end.object);store(last_m11,end.m11);store(scene_key,saved);
        counters.replay_frames.fetch_add(1,std::memory_order_relaxed);
        counters.records.fetch_add(records,std::memory_order_relaxed);
        counters.pushes.fetch_add(pushes,std::memory_order_relaxed);
        raise(counters.ring_max,static_cast<uint64_t>(std::max(load<int32_t>(L::ring_count),0)));
        return;
    }
    // Serial frame: what the logs would hand over against what the proxy did.
    capture_thread.store(0);
    planned.clear();
    const auto end=rr::replay(logs.data(),frame_passes,shadow_start,
        [&](const rr::Camera& camera) {planned.push_back({true,camera});},
        [&](const rr::Camera& camera) {planned.push_back({false,camera});});
    // Records and pushes write different proxy state: compare each sequence on its own.
    std::string mismatch;
    for(const bool record:{true,false}) {
        std::vector<const rr::Camera*> a,b;
        for(const auto& call:planned) if(call.record==record) a.push_back(&call.camera);
        for(const auto& call:actual) if(call.record==record) b.push_back(&call.camera);
        (record?counters.shadow_records:counters.shadow_pushes).fetch_add(b.size(),std::memory_order_relaxed);
        if(a.size()!=b.size()) {mismatch=std::string(record?"records":"pushes")+" planned "+std::to_string(a.size())+" actual "+std::to_string(b.size());break;}
        for(size_t i=0;i<a.size() && mismatch.empty();++i)
            if(!rr::same(*a[i],*b[i])) mismatch=std::string(record?"record ":"push ")+std::to_string(i)+(a[i]->key!=b[i]->key?" key":" camera")+" differs";
        if(!mismatch.empty()) break;
    }
    const auto now=proxy_state();
    if(mismatch.empty() && (now.object!=end.object || std::memcmp(&now.m11,&end.m11,sizeof now.m11) || now.won!=end.won))
        mismatch="last-camera state differs";
    counters.shadow_frames.fetch_add(1,std::memory_order_relaxed);
    if(mismatch.empty()) return;
    counters.shadow_mismatch_frames.fetch_add(1,std::memory_order_relaxed);
    std::lock_guard lock(mismatch_mutex);
    if(first_mismatch.empty()) {first_mismatch=mismatch;log("parallel_passes: proxy shadow check: "+mismatch);}
}
json ProxyPasses::status() const {
    json slot_status=json::array();
    for(size_t i=0;i<slots.size();++i) {
        const auto& c=slot_counters[i];
        slot_status.push_back({{"slot",slots[i].index},{"name",slots[i].name},{"locked_slot",slots[i].locked},{"other_threads",c.other.load()},
            {"executor_locked",c.locked.load()},{"executor_inside_pool_lock",c.inside.load()},{"executor_outside_pool_lock",c.outside.load()}});
    }
    std::string mismatch;
    {std::lock_guard lock(mismatch_mutex);mismatch=first_mismatch;}
    return {{"installed",installed_},{"enabled",enabled()},{"slots_swapped",slots_swapped_},{"install",install_report_},
        {"scene_calls",counters.scene.load()},{"leaf_calls_executor",counters.leaf_executor.load()},{"leaf_calls_shadow",counters.leaf_shadow.load()},
        {"leaf_calls_keyed",counters.keyed.load()},{"key_without_argument",counters.key_without_argument.load()},
        {"replay",{{"frames",counters.replay_frames.load()},{"records",counters.records.load()},{"pushes",counters.pushes.load()},
            {"dropped",counters.dropped.load()},{"max_pass_pushes",counters.max_pass_pushes.load()},{"ring_max",counters.ring_max.load()}}},
        {"shadow",{{"frames",counters.shadow_frames.load()},{"mismatch_frames",counters.shadow_mismatch_frames.load()},
            {"records",counters.shadow_records.load()},{"pushes",counters.shadow_pushes.load()},{"first_mismatch",mismatch}}},
        {"device_slots",std::move(slot_status)}};
}
void ProxyPasses::clear_counters() noexcept {
    for(auto* c:{&counters.scene,&counters.leaf_executor,&counters.leaf_shadow,&counters.keyed,&counters.key_without_argument,
                 &counters.replay_frames,&counters.records,&counters.pushes,&counters.dropped,&counters.max_pass_pushes,&counters.ring_max,
                 &counters.shadow_frames,&counters.shadow_mismatch_frames,&counters.shadow_records,&counters.shadow_pushes}) c->store(0);
    for(auto& c:slot_counters) {c.other=0;c.locked=0;c.inside=0;c.outside=0;}
    std::lock_guard lock(mismatch_mutex);
    first_mismatch.clear();
}
}
