#include "parallel_passes.hpp"
#include "hook_support.hpp"
#include <algorithm>
#include <stdexcept>
#include <Zydis.h>

namespace ot {
namespace {
std::atomic<ParallelPasses*> instance{};
std::atomic<uint32_t> callbacks{};
std::atomic<uint64_t> non_main_calls{};
// 1.61.1.1 sites (docs/28 §2-§3). The game hash gate pins these; the
// signatures confirm the inspected bytes before any hook is created.
constexpr uintptr_t execute_rva=0x226960,join_rva=0x227B1F,allocate_rva=0x28CE60;
constexpr uintptr_t device_pointer_rva=0x3681860,device_vtable_rva=0x22127A8,storage_vtable_rva=0x21FD700;
// 0x226960 prologue: mov [rsp+8],rbx; mov [rsp+18h],rsi; mov [rsp+10h],rdx; push rbp/rdi/r14; lea rbp,[rsp-80h]
constexpr std::array<uint8_t,24> execute_signature={0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x74,0x24,0x18,0x48,0x89,
    0x54,0x24,0x10,0x55,0x57,0x41,0x56,0x48,0x8D,0x6C,0x24,0x80};
// LAB_140227b1f, the only exit of the pass loop: mov [rbp+898h],r15 then the tail storage setup.
constexpr std::array<uint8_t,21> join_signature={0x4C,0x89,0xBD,0x98,0x08,0x00,0x00,0x48,0x8D,0x85,0xA8,0x08,0x00,0x00,
    0x48,0x89,0x85,0x90,0x08,0x00,0x00};
constexpr std::array<uint8_t,16> allocate_prologue={0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9,0x48,0x8B,0xFA};
constexpr std::array<std::pair<uintptr_t,uintptr_t>,3> device_slots={{{0x108,0x2B1A30},{0x120,0x2E8FE0},{0x278,0x2B6B20}}};
constexpr uint64_t join_limit_ms=50,overflow_limit_frames=100;

uint64_t fnv(std::string_view text,uint64_t hash=1469598103934665603ull) noexcept {
    for(const auto c:text) {hash^=static_cast<uint8_t>(c);hash*=1099511628211ull;}
    return hash;
}
std::string engine_string(uintptr_t address) {
    std::array<char,96> buffer{};
    if(!address || !copy_memory(address,buffer.data(),buffer.size()-1)) return {};
    return std::string(buffer.data(),strnlen(buffer.data(),buffer.size()-1));
}
uint64_t ms_ticks(uint64_t ms) noexcept {return qpc_frequency()*ms/1000;}
bool relocates_call(const std::vector<uint8_t>& bytes) {
    ZydisDecoder decoder{};
    ZydisDecoderInit(&decoder,ZYDIS_MACHINE_MODE_LONG_64,ZYDIS_STACK_WIDTH_64);
    for(size_t offset=0;offset<bytes.size();) {
        ZydisDecodedInstruction instruction{};
        if(!ZYAN_SUCCESS(ZydisDecoderDecodeInstruction(&decoder,nullptr,bytes.data()+offset,bytes.size()-offset,&instruction)) ||
           instruction.meta.category==ZYDIS_CATEGORY_CALL) return true;
        offset+=instruction.length;
    }
    return false;
}
// A dxgi.dll loaded from the game folder is a proxy (snowymoon). Reported, not refused:
// M3a records on the main thread in the original order (docs/28 §3.4 5 concerns workers).
bool game_folder_dxgi() {
    const auto dxgi=GetModuleHandleW(L"dxgi.dll");
    if(!dxgi) return false;
    return module_path(dxgi).parent_path()==module_path(nullptr).parent_path();
}
const char* mode_name(int mode) {return mode==1?"defer":"off";}
}

ParallelPasses::~ParallelPasses() {
    // Runtime calls close() before destruction; a failed close retains the object.
    execute_hook_.reset();join_hook_.reset();
    if(module_reference_) FreeLibrary(module_reference_);
}
bool ParallelPasses::device_matches() const noexcept {
    uintptr_t device{},vtable{};
    return read_memory(base_+device_pointer_rva,device) && device==device_ && read_memory(device,vtable) && vtable==device_vtable_;
}
void ParallelPasses::enable(Mode mode) {
    std::lock_guard lock(control_);
    if(mode==Mode::off) throw std::runtime_error("Use disable for off");
    if(GetModuleHandleW(L"renderdoc.dll"))
        throw std::runtime_error("RenderDoc is loaded; restart ETS2 normally before enabling parallel_passes");
    base_=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    uintptr_t device{},vtable{};
    if(!read_memory(base_+device_pointer_rva,device) || !device || !read_memory(device,vtable))
        throw std::runtime_error("Render device is not available yet");
    if(vtable!=base_+device_vtable_rva) throw std::runtime_error("Render device is not the inspected DX11 device (-rdevice dx11 only)");
    for(const auto& [slot,rva]:device_slots) {
        uintptr_t entry{};
        if(!read_memory(vtable+slot,entry) || entry!=base_+rva) throw std::runtime_error("DX11 device vtable differs from docs/28 §2.4");
    }
    if(std::memcmp(reinterpret_cast<const void*>(base_+allocate_rva),allocate_prologue.data(),allocate_prologue.size()))
        throw std::runtime_error("r_cmd_storage_t::allocate differs from the inspected build");
    if(!execute_hook_) {
        if(instance.load() && instance.load()!=this)
            throw std::runtime_error("A previous parallel_passes instance is retained; restart ETS2");
        const auto execute_target=find_target(execute_signature,0,execute_rva);
        const auto join_target=find_target(join_signature,0,join_rva);
        if(!module_reference_) {
            if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(&join_callback),&module_reference_))
                throw std::runtime_error("Cannot retain ot_core while parallel_passes hooks are installed");
            module_begin_=reinterpret_cast<uintptr_t>(module_reference_);
            auto* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(module_begin_);
            auto* nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(module_begin_+dos->e_lfanew);
            module_end_=module_begin_+nt->OptionalHeader.SizeOfImage;
        }
        auto execute=safetyhook::InlineHook::create(execute_target,&execute_detour,safetyhook::InlineHook::StartDisabled);
        if(!execute) throw std::runtime_error("SafetyHook could not create the pass execution detour");
        auto join=safetyhook::MidHook::create(join_target,&join_callback,safetyhook::MidHook::StartDisabled);
        if(!join) {execute->reset();throw std::runtime_error("SafetyHook could not create the pass loop join hook");}
        // A suspended thread must not have a return address in a trampoline
        // without unwind metadata; the relocated prologues contain no CALL.
        if(relocates_call(execute->original_bytes()) || relocates_call(join->original_bytes())) {
            execute->reset();join->reset();
            throw std::runtime_error("parallel_passes detour cannot relocate an external CALL");
        }
        execute_hook_=std::move(*execute);
        join_hook_=std::move(*join);
        original_.store(execute_hook_.original<Execute>());
        allocate_=reinterpret_cast<Allocate>(base_+allocate_rva);
        instance.store(this);
    }
    device_=device;device_vtable_=vtable;
    if(requested_.load()==0) {
        std::lock_guard stats(stats_mutex_);
        counters_={};keys_.clear();
        non_main_calls=0;
    }
    reason_.clear();error_.clear();
    // The join must be live before any pass can be deferred.
    if(!join_hook_.enabled() && !join_hook_.enable()) throw std::runtime_error("Cannot enable the pass loop join hook");
    if(!execute_hook_.enabled() && !execute_hook_.enable()) {
        (void)join_hook_.disable();
        throw std::runtime_error("Cannot enable the pass execution detour");
    }
    requested_.store(static_cast<int>(mode));
    log(std::string("parallel_passes enabled: ")+mode_name(static_cast<int>(mode))+(game_folder_dxgi()?" (game-folder dxgi.dll present)":""));
}
void ParallelPasses::disable() noexcept {
    try {
        std::lock_guard lock(control_);
        requested_.store(0);
        if(!execute_hook_ && !join_hook_) return;
        // latch() publishes frame_active_ before reading requested_, so after
        // this wait no frame holds deferred passes and later frames latch off.
        const auto deadline=GetTickCount64()+2000;
        while(frame_active_.load() && GetTickCount64()<deadline) Sleep(1);
        if(frame_active_.load()) {
            error_="A deferring frame did not reach its join; hooks stay installed in off mode";
            log("parallel_passes: "+error_);
            return;
        }
        bool changed=false;
        if(execute_hook_.enabled()) {changed=true;if(!execute_hook_.disable()) error_="Pass execution detour disable failed";}
        if(join_hook_.enabled()) {changed=true;if(!join_hook_.disable()) error_="Join hook disable failed";}
        while(callbacks.load() && GetTickCount64()<deadline+2000) Sleep(1);
        if(changed) log("parallel_passes disabled"+(error_.empty()?std::string{}:": "+error_));
    } catch(...) {requested_.store(0);}
}
bool ParallelPasses::quiescent() noexcept {
    if(callbacks.load() || frame_active_.load()) return false;
    CodeRanges ranges;
    ranges.push_back({module_begin_,module_end_});
    ranges.push_back({execute_hook_.trampoline().address(),execute_hook_.trampoline().address()+execute_hook_.trampoline().size()});
    ranges.push_back({join_hook_.stub().address(),join_hook_.stub().address()+join_hook_.stub().size()});
    ranges.push_back({join_hook_.trampoline().address(),join_hook_.trampoline().address()+join_hook_.trampoline().size()});
    return threads_outside(ranges) && callbacks.load()==0;
}
bool ParallelPasses::close() noexcept {
    disable();
    if(!execute_hook_ && !join_hook_) return true;
    if(execute_hook_.enabled() || join_hook_.enabled()) return false;
    const auto deadline=GetTickCount64()+2000;
    do {
        if(quiescent()) {
            instance.store(nullptr);
            original_.store(nullptr);
            execute_hook_.reset();join_hook_.reset();
            log("parallel_passes drained; trampolines released");
            return true;
        }
        Sleep(1);
    } while(GetTickCount64()<deadline);
    log("parallel_passes retained after drain timeout; restart ETS2 before DLL replacement");
    return false;
}
void ParallelPasses::turn_off(const std::string& reason) noexcept {
    requested_.store(0);
    try {
        std::lock_guard stats(stats_mutex_);
        if(reason_.empty()) reason_=reason;
    } catch(...) {}
    log("parallel_passes off: "+reason);
}

void ParallelPasses::execute_detour(uintptr_t builder,uintptr_t value,uintptr_t pass,void* marker) {
    callbacks.fetch_add(1);
    auto* self=instance.load();
    const auto original=self?self->original_.load():nullptr;
    if(self && original && self->defer(builder,value,pass,marker)) {callbacks.fetch_sub(1);return;}
    callbacks.fetch_sub(1);
    if(original) original(builder,value,pass,marker);
}
void ParallelPasses::join_callback(safetyhook::Context&) noexcept {
    callbacks.fetch_add(1);
    if(auto* self=instance.load()) self->join();
    callbacks.fetch_sub(1);
}
void ParallelPasses::latch() noexcept {
    if(queued_) {
        // A deferring frame ended without the join: its passes were compiled empty.
        std::lock_guard stats(stats_mutex_);
        ++counters_.orphaned;queued_=0;
    }
    frame_active_.store(true);
    auto mode=static_cast<Mode>(requested_.load());
    if(mode!=Mode::off && !device_matches()) {turn_off("render device changed (drain or -rdevice)");mode=Mode::off;}
    if(mode!=Mode::off && paused_.load()) mode=Mode::off;
    if(mode==Mode::off) frame_active_.store(false);
    frame_mode_=mode;latched_=true;latch_qpc_=qpc_now();
}
bool ParallelPasses::defer(uintptr_t builder,uintptr_t value,uintptr_t pass,void* marker) noexcept {
    if(GetCurrentThreadId()!=main_thread_.load()) {++non_main_calls;return false;}
    if(joining_) return false;
    if(latched_ && qpc_now()-latch_qpc_>qpc_frequency()) {
        // No join for a second: a frame boundary passed without 0x227B1F.
        if(queued_) turn_off("deferred passes were not joined");
        latched_=false;frame_active_.store(false);
    }
    if(!latched_) latch();
    if(frame_mode_==Mode::off) return false;
    std::lock_guard stats(stats_mutex_);
    if(marker) {++counters_.marker;return false;}
    if(queued_==queue_.size()) {++counters_.overflow;return false;}
    uintptr_t vtable{};
    if(!read_memory(pass+0x2E8,vtable) || vtable!=base_+storage_vtable_rva) {++counters_.overflow;return false;}
    const auto& storage=*reinterpret_cast<const passes::CmdStorage*>(pass+0x2E8);
    const auto splice=passes::prepare_splice(storage);
    ++counters_.chunks[std::min<uint64_t>(storage.count,3)];
    if(splice.usable) ++counters_.front[splice.k==0?0:splice.k==4?1:2];
    queue_[queued_++]={builder,value,pass,splice};
    ++counters_.deferred;
    return true;
}
// Pass objects are pooled and reassigned every frame (docs/28 §2.8), so the
// key is (name, implementation space) plus its occurrence within the frame.
uint64_t ParallelPasses::pass_key(uintptr_t pass,KeyStats& seen) const {
    uintptr_t name{},space{};
    read_memory(pass+0x20,name);read_memory(pass+0xC0,space);
    seen.name=engine_string(name);seen.space=engine_string(space);
    seen.last_raw=seen.name+'/'+seen.space;
    seen.name=passes::normalize_name(seen.name);seen.space=passes::normalize_name(seen.space);
    return fnv(seen.space,fnv(seen.name)^0x9E3779B97F4A7C15ull);
}
void ParallelPasses::record(const Deferred& item,uint64_t& frame_dwords,uint64_t& largest) {
    auto* storage=reinterpret_cast<passes::CmdStorage*>(item.pass+0x2E8);
    const auto original=original_.load();
    const auto before=passes::total_dwords(*storage);
    const auto start=qpc_now();
    const bool spliced=passes::run_spliced(*storage,item.splice,tail_,
        [&]{original(item.builder,item.value,item.pass,nullptr);},
        [&](passes::CmdStorage* target,uint64_t size){return allocate_(target,size);},
        [](const passes::CmdStorage&){});
    const auto ticks=qpc_now()-start;
    const auto dwords=passes::total_dwords(*storage)-before;
    frame_dwords+=dwords;largest=std::max(largest,ticks);
    KeyStats seen;
    const auto base_key=pass_key(item.pass,seen);
    const auto key=base_key+occurrences_[base_key]++*0x100000001B3ull;
    std::lock_guard stats(stats_mutex_);
    ++(spliced?counters_.splice_used:counters_.splice_fallback);
    if(spliced) ++counters_.back[tail_.empty()?0:tail_.size()==4?1:2];
    auto& entry=keys_[key];
    if(!entry.passes) {entry.name=seen.name;entry.space=seen.space;}
    else if(entry.last_raw!=seen.last_raw) ++entry.raw_changes;
    entry.last_raw=seen.last_raw;
    ++entry.passes;entry.ticks+=ticks;entry.max_ticks=std::max(entry.max_ticks,ticks);entry.dwords+=dwords;
}
void ParallelPasses::join() noexcept {
    if(!latched_) return;
    latched_=false;
    const auto mode=frame_mode_;
    if(mode==Mode::off) {
        std::lock_guard stats(stats_mutex_);
        ++counters_.frames_off;
        return;
    }
    joining_=true;
    occurrences_.clear();
    uint64_t frame_dwords=0,largest=0;
    const auto start=qpc_now();
    for(size_t i=0;i<queued_;++i) record(queue_[i],frame_dwords,largest);
    const auto ticks=qpc_now()-start;
    const bool overflowed=queued_==queue_.size();
    queued_=0;
    joining_=false;
    frame_active_.store(false);
    bool too_long=false,overflow_off=false;
    {
        std::lock_guard stats(stats_mutex_);
        ++counters_.frames_defer;
        counters_.join.add(ticks);
        counters_.frame_dwords+=frame_dwords;counters_.frame_ticks+=ticks;counters_.largest_ticks+=largest;++counters_.frames_measured;
        overflow_frames_=overflowed?overflow_frames_+1:0;
        too_long=ticks>ms_ticks(join_limit_ms);
        overflow_off=overflow_frames_>=overflow_limit_frames;
    }
    if(too_long) turn_off("join exceeded 50 ms");
    if(overflow_off) turn_off("deferral queue overflowed for 100 frames");
}
json ParallelPasses::status() {
    json result={{"requested_mode",mode_name(requested_.load())},{"hooks_installed",static_cast<bool>(execute_hook_)},
        {"hooks_enabled",execute_hook_.enabled() && join_hook_.enabled()},{"frame_active",frame_active_.load()},
        {"game_folder_dxgi",game_folder_dxgi()},{"qpc_frequency",qpc_frequency()},{"non_main_calls",non_main_calls.load()}};
    std::lock_guard stats(stats_mutex_);
    const auto& c=counters_;
    result["frames"]={{"off",c.frames_off},{"defer",c.frames_defer}};
    result["passes"]={{"deferred",c.deferred},{"marker_passthrough",c.marker},{"queue_overflow_or_bad_storage",c.overflow},
        {"orphaned_frames",c.orphaned},{"splice_used",c.splice_used},{"splice_fallback",c.splice_fallback},
        {"storage_records_at_deferral",{{"0",c.chunks[0]},{"1",c.chunks[1]},{"2",c.chunks[2]},{"3+",c.chunks[3]}}},
        {"front_dwords",{{"0",c.front[0]},{"4",c.front[1]},{"other",c.front[2]}}},
        {"back_dwords",{{"0",c.back[0]},{"4",c.back[1]},{"other",c.back[2]}}}};
    result["join"]=c.join.snapshot();
    if(c.frames_measured) result["defer_frame_mean"]={{"frames",c.frames_measured},
        {"dwords",c.frame_dwords/c.frames_measured},{"join_ticks",c.frame_ticks/c.frames_measured},
        {"largest_pass_ticks",c.largest_ticks/c.frames_measured}};
    std::vector<const KeyStats*> sorted;sorted.reserve(keys_.size());
    for(const auto& [key,entry]:keys_) sorted.push_back(&entry);
    std::sort(sorted.begin(),sorted.end(),[](auto* a,auto* b){return a->ticks>b->ticks;});
    json keys=json::array();
    for(size_t i=0;i<sorted.size() && i<60;++i) {
        const auto& k=*sorted[i];
        keys.push_back({{"name",k.name},{"space",k.space},{"passes",k.passes},{"ticks",k.ticks},{"max_ticks",k.max_ticks},
            {"dwords",k.dwords},{"raw_name_changes",k.raw_changes}});
    }
    result["keys"]=std::move(keys);result["key_count"]=keys_.size();
    result["off_reason"]=reason_.empty()?json(nullptr):json(reason_);
    result["error"]=error_.empty()?json(nullptr):json(error_);
    return result;
}
}
