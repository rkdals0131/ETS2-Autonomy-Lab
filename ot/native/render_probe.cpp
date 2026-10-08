#include "render_probe.hpp"
#include <tlhelp32.h>
#include <algorithm>
#include <span>
#include <stdexcept>
#include <Zydis.h>

namespace ot {
namespace {
std::atomic<RenderProbe*> observer{};
std::atomic<uint32_t> callbacks{};
constexpr uintptr_t hook_rva=0x2B3193;
constexpr uintptr_t label_hook_rva=0x21F09D;
constexpr uintptr_t present_hook_rva=0x2BFEDA;
constexpr std::array<uint8_t,27> present_signature={
    0x85,0xC0,0x79,0x12,0x8B,0xD0,0x48,0x8D,0x0D,0x41,0x4E,0xF5,0x01,
    0x48,0x83,0xC4,0x28,0xE9,0x70,0xC9,0xE3,0xFF,0x48,0x83,0xC4,0x28,0xC3};
constexpr std::array<uint8_t,29> label_signature={
    0x0F,0xB7,0x53,0x34,0x48,0x8D,0x8F,0x40,0x07,0x00,0x00,
    0x4C,0x8B,0x6C,0x24,0x78,0x4C,0x8B,0xA4,0x24,0xC0,0x00,0x00,0x00,
    0x4C,0x8B,0x74,0x24,0x70};
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
struct CodeRanges { std::array<std::pair<uintptr_t,uintptr_t>,7> ranges; };
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
    hook_.reset();
    label_hook_.reset();
    present_hook_.reset();
    if(module_reference_) FreeLibrary(module_reference_);
}
void RenderProbe::enable() {
    std::lock_guard lock(control_);
    if(hook_.enabled()) return;
    if(GetModuleHandleW(L"renderdoc.dll"))
        throw std::runtime_error("RenderDoc is loaded; restart ETS2 normally before enabling ot render hooks");
    if(!hook_) {
        if(observer.load() && observer.load()!=this)
            throw std::runtime_error("A previous render observer is retained; restart ETS2");
        const auto target=find_target(signature,15,hook_rva);
        const auto label_target=find_target(label_signature,0,label_hook_rva);
        const auto present_target=find_target(present_signature,0,present_hook_rva);
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
        auto label_result=safetyhook::MidHook::create(label_target,&label_callback,safetyhook::MidHook::StartDisabled);
        if(!label_result) {hook_.reset();throw std::runtime_error("SafetyHook could not create the graph label observer");}
        label_hook_=std::move(*label_result);
        auto present_result=safetyhook::MidHook::create(present_target,&present_callback,safetyhook::MidHook::StartDisabled);
        if(!present_result) {
            hook_.reset();label_hook_.reset();
            throw std::runtime_error("SafetyHook could not create the Present observer");
        }
        present_hook_=std::move(*present_result);
        // A suspended external callee must not have a return address in a
        // trampoline without unwind metadata. Check the instructions themselves.
        ZydisDecoder decoder{};
        ZydisDecoderInit(&decoder,ZYDIS_MACHINE_MODE_LONG_64,ZYDIS_STACK_WIDTH_64);
        for(const auto* inspected:{&hook_,&label_hook_,&present_hook_}) {
        const auto& relocated=inspected->original_bytes();
        for(size_t offset=0;offset<relocated.size();) {
            ZydisDecodedInstruction instruction{};
            if(!ZYAN_SUCCESS(ZydisDecoderDecodeInstruction(&decoder,nullptr,
                    relocated.data()+offset,relocated.size()-offset,&instruction)) ||
                    instruction.meta.category==ZYDIS_CATEGORY_CALL) {
                hook_.reset();
                label_hook_.reset();
                present_hook_.reset();
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
    accepting_=true;
    if(!label_hook_.enable()) {accepting_=false;throw std::runtime_error("Graph label detour enable failed");}
    if(!hook_.enable()) {
        accepting_=false;
        if(!label_hook_.disable()) last_error_="Graph label rollback failed";
        throw std::runtime_error("Render detour enable failed");
    }
    if(!present_hook_.enable()) {
        accepting_=false;
        const auto render_disabled=hook_.disable();
        const auto label_disabled=label_hook_.disable();
        if(!render_disabled || !label_disabled) last_error_="Render hook rollback failed";
        throw std::runtime_error("Present detour enable failed");
    }
    last_error_.clear();
    log("Render probe enabled at OMSetRenderTargets argument preparation");
}
void RenderProbe::disable() noexcept {
    try {
        std::lock_guard lock(control_);
        accepting_=false;
        frame_boundary_seen_=false;
        if(hook_.enabled()) {
            if(!hook_.disable()) last_error_="Render detour disable failed";
            else log("Render probe disabled; original game instructions restored");
        }
        if(label_hook_.enabled() && !label_hook_.disable()) last_error_="Graph label detour disable failed";
        if(present_hook_.enabled() && !present_hook_.disable()) last_error_="Present detour disable failed";
        gpu_.cancel();
    } catch(...) { accepting_=false; }
}
bool RenderProbe::quiescent() noexcept {
    if(callbacks.load()) return false;
    CodeRanges ranges{{{{module_begin_,module_end_},
        {hook_.stub().address(),hook_.stub().address()+hook_.stub().size()},
        {hook_.trampoline().address(),hook_.trampoline().address()+hook_.trampoline().size()},
        {label_hook_.stub().address(),label_hook_.stub().address()+label_hook_.stub().size()},
        {label_hook_.trampoline().address(),label_hook_.trampoline().address()+label_hook_.trampoline().size()},
        {present_hook_.stub().address(),present_hook_.stub().address()+present_hook_.stub().size()},
        {present_hook_.trampoline().address(),present_hook_.trampoline().address()+present_hook_.trampoline().size()}}}};
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
    if(!hook_ && !label_hook_ && !present_hook_) return true;
    if(hook_.enabled() || label_hook_.enabled() || present_hook_.enabled()) return false;
    const auto deadline=GetTickCount64()+2000;
    do {
        if(quiescent()) {
            observer.store(nullptr);
            hook_.reset();
            label_hook_.reset();
            present_hook_.reset();
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
    if(auto* self=observer.load();self && self->accepting_.load()) self->observe(context);
    callbacks.fetch_sub(1);
}
void RenderProbe::label_callback(safetyhook::Context& context) noexcept {
    callbacks.fetch_add(1);
    if(auto* self=observer.load();self && self->accepting_.load())
        self->gpu_.label_image(context.rdi,context.rbx+0x34);
    callbacks.fetch_sub(1);
}
void RenderProbe::present_callback(safetyhook::Context& context) noexcept {
    callbacks.fetch_add(1);
    if(auto* self=observer.load();self && self->accepting_.load())
        self->present(static_cast<HRESULT>(context.rax));
    callbacks.fetch_sub(1);
}
void RenderProbe::present(HRESULT result) noexcept {
    PresentRecord record{presents_.fetch_add(1)+1,qpc_now(),sdk_frame_.load(),result,GetCurrentThreadId()};
    frame_boundary_seen_=true;
    if(!TryAcquireSRWLockExclusive(&frames_lock_)) {missed_frames_.fetch_add(1);return;}
    frames_[frames_written_%frames_.size()]=record;++frames_written_;
    ReleaseSRWLockExclusive(&frames_lock_);
}
json RenderProbe::frames(uint64_t after_id) {
    std::array<PresentRecord,600> copy{};uint64_t written{};
    AcquireSRWLockShared(&frames_lock_);copy=frames_;written=frames_written_;ReleaseSRWLockShared(&frames_lock_);
    json records=json::array();
    for(auto i=written>copy.size()?written-copy.size():0;i<written;++i) {
        const auto& record=copy[i%copy.size()];
        if(record.id<=after_id) continue;
        records.push_back({{"present_id",record.id},{"qpc",record.qpc},{"hresult",record.result},
            {"sdk_frame_hint",record.sdk_frame},{"thread_id",record.thread}});
    }
    return {{"observation_session_qpc",observation_session_},{"qpc_frequency",qpc_frequency()},
        {"boundary_observed",frame_boundary_seen_.load()},{"present_calls",presents_.load()},
        {"missed_records",missed_frames_.load()},{"records",records}};
}
void RenderProbe::observe(const safetyhook::Context& context) noexcept {
    Record record{};
    record.sequence=calls_.fetch_add(1)+1;
    record.sdk_frame_hint=sdk_frame_.load(std::memory_order_relaxed);
    record.render_frame=frame_boundary_seen_.load()?presents_.load()+1:0;
    record.thread=GetCurrentThreadId();
    record.count=static_cast<uint32_t>(context.rdx);
    if(record.count>record.targets.size() ||
       !read_memory(context.rax+0x363FCF0,record.context) ||
       !read_memory(context.rbp+0xA8,record.depth) ||
       (record.count && !copy_memory(context.r8,record.targets.data(),record.count*sizeof(uintptr_t)))) {
        missed_.fetch_add(1);return;
    }
    gpu_.observe(reinterpret_cast<ID3D11DeviceContext*>(record.context),record.count,
                 record.targets.data(),record.sequence,record.sdk_frame_hint,record.render_frame,observation_session_);
    if(!TryAcquireSRWLockExclusive(&records_lock_)) {missed_.fetch_add(1);return;}
    records_[records_written_%records_.size()]=record;
    ++records_written_;
    ReleaseSRWLockExclusive(&records_lock_);
}
json RenderProbe::capture(const std::string& action) {
    std::lock_guard lock(control_);
    if(action=="arm" && !hook_.enabled()) throw std::runtime_error("Enable the render probe before arming capture");
    return gpu_.command(action);
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
            {"render_targets",std::vector<uintptr_t>(record.targets.begin(),record.targets.begin()+record.count)},
            {"depth_view",record.depth}});
    }
    return {{"active",(hook_.enabled()?1:0)+(label_hook_.enabled()?1:0)+(present_hook_.enabled()?1:0)},
        {"callbacks_in_flight",callbacks.load()},
        {"hooks",json::array({{{"name","dx11.omset_before_bind"},{"tier",1},{"rva",hook_rva},
            {"enabled",hook_.enabled()},{"calls",calls_.load()},{"missed_records",missed_.load()}},
            {{"name","rendergraph.image_name"},{"tier",1},{"rva",label_hook_rva},{"enabled",label_hook_.enabled()}},
            {{"name","dxgi.present_return"},{"tier",1},{"rva",present_hook_rva},{"enabled",present_hook_.enabled()},
             {"calls",presents_.load()}}})},
        {"last_error",last_error_},{"render_coherent",false},{"capture",gpu_.command("status")},
        {"recent_bindings",entries}};
}
}
