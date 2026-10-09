#include "render_probe.hpp"
#include <tlhelp32.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <span>
#include <stdexcept>
#include <Zydis.h>

namespace ot {
namespace {
std::atomic<RenderProbe*> observer{};
std::atomic<uint32_t> callbacks{};
constexpr uintptr_t hook_rva=0x2B3193;
constexpr uintptr_t present_hook_rva=0x2BFEDA;
constexpr uintptr_t compile_begin_rva=0x2B1B40,compile_end_rva=0x2B266A;
constexpr uintptr_t draw_batch_rva=0x2E6243;
constexpr uintptr_t rig_select_rva=0x538A4E,rig_begin_rva=0x538B11,rig_end_rva=0x538CE3;
constexpr uintptr_t rig_drawables_rva=0x538AD4,rig_graph_drawables_rva=0x4D458B,rig_graph_cameras_rva=0x4D46E4,rig_graph_end_rva=0x4D4E54;
constexpr std::array<uint8_t,16> rig_drawables_signature={0x48,0x3B,0x71,0x10,0x0F,0x83,0x71,0x02,0x00,0x00,0x48,0x8B,0x41,0x08,0x48,0x8B};
constexpr std::array<uint8_t,16> rig_graph_drawables_signature={0x48,0x3B,0x43,0x10,0x0F,0x83,0x89,0x09,0x00,0x00,0x48,0x8B,0x43,0x08,0x48,0x83};
constexpr std::array<uint8_t,16> rig_graph_cameras_signature={0x48,0x3B,0x79,0x10,0x0F,0x83,0xB0,0x08,0x00,0x00,0x48,0x8B,0x59,0x08,0x48,0x8B};
constexpr std::array<uint8_t,16> rig_graph_end_signature={0x48,0x8B,0x44,0x24,0x40,0x48,0x05,0x98,0x00,0x00,0x00,0x48,0x89,0x44,0x24,0x40};
constexpr uintptr_t rig_dimensions_rva=0x1610250;
constexpr uintptr_t rig_ego_parts_rva=0xA3CADE;
constexpr std::array<uint8_t,11> rig_ego_parts_signature={0x49,0x8B,0xF8,0x48,0x8B,0xD9,0x74,0x0F,0x49,0x8B,0xD0};
constexpr std::array<uint8_t,17> rig_dimensions_signature={0x48,0x83,0xEC,0x58,0xF3,0x0F,0x10,0x0D,0x3C,0xC2,0xF0,0,0x0F,0x57,0xC0,0x8B,0xC2};
constexpr std::array<uint8_t,16> rig_select_signature={0x44,0x0F,0x29,0x84,0x24,0xC0,0x01,0x00,0x00,0x41,0x8B,0xED,0xF3,0x44,0x0F,0x10};
constexpr std::array<uint8_t,13> rig_begin_signature={0xBA,1,0,0,0,0x49,0x8B,0xCF,0xE8,0x42,0xAD,0xF5,0xFF};
constexpr std::array<uint8_t,16> rig_end_signature={0x4C,0x8B,0xBC,0x24,0x28,0x02,0,0,0x45,0x33,0xED,0xFF,0xC5,0x83,0xFD,0x09};
constexpr std::array<uint8_t,25> draw_batch_signature={
    0x8B,0x4C,0x24,0x40,0x4C,0x8D,0x05,0x02,0x4B,0xF3,0x01,
    0x48,0x8B,0x5D,0x7F,0xFF,0xC1,0x8B,0xC1,0x89,0x4C,0x24,0x40,0x48,0x3B};
constexpr std::array<uint8_t,24> compile_begin_signature={
    0x4D,0x8B,0x3A,0x49,0x8B,0x4F,0x10,0x48,0x85,0xC9,0x0F,0x84,
    0x1A,0x0B,0x00,0x00,0x4D,0x8B,0x7F,0x08,0x48,0x8D,0x0C,0x49};
constexpr std::array<uint8_t,24> compile_end_signature={
    0x49,0x83,0xC2,0x08,0x4C,0x89,0x55,0xF0,0x4D,0x3B,0xD3,0x0F,
    0x85,0xC5,0xF4,0xFF,0xFF,0x48,0x8B,0x7C,0x24,0x58,0x0F,0xB7};
constexpr std::array<uint8_t,27> present_signature={
    0x85,0xC0,0x79,0x12,0x8B,0xD0,0x48,0x8D,0x0D,0x41,0x4E,0xF5,0x01,
    0x48,0x83,0xC4,0x28,0xE9,0x70,0xC9,0xE3,0xFF,0x48,0x83,0xC4,0x28,0xC3};
// The surrounding instruction sequence selects RTVs and calls OMSetRenderTargets.
// The detour starts at mov r9,[rbp+0xA8]; relocated bytes contain no CALL.
constexpr std::array<uint8_t,38> signature={
    0x48,0x8B,0x85,0xA0,0x01,0x00,0x00,0x4C,0x8D,0x45,0x68,0x0F,0xB7,0x55,0x60,
    0x4C,0x8B,0x8D,0xA8,0x00,0x00,0x00,0x48,0x8B,0x88,0xF0,0xFC,0x63,0x03,
    0x48,0x8B,0x01,0xFF,0x90,0x08,0x01,0x00,0x00};
uintptr_t find_target(std::span<const uint8_t> bytes_to_find,size_t adjustment,uintptr_t expected_rva) {
    auto* base=reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
    auto* section=IMAGE_FIRST_SECTION(nt);
    uintptr_t found{};
    for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i) {
        if(!(section[i].Characteristics&IMAGE_SCN_MEM_EXECUTE)) continue;
        auto bytes=std::span(base+section[i].VirtualAddress,section[i].Misc.VirtualSize);
        auto at=bytes.begin();
        while((at=std::search(at,bytes.end(),bytes_to_find.begin(),bytes_to_find.end()))!=bytes.end()) {
            if(found) throw std::runtime_error("Render hook signature is ambiguous");
            found=reinterpret_cast<uintptr_t>(&*at)+adjustment;
            ++at;
        }
    }
    if(found!=reinterpret_cast<uintptr_t>(base)+expected_rva)
        throw std::runtime_error("Render hook signature does not resolve to the inspected call site");
    return found;
}
struct CodeRanges { std::vector<std::pair<uintptr_t,uintptr_t>> ranges; };
bool contains(const CodeRanges& ranges,DWORD64 ip) noexcept {
    for(const auto& [begin,end]:ranges.ranges) if(ip>=begin && ip<end) return true;
    return false;
}
// Check return addresses too: a callback can be inside a Windows/D3D function
// while its instruction pointer is outside this DLL. The thread stays suspended
// only during this read-only unwind and is resumed by the caller in all cases.
bool stack_clear(CONTEXT context,const CodeRanges& ranges) noexcept {
    __try {
        for(unsigned depth=0;depth<256;++depth) {
            if(!context.Rip) return true;
            if(contains(ranges,context.Rip)) return false;
            DWORD64 image_base{};
            auto* function=RtlLookupFunctionEntry(context.Rip,&image_base,nullptr);
            if(function) {
                void* handler_data{};DWORD64 establisher{};
                RtlVirtualUnwind(UNW_FLAG_NHANDLER,image_base,context.Rip,function,&context,
                                 &handler_data,&establisher,nullptr);
            } else {
                SIZE_T read{};DWORD64 next{};
                if(!ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(context.Rsp),
                                      &next,sizeof(next),&read) || read!=sizeof(next)) return false;
                context.Rip=next;context.Rsp+=sizeof(next);
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    return false;
}
}

RenderProbe::~RenderProbe() {
    // Runtime calls close() before destruction; a failed close retains the object.
    for(auto* hook:hookset()) hook->reset();
    if(module_reference_) FreeLibrary(module_reference_);
}
void RenderProbe::enable(bool vehicle_metadata,const std::string& mode,bool frame_timing,bool draw_metadata) {
    std::lock_guard lock(control_);
    if(mode!="observe" && mode!="rig") throw std::runtime_error("Render mode must be observe or rig");
    if(mode=="rig" && vehicle_metadata) throw std::runtime_error("Vehicle metadata requires observe mode");
    const auto selected=[&](const safetyhook::MidHook* hook) {
        if(hook==&rig_select_hook_ || hook==&rig_begin_hook_ || hook==&rig_end_hook_ || hook==&rig_dimensions_hook_ || hook==&rig_ego_parts_hook_ ||
           hook==&rig_drawables_hook_ || hook==&rig_graph_drawables_hook_ || hook==&rig_graph_cameras_hook_ || hook==&rig_graph_end_hook_) return true;
        if(mode=="observe") return hook!=&draw_batch_hook_ || (vehicle_metadata && draw_metadata);
        return frame_timing && hook==&present_hook_;
    };
    const auto all=hookset();
    if(accepting_ && vehicle_metadata_==vehicle_metadata && std::all_of(all.begin(),all.end(),[&](auto* hook){return hook->enabled()==selected(hook);})) return;
    if(GetModuleHandleW(L"renderdoc.dll"))
        throw std::runtime_error("RenderDoc is loaded; restart ETS2 normally before enabling ot render hooks");
    if(auto stream=stream_.load()) stream->stop();
    if(accepting_ && rig_select_hook_.enabled() && rig_begin_hook_.enabled() &&
       rig_end_hook_.enabled() && rig_dimensions_hook_.enabled()) {
        // Keep camera submission continuous while changing observation modes.
        // Dropping the selection hook even briefly can omit sensor-only views
        // from the engine's already queued render work.
        observing_=false;
        for(auto* hook:{&hook_,&present_hook_,&compile_begin_hook_,&compile_end_hook_,&draw_batch_hook_}) {
            if(hook->enabled() && !hook->disable()) {
                disable_locked(true);
                throw std::runtime_error("Cannot disable previous observation hooks");
            }
        }
        const auto deadline=GetTickCount64()+2000;
        while(callbacks.load() && GetTickCount64()<deadline) Sleep(1);
        if(callbacks.load()) {
            disable_locked(true);
            throw std::runtime_error("Previous observation callbacks did not drain");
        }
        for(auto* camera:cameras()) camera->cancel();
        bundle_frame_=0;published_bundle_=nullptr;
    } else if(!disable_locked(false)) {
        rig_.clear();
        throw std::runtime_error("Previous render mode did not drain: "+last_error_);
    }
    if(!hook_) {
        if(observer.load() && observer.load()!=this)
            throw std::runtime_error("A previous render observer is retained; restart ETS2");
        const auto target=find_target(signature,15,hook_rva);
        const auto present_target=find_target(present_signature,0,present_hook_rva);
        const auto compile_begin_target=find_target(compile_begin_signature,0,compile_begin_rva);
        const auto compile_end_target=find_target(compile_end_signature,0,compile_end_rva);
        const auto draw_batch_target=find_target(draw_batch_signature,0,draw_batch_rva);
        if(!module_reference_) {
            if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                  reinterpret_cast<LPCWSTR>(&callback),&module_reference_))
                throw std::runtime_error("Cannot retain ot_core while its render hook is installed");
            module_begin_=reinterpret_cast<uintptr_t>(module_reference_);
            auto* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(module_begin_);
            auto* nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(module_begin_+dos->e_lfanew);
            module_end_=module_begin_+nt->OptionalHeader.SizeOfImage;
        }
        auto result=safetyhook::MidHook::create(target,&callback,safetyhook::MidHook::StartDisabled);
        if(!result) throw std::runtime_error("SafetyHook could not create the render detour");
        hook_=std::move(*result);
        auto present_result=safetyhook::MidHook::create(present_target,&present_callback,safetyhook::MidHook::StartDisabled);
        if(!present_result) {
            hook_.reset();
            throw std::runtime_error("SafetyHook could not create the Present observer");
        }
        present_hook_=std::move(*present_result);
        auto compile_begin_result=safetyhook::MidHook::create(compile_begin_target,&compile_begin_callback,safetyhook::MidHook::StartDisabled);
        if(!compile_begin_result) {
            for(auto* hook:hookset()) hook->reset();
            throw std::runtime_error("Cannot create command compilation observer");
        }
        compile_begin_hook_=std::move(*compile_begin_result);
        auto compile_end_result=safetyhook::MidHook::create(compile_end_target,&compile_end_callback,safetyhook::MidHook::StartDisabled);
        if(!compile_end_result) {
            for(auto* hook:hookset()) hook->reset();
            throw std::runtime_error("Cannot create command compilation end observer");
        }
        compile_end_hook_=std::move(*compile_end_result);
        auto draw_result=safetyhook::MidHook::create(draw_batch_target,&draw_batch_callback,safetyhook::MidHook::StartDisabled);
        if(!draw_result) {
            for(auto* hook:hookset()) hook->reset();
            throw std::runtime_error("Cannot create draw binding observer");
        }
        draw_batch_hook_=std::move(*draw_result);
        const std::array<std::tuple<safetyhook::MidHook*,uintptr_t,safetyhook::MidHookFn>,9> rig_hooks={{
            {&rig_select_hook_,find_target(rig_select_signature,0,rig_select_rva),&rig_select_callback},
            {&rig_begin_hook_,find_target(rig_begin_signature,0,rig_begin_rva),&rig_begin_callback},
            {&rig_end_hook_,find_target(rig_end_signature,0,rig_end_rva),&rig_end_callback},
            {&rig_dimensions_hook_,find_target(rig_dimensions_signature,0,rig_dimensions_rva),&rig_dimensions_callback},
            {&rig_ego_parts_hook_,find_target(rig_ego_parts_signature,0,rig_ego_parts_rva),&rig_ego_parts_callback},
            {&rig_drawables_hook_,find_target(rig_drawables_signature,0,rig_drawables_rva),&rig_drawables_callback},
            {&rig_graph_drawables_hook_,find_target(rig_graph_drawables_signature,0,rig_graph_drawables_rva),&rig_graph_drawables_callback},
            {&rig_graph_cameras_hook_,find_target(rig_graph_cameras_signature,0,rig_graph_cameras_rva),&rig_graph_cameras_callback},
            {&rig_graph_end_hook_,find_target(rig_graph_end_signature,0,rig_graph_end_rva),&rig_graph_end_callback}}};
        for(const auto& [destination,address,function]:rig_hooks) {
            auto rig_result=safetyhook::MidHook::create(address,function,safetyhook::MidHook::StartDisabled);
            if(!rig_result) {
                for(auto* hook:hookset()) hook->reset();
                throw std::runtime_error("Cannot create camera submission hook");
            }
            *destination=std::move(*rig_result);
        }
        // A suspended external callee must not have a return address in a
        // trampoline without unwind metadata. Check the instructions themselves.
        ZydisDecoder decoder{};
        ZydisDecoderInit(&decoder,ZYDIS_MACHINE_MODE_LONG_64,ZYDIS_STACK_WIDTH_64);
        for(const auto* inspected:hookset()) {
        const auto& relocated=inspected->original_bytes();
        for(size_t offset=0;offset<relocated.size();) {
            ZydisDecodedInstruction instruction{};
            if(!ZYAN_SUCCESS(ZydisDecoderDecodeInstruction(&decoder,nullptr,
                    relocated.data()+offset,relocated.size()-offset,&instruction)) ||
                    instruction.meta.category==ZYDIS_CATEGORY_CALL) {
                for(auto* hook:hookset()) hook->reset();
                throw std::runtime_error("Render detour cannot retain a relocated external CALL");
            }
            offset+=instruction.length;
        }
        }
        observer.store(this);
    }
    frame_boundary_seen_=false;
    AcquireSRWLockExclusive(&frames_lock_);frames_written_=0;ReleaseSRWLockExclusive(&frames_lock_);
    missed_frames_=0;
    AcquireSRWLockExclusive(&records_lock_);records_written_=0;ReleaseSRWLockExclusive(&records_lock_);
    pass_commands_.clear();
    vehicle_metadata_=vehicle_metadata;
    for(auto* hook:hookset()) {
        if(!selected(hook) || hook->enabled()) continue;
        if(hook->enable()) continue;
        disable_locked(true);
        throw std::runtime_error("Render observer enable failed");
    }
    // In particular, do not redirect a camera before the matching end hook
    // is installed. Mode switches retain the rig configuration while drained.
    mode_=mode;
    observing_=true;
    accepting_=true;
    last_error_.clear();
    log("Render mode enabled: "+mode+(frame_timing?" with frame timing":""));
}
void RenderProbe::disable() noexcept {
    try {
        std::lock_guard lock(control_);
        disable_locked(true);
    } catch(...) { accepting_=false; }
}
bool RenderProbe::disable_locked(bool clear_rig) {
    accepting_=false;
    observing_=false;
    if(auto stream=stream_.load()) stream->stop();
    vehicle_metadata_=false;
    frame_boundary_seen_=false;
    mode_="off";
    last_error_.clear();
    bool changed=false;
    const auto draining=[&](auto* hook) {return hook==&rig_begin_hook_ || hook==&rig_end_hook_ || hook==&rig_dimensions_hook_ ||
        hook==&rig_drawables_hook_ || hook==&rig_graph_drawables_hook_ || hook==&rig_graph_cameras_hook_ || hook==&rig_graph_end_hook_;};
    for(auto* hook:hookset()) if(!draining(hook) && hook->enabled()) {
        changed=true;
        if(!hook->disable()) last_error_="Render observer disable failed";
    }
    if(changed) log("Render probe disable requested for all hook sites");
    // The private source copy lives until the engine has copied its pose
    // and frustum. Keep the end callback until these submissions finish.
    const auto deadline=GetTickCount64()+2000;
    while((callbacks.load() || rig_.in_flight()) && GetTickCount64()<deadline) Sleep(1);
    if(!callbacks.load() && !rig_.in_flight()) {
        for(auto* hook:hookset()) if(draining(hook) && hook->enabled() && !hook->disable()) last_error_="Camera graph drain hook disable failed";
        if(clear_rig) rig_.clear();
    } else last_error_="Camera submission still draining; payload must stay loaded";
    for(auto* camera:cameras()) camera->cancel();
    bundle_frame_=0;published_bundle_=nullptr;
    return last_error_.empty();
}
bool RenderProbe::quiescent() noexcept {
    if(callbacks.load() || rig_.in_flight()) return false;
    CodeRanges ranges{};ranges.ranges.reserve(1+2*hookset().size());
    ranges.ranges.push_back({module_begin_,module_end_});
    for(const auto* hook:hookset()) {
        ranges.ranges.push_back({hook->stub().address(),hook->stub().address()+hook->stub().size()});
        ranges.ranges.push_back({hook->trampoline().address(),hook->trampoline().address()+hook->trampoline().size()});
    }
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0));
    if(!snapshot) return false;
    THREADENTRY32 entry{sizeof(entry)};
    if(!Thread32First(snapshot.h,&entry)) return false;
    do {
        if(entry.th32OwnerProcessID!=GetCurrentProcessId() || entry.th32ThreadID==GetCurrentThreadId()) continue;
        Handle thread(OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|SYNCHRONIZE,FALSE,entry.th32ThreadID));
        if(!thread) {if(GetLastError()==ERROR_INVALID_PARAMETER) continue;return false;}
        if(WaitForSingleObject(thread.h,0)==WAIT_OBJECT_0) continue;
        if(SuspendThread(thread.h)==static_cast<DWORD>(-1)) return false;
        CONTEXT context{};context.ContextFlags=CONTEXT_FULL;
        const bool clear=GetThreadContext(thread.h,&context) && stack_clear(context,ranges);
        const auto resumed=ResumeThread(thread.h);
        if(!clear || resumed==static_cast<DWORD>(-1)) return false;
    } while(Thread32Next(snapshot.h,&entry));
    return GetLastError()==ERROR_NO_MORE_FILES && callbacks.load()==0;
}
bool RenderProbe::close() noexcept {
    disable();
    const auto all=hookset();
    if(std::none_of(all.begin(),all.end(),[](auto* h){return static_cast<bool>(*h);})) return true;
    if(std::any_of(all.begin(),all.end(),[](auto* h){return h->enabled();})) return false;
    const auto deadline=GetTickCount64()+2000;
    do {
        if(quiescent()) {
            observer.store(nullptr);
            for(auto* hook:hookset()) hook->reset();
            log("Render probe drained; trampoline and stub released");
            return true;
        }
        Sleep(1);
    } while(GetTickCount64()<deadline);
    log("Render observer retained after drain timeout; hooks disabled, restart ETS2 before DLL replacement");
    return false;
}
void RenderProbe::callback(safetyhook::Context& context) noexcept {
    callbacks.fetch_add(1);
    if(auto* self=observer.load();self && self->accepting_.load() && self->observing_.load()) {
        const auto start=qpc_now();
        self->observe(context);
        self->bind_timing_.add(qpc_now()-start);
    }
    callbacks.fetch_sub(1);
}
void RenderProbe::present_callback(safetyhook::Context& context) noexcept {
    callbacks.fetch_add(1);
    if(auto* self=observer.load();self && self->accepting_.load() && self->observing_.load()) {
        const auto start=qpc_now();
        self->present(static_cast<HRESULT>(context.rax));
        self->present_timing_.add(qpc_now()-start);
    }
    callbacks.fetch_sub(1);
}
void RenderProbe::compile_begin_callback(safetyhook::Context& context) noexcept {
    callbacks.fetch_add(1);
    if(auto* self=observer.load();self && self->accepting_.load() && self->observing_.load() && self->compile_frame()) {
        const auto start=qpc_now();
        uintptr_t input{},output{};uint16_t id{};
        if(read_memory(context.r10,input) && read_memory(context.rsp+0x58,output) &&
           read_memory(context.rbp+0x240,id))
            self->pass_commands_.begin(context.rbp,input,output,id,self->vehicle_metadata_.load(),self->sdk_state_.load());
        self->compile_begin_timing_.add(qpc_now()-start);
    }
    callbacks.fetch_sub(1);
}
void RenderProbe::compile_end_callback(safetyhook::Context& context) noexcept {
    callbacks.fetch_add(1);
    if(auto* self=observer.load();self && self->accepting_.load() && self->observing_.load()) {
        const auto start=qpc_now();
        self->pass_commands_.end(context.rbp);
        self->compile_end_timing_.add(qpc_now()-start);
    }
    callbacks.fetch_sub(1);
}
void RenderProbe::rig_select_callback(safetyhook::Context& context) noexcept {
    ++callbacks;
    if(auto* self=observer.load();self && self->accepting_.load()) {
        auto stream=self->stream_.load();
        // Finish an initial scene preparation for a new rig before capturing;
        // its first render can still lack trees and poles. Also keep private
        // descriptors immutable until the previous queued graphs finish.
        self->rig_.select(context,self->rig_.can_capture()?
            (stream && stream->running()?stream->select_pending():UINT32_MAX):0);
    }
    --callbacks;
}
void RenderProbe::rig_begin_callback(safetyhook::Context& context) noexcept {
    ++callbacks;
    if(auto* self=observer.load();self && (self->accepting_.load() || self->rig_.in_flight())) self->rig_.begin(context);
    --callbacks;
}
void RenderProbe::rig_end_callback(safetyhook::Context& context) noexcept {
    ++callbacks;
    if(auto* self=observer.load()) self->rig_.end(context);
    --callbacks;
}
void RenderProbe::rig_dimensions_callback(safetyhook::Context& context) noexcept {
    ++callbacks;
    if(auto* self=observer.load()) self->rig_.dimensions(context);
    --callbacks;
}
void RenderProbe::rig_ego_parts_callback(safetyhook::Context& context) noexcept {
    ++callbacks;
    if(auto* self=observer.load();self && self->accepting_.load()) self->rig_.ego_parts(context);
    --callbacks;
}
void RenderProbe::rig_drawables_callback(safetyhook::Context& context) noexcept {
    ++callbacks;if(auto* self=observer.load()) self->rig_.submission_drawables(context);--callbacks;
}
void RenderProbe::rig_graph_drawables_callback(safetyhook::Context& context) noexcept {
    ++callbacks;if(auto* self=observer.load()) self->rig_.graph_drawables(context);--callbacks;
}
void RenderProbe::rig_graph_cameras_callback(safetyhook::Context& context) noexcept {
    ++callbacks;if(auto* self=observer.load()) self->rig_.graph_cameras(context);--callbacks;
}
void RenderProbe::rig_graph_end_callback(safetyhook::Context&) noexcept {
    ++callbacks;if(auto* self=observer.load()) self->rig_.graph_end();--callbacks;
}
json RenderProbe::camera_rig(const json& request) {
    std::lock_guard lock(control_);
    if(request.contains("views") || request.contains("enabled")) {
        if(auto stream=stream_.load();stream && stream->running()) {
            if(request.value("enabled",true)) throw std::runtime_error("Stop the capture stream before changing cameras");
            stream->stop();
        }
        if(request.value("enabled",true) && !rig_end_hook_.enabled())
            throw std::runtime_error("Enable the render probe before configuring cameras");
        return rig_.configure(request);
    }
    return rig_.status();
}
void RenderProbe::draw_batch_callback(safetyhook::Context& context) noexcept {
    callbacks.fetch_add(1);
    if(auto* self=observer.load();self && self->accepting_.load() && self->observing_.load() && self->vehicle_metadata_.load() && self->compile_frame()) {
        const auto start=qpc_now();
        uintptr_t work{},input{},items{};
        if(read_memory(context.rbp+0x5F,work) && read_memory(work+0xF0,input) &&
           read_memory(context.rbp-0x39,items))
            self->pass_commands_.draw_batch(input,items,context.r15,static_cast<uint32_t>(context.rsi));
        self->draw_batch_timing_.add(qpc_now()-start);
    }
    callbacks.fetch_sub(1);
}
void RenderProbe::Timing::add(uint64_t ticks) noexcept {
    total.fetch_add(ticks,std::memory_order_relaxed);
    auto previous=maximum.load(std::memory_order_relaxed);
    while(previous<ticks && !maximum.compare_exchange_weak(previous,ticks,std::memory_order_relaxed)) {}
    const auto bucket=ticks?std::bit_width(ticks)-1:0;
    buckets[bucket].fetch_add(1,std::memory_order_relaxed);
    count.fetch_add(1,std::memory_order_relaxed);
}
json RenderProbe::Timing::snapshot() const {
    json histogram=json::array();
    for(const auto& bucket:buckets) histogram.push_back(bucket.load(std::memory_order_relaxed));
    return {{"samples",count.load(std::memory_order_relaxed)},
        {"total_ticks",total.load(std::memory_order_relaxed)},
        {"max_ticks",maximum.load(std::memory_order_relaxed)},{"log2_tick_buckets",histogram}};
}
void RenderProbe::present(HRESULT result) noexcept {
    DWORD foreground_pid{};
    GetWindowThreadProcessId(GetForegroundWindow(),&foreground_pid);
    PresentRecord record{presents_.fetch_add(1)+1,qpc_now(),sdk_frame_.load(),result,GetCurrentThreadId(),
                         foreground_pid==GetCurrentProcessId()};
    frame_boundary_seen_=true;
    if(!TryAcquireSRWLockExclusive(&frames_lock_)) {missed_frames_.fetch_add(1);return;}
    frames_[frames_written_%frames_.size()]=record;++frames_written_;
    ReleaseSRWLockExclusive(&frames_lock_);
}
json RenderProbe::frames(uint64_t after_id) {
    std::lock_guard lock(control_);
    std::array<PresentRecord,600> copy{};uint64_t written{};
    AcquireSRWLockShared(&frames_lock_);copy=frames_;written=frames_written_;ReleaseSRWLockShared(&frames_lock_);
    json records=json::array();
    for(auto i=written>copy.size()?written-copy.size():0;i<written;++i) {
        const auto& record=copy[i%copy.size()];
        if(record.id<=after_id) continue;
        records.push_back({{"present_id",record.id},{"qpc",record.qpc},{"hresult",record.result},
            {"sdk_frame_hint",record.sdk_frame},{"thread_id",record.thread},{"game_foreground",record.game_foreground}});
    }
    return {{"enabled",present_hook_.enabled()},{"observation_session_qpc",observation_session_},{"qpc_frequency",qpc_frequency()},
        {"boundary_observed",frame_boundary_seen_.load()},{"present_calls",presents_.load()},
        {"missed_records",missed_frames_.load()},{"records",records}};
}
bool RenderProbe::compile_frame() const noexcept {
    const auto stream=stream_.load();
    return !stream || !stream->running() ||
        stream->compiling();
}
void RenderProbe::observe(const safetyhook::Context& context) noexcept {
    const auto stream=stream_.load();
    if(stream && stream->running() && !stream->pending()) return;
    Record record{};
    record.sequence=calls_.fetch_add(1)+1;
    record.sdk_frame_hint=sdk_frame_.load(std::memory_order_relaxed);
    record.render_frame=frame_boundary_seen_.load()?presents_.load()+1:0;
    record.thread=GetCurrentThreadId();
    record.count=static_cast<uint32_t>(context.rdx);
    uintptr_t cursor{},data{};
    const auto index=static_cast<uint32_t>(context.r13);
    if((!stream || !stream->running() || stream->compiling()) &&
       index && read_memory(context.rbp-0x30,cursor) && read_memory(cursor,record.compiled_id) &&
       read_memory(context.r12,data)) {
        record.token=data+(index-1)*4;
        record.pass=pass_commands_.lookup(record.compiled_id,record.token);
    }
    if(record.count>record.targets.size() ||
       !read_memory(context.rax+0x363FCF0,record.context) ||
       !read_memory(context.rbp+0xA8,record.depth) ||
       (record.count && !copy_memory(context.r8,record.targets.data(),record.count*sizeof(uintptr_t)))) {
        missed_.fetch_add(1);return;
    }
    if(!stream || !stream->running()) for(auto* camera:cameras())
        camera->observe(reinterpret_cast<ID3D11DeviceContext*>(record.context),record.count,
            record.targets.data(),record.sequence,record.sdk_frame_hint,record.render_frame,observation_session_,record.pass.get());
    if(stream)
        stream->observe(reinterpret_cast<ID3D11DeviceContext*>(record.context),record.count,
            record.targets.data(),record.sequence,record.sdk_frame_hint,record.render_frame,record.pass.get());
    if(!TryAcquireSRWLockExclusive(&records_lock_)) {missed_.fetch_add(1);return;}
    records_[records_written_%records_.size()]=record;
    ++records_written_;
    ReleaseSRWLockExclusive(&records_lock_);
}
json RenderProbe::capture(const std::string& action,const CaptureOptions& options) {
    std::lock_guard lock(control_);
    if(auto stream=stream_.load();action!="status" && stream && stream->running())
        throw std::runtime_error("Stop the capture stream before a manual capture command");
    if(action=="arm" && !hook_.enabled()) throw std::runtime_error("Capture requires render_probe observe mode");
    if(action=="arm") {
        for(auto* camera:cameras()) {
            const auto phase=camera->command("status",0,false).at("phase");
            if(phase=="armed" || phase=="waiting_gpu") throw std::runtime_error("A camera capture is already pending");
        }
        bundle_frame_=0;
    }
    return gpu_.command(action,0,true,options);
}
json RenderProbe::capture_views(const std::string& action,Transport* publisher,bool metadata,const CaptureOptions& options) {
    std::lock_guard lock(control_);
    if(auto stream=stream_.load();action!="status" && stream && stream->running())
        throw std::runtime_error("Stop the capture stream before a manual capture command");
    if(action=="arm") {
        if(!hook_.enabled()) throw std::runtime_error("Capture requires render_probe observe mode");
        for(auto* camera:cameras()) {
            const auto phase=camera->command("status",0,false).at("phase");
            if(phase=="armed" || phase=="waiting_gpu") throw std::runtime_error("A camera capture is already pending");
        }
        // Skip the interval already in progress so every requested view has a
        // chance to render after every requested camera has been armed.
        bundle_mask_=rig_.mask();if(!bundle_mask_) bundle_mask_=0x27;
        bundle_frame_=presents_.load()+2;
        published_bundle_=nullptr;
        try {for(auto* camera:capture_cameras()) camera->command("arm",bundle_frame_,true,options);}
        catch(...) {for(auto* camera:cameras()) camera->cancel();bundle_frame_=0;throw;}
    } else if(action=="cancel") {
        for(auto* camera:cameras()) camera->cancel();
        bundle_frame_=0;
    } else if(action!="status" && action!="save" && action!="save_partial" && action!="publish" && action!="publish_partial") throw std::runtime_error("Unknown camera bundle action");
    json views=json::array();bool ready=true,error=false,pending=false;
    for(auto* camera:capture_cameras()) {
        auto state=camera->command("status",0,metadata);const auto phase=state.at("phase");
        ready=ready && phase=="ready";error=error || phase=="error";
        pending=pending || phase=="armed" || phase=="waiting_gpu";
        views.push_back(std::move(state));
    }
    if(action=="save") {
        if(!bundle_frame_ || !ready) throw std::runtime_error("No complete camera bundle to save");
        views=json::array();for(auto* camera:capture_cameras()) views.push_back(camera->command("save"));
    }
    if(action=="save_partial") {
        if(!bundle_frame_) throw std::runtime_error("No camera bundle was requested");
        views=json::array();
        for(auto* camera:capture_cameras()) {
            auto state=camera->command("status");
            views.push_back(state.at("phase")=="ready"?camera->command("save"):std::move(state));
        }
    }
    if(action=="publish" || action=="publish_partial") {
        if(!bundle_frame_ || pending || (!ready && action=="publish") || !publisher)
            throw std::runtime_error("Camera bundle is incomplete or still pending");
        if(!published_bundle_.is_null()) return published_bundle_;
        json names=json::array();for(unsigned slot=0;slot<9;++slot) if(bundle_mask_&(1u<<slot)) names.push_back("mirror"+std::to_string(slot));
        json manifest={{"render_frame_id",bundle_frame_},{"observation_session_qpc",observation_session_},
            {"complete",ready},{"requested_cameras",names},
            {"missing_views",json::array()},{"views",json::array()}};
        std::vector<BundleBlob> blobs;
        // control_ excludes arm/cancel/panic while these completed CPU spans
        // are copied. Render callbacks leave ready samples untouched.
        for(auto* camera:capture_cameras()) {
            const auto state=camera->command("status",0,false);
            if(state.at("phase")=="ready") camera->append_bundle(manifest["views"],blobs);
            else manifest["missing_views"].push_back({{"camera",state.at("camera")},
                {"phase",state.at("phase")},{"error",state.at("error")}});
        }
        if(manifest.at("views").empty()) throw std::runtime_error("No completed views to publish");
        auto result=publisher->publish_bundle(std::move(manifest),blobs);
        result["render_frame_id"]=bundle_frame_;result["observation_session_qpc"]=observation_session_;
        if(result.at("published").get<bool>()) published_bundle_=result;
        return result;
    }
    return {{"phase",!bundle_frame_?"idle":error?"error":ready?"ready":pending?"pending":"idle"},
        {"render_frame_id",bundle_frame_},{"observation_session_qpc",observation_session_},{"views",views}};
}
json RenderProbe::stream(const json& request,Transport& publisher,const CaptureOptions& options) {
    std::lock_guard lock(control_);
    const auto action=request.value("action",std::string("status"));
    auto stream=stream_.load();
    if(action=="start") {
        if(stream && stream->running()) throw std::runtime_error("A capture stream is already running");
        if(!hook_.enabled()) throw std::runtime_error("Capture stream requires render_probe observe mode");
        // The IPC command is the boundary for rates and bounded lifetime.
        const auto hz=request.at("hz").get<double>(),duration=request.at("duration").get<double>();
        if(!std::isfinite(hz) || hz<=0 || !std::isfinite(duration) || duration<=0)
            throw std::runtime_error("Stream hz and duration must be finite and positive");
        for(auto* camera:cameras()) {
            const auto phase=camera->phase();
            if(phase==GpuCapture::Phase::armed || phase==GpuCapture::Phase::waiting_gpu)
                throw std::runtime_error("A manual camera capture is still pending");
        }
        if(stream) stream->stop();
        for(auto* camera:cameras()) camera->cancel();
        bundle_frame_=0;published_bundle_=nullptr;
        auto mask=rig_.mask();if(!mask) mask=0x27;
        stream=std::make_shared<CaptureStream>(mask,options,hz,duration,presents_,observation_session_,publisher);
        stream_.store(stream);stream->start();
    } else if(action=="update") {
        if(!stream || !stream->running()) throw std::runtime_error("No capture stream to update");
        stream->update(options);
    } else if(action=="stop") {
        if(stream) stream->stop();
    } else if(action!="status") throw std::runtime_error("Unknown capture stream action");
    return stream?stream->status():json{{"running",false}};
}
json RenderProbe::status() {
    std::lock_guard lock(control_);
    std::array<Record,128> copy{};uint64_t written{};
    AcquireSRWLockShared(&records_lock_);copy=records_;written=records_written_;ReleaseSRWLockShared(&records_lock_);
    json entries=json::array();
    for(auto i=written>copy.size()?written-copy.size():0;i<written;++i) {
        const auto& record=copy[i%copy.size()];
        entries.push_back({{"sequence",record.sequence},{"sdk_frame_hint",record.sdk_frame_hint},
            {"render_frame_id",record.render_frame?json(record.render_frame):json(nullptr)},
            {"thread_id",record.thread},{"context",record.context},{"render_target_count",record.count},
            {"compiled_buffer_id",record.compiled_id},{"command_token",record.token},
            {"logical_pass",record.pass?*record.pass:json(nullptr)},
            {"render_targets",std::vector<uintptr_t>(record.targets.begin(),record.targets.begin()+record.count)},
            {"depth_view",record.depth}});
    }
    const auto all=hookset();
    return {{"active",std::count_if(all.begin(),all.end(),[](auto* h){return h->enabled();})},
        {"mode",mode_},{"frame_timing_enabled",present_hook_.enabled()},{"capture_enabled",hook_.enabled()},
        {"vehicle_metadata_enabled",vehicle_metadata_.load()},
        {"camera_rig",rig_.status()},
        {"callbacks_in_flight",callbacks.load()},
        {"qpc_frequency",qpc_frequency()},
        {"timing_scope","callback_body_elapsed; cumulative per module; excludes detour and counter bookkeeping"},
        {"pass_commands",pass_commands_.status()},
        {"hooks",json::array({{{"name","dx11.omset_before_bind"},{"tier",1},{"rva",hook_rva},
            {"enabled",hook_.enabled()},{"calls",calls_.load()},{"missed_records",missed_.load()},
            {"timing",bind_timing_.snapshot()}},
            {{"name","dxgi.present_return"},{"tier",1},{"rva",present_hook_rva},{"enabled",present_hook_.enabled()},
             {"calls",presents_.load()},{"timing",present_timing_.snapshot()}},
            {{"name","dx11.compile_pass_begin"},{"tier",1},{"rva",compile_begin_rva},{"enabled",compile_begin_hook_.enabled()},
             {"timing",compile_begin_timing_.snapshot()}},
            {{"name","dx11.compile_pass_end"},{"tier",1},{"rva",compile_end_rva},{"enabled",compile_end_hook_.enabled()},
             {"timing",compile_end_timing_.snapshot()}},
            {{"name","dx11.prepare_draw_bindings"},{"tier",1},{"rva",draw_batch_rva},{"enabled",draw_batch_hook_.enabled()},
             {"timing",draw_batch_timing_.snapshot()}},
            {{"name","camera.sensor_selection"},{"tier",2},{"rva",rig_select_rva},{"enabled",rig_select_hook_.enabled()}},
            {{"name","camera.sensor_submission_begin"},{"tier",2},{"rva",rig_begin_rva},{"enabled",rig_begin_hook_.enabled()}},
            {{"name","camera.sensor_submission_end"},{"tier",2},{"rva",rig_end_rva},{"enabled",rig_end_hook_.enabled()}},
            {{"name","camera.sensor_dimensions"},{"tier",2},{"rva",rig_dimensions_rva},{"enabled",rig_dimensions_hook_.enabled()}},
            {{"name","camera.sensor_ego_parts"},{"tier",2},{"rva",rig_ego_parts_rva},{"enabled",rig_ego_parts_hook_.enabled()}},
            {{"name","camera.private_submission_drawables"},{"tier",2},{"rva",rig_drawables_rva},{"enabled",rig_drawables_hook_.enabled()}},
            {{"name","camera.private_graph_drawables"},{"tier",2},{"rva",rig_graph_drawables_rva},{"enabled",rig_graph_drawables_hook_.enabled()}},
            {{"name","camera.private_graph_cameras"},{"tier",2},{"rva",rig_graph_cameras_rva},{"enabled",rig_graph_cameras_hook_.enabled()}},
            {{"name","camera.private_graph_end"},{"tier",2},{"rva",rig_graph_end_rva},{"enabled",rig_graph_end_hook_.enabled()}}})},
        {"last_error",last_error_},{"render_coherent",false},{"capture",gpu_.command("status")},
        {"recent_bindings",entries}};
}
}
