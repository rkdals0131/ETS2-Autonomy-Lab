#include "render_probe.hpp"
#include <tlhelp32.h>
#include <algorithm>
#include <bit>
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
struct CodeRanges { std::array<std::pair<uintptr_t,uintptr_t>,9> ranges; };
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
void RenderProbe::enable(bool vehicle_metadata) {
    std::lock_guard lock(control_);
    if(hook_.enabled()) {vehicle_metadata_=vehicle_metadata;return;}
    if(GetModuleHandleW(L"renderdoc.dll"))
        throw std::runtime_error("RenderDoc is loaded; restart ETS2 normally before enabling ot render hooks");
    if(!hook_) {
        if(observer.load() && observer.load()!=this)
            throw std::runtime_error("A previous render observer is retained; restart ETS2");
        const auto target=find_target(signature,15,hook_rva);
        const auto present_target=find_target(present_signature,0,present_hook_rva);
        const auto compile_begin_target=find_target(compile_begin_signature,0,compile_begin_rva);
        const auto compile_end_target=find_target(compile_end_signature,0,compile_end_rva);
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
    pass_commands_.clear();
    vehicle_metadata_=vehicle_metadata;
    accepting_=true;
    for(auto* hook:hookset()) {
        if(hook->enable()) continue;
        accepting_=false;
        vehicle_metadata_=false;
        for(auto* rollback:hookset())
            if(rollback->enabled() && !rollback->disable()) last_error_="Render hook rollback failed";
        throw std::runtime_error("Render observer enable failed");
    }
    last_error_.clear();
    log("Render probe enabled at OMSetRenderTargets argument preparation");
}
void RenderProbe::disable() noexcept {
    try {
        std::lock_guard lock(control_);
        accepting_=false;
        vehicle_metadata_=false;
        frame_boundary_seen_=false;
        bool changed=false;
        for(auto* hook:hookset()) if(hook->enabled()) {
            changed=true;
            if(!hook->disable()) last_error_="Render observer disable failed";
        }
        if(changed) log("Render probe disable requested for all hook sites");
        for(auto* camera:cameras()) camera->cancel();
    } catch(...) { accepting_=false; }
}
bool RenderProbe::quiescent() noexcept {
    if(callbacks.load()) return false;
    CodeRanges ranges{};ranges.ranges[0]={module_begin_,module_end_};
    size_t range=1;
    for(const auto* hook:hookset()) {
        ranges.ranges[range++]={hook->stub().address(),hook->stub().address()+hook->stub().size()};
        ranges.ranges[range++]={hook->trampoline().address(),hook->trampoline().address()+hook->trampoline().size()};
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
    if(auto* self=observer.load();self && self->accepting_.load()) {
        const auto start=qpc_now();
        self->observe(context);
        self->bind_timing_.add(qpc_now()-start);
    }
    callbacks.fetch_sub(1);
}
void RenderProbe::present_callback(safetyhook::Context& context) noexcept {
    callbacks.fetch_add(1);
    if(auto* self=observer.load();self && self->accepting_.load()) {
        const auto start=qpc_now();
        self->present(static_cast<HRESULT>(context.rax));
        self->present_timing_.add(qpc_now()-start);
    }
    callbacks.fetch_sub(1);
}
void RenderProbe::compile_begin_callback(safetyhook::Context& context) noexcept {
    callbacks.fetch_add(1);
    if(auto* self=observer.load();self && self->accepting_.load()) {
        const auto start=qpc_now();
        uintptr_t input{},output{};uint16_t id{};
        if(read_memory(context.r10,input) && read_memory(context.rsp+0x58,output) &&
           read_memory(context.rbp+0x240,id))
            self->pass_commands_.begin(context.rbp,input,output,id,self->vehicle_metadata_.load());
        self->compile_begin_timing_.add(qpc_now()-start);
    }
    callbacks.fetch_sub(1);
}
void RenderProbe::compile_end_callback(safetyhook::Context& context) noexcept {
    callbacks.fetch_add(1);
    if(auto* self=observer.load();self && self->accepting_.load()) {
        const auto start=qpc_now();
        self->pass_commands_.end(context.rbp);
        self->compile_end_timing_.add(qpc_now()-start);
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
    uintptr_t cursor{},data{};
    const auto index=static_cast<uint32_t>(context.r13);
    if(index && read_memory(context.rbp-0x30,cursor) && read_memory(cursor,record.compiled_id) &&
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
    for(auto* camera:cameras())
        camera->observe(reinterpret_cast<ID3D11DeviceContext*>(record.context),record.count,
            record.targets.data(),record.sequence,record.sdk_frame_hint,record.render_frame,observation_session_,record.pass.get());
    if(!TryAcquireSRWLockExclusive(&records_lock_)) {missed_.fetch_add(1);return;}
    records_[records_written_%records_.size()]=record;
    ++records_written_;
    ReleaseSRWLockExclusive(&records_lock_);
}
json RenderProbe::capture(const std::string& action) {
    std::lock_guard lock(control_);
    if(action=="arm" && !hook_.enabled()) throw std::runtime_error("Enable the render probe before arming capture");
    if(action=="arm") {
        for(auto* camera:cameras()) {
            const auto phase=camera->command("status").at("phase");
            if(phase=="armed" || phase=="waiting_gpu") throw std::runtime_error("A camera capture is already pending");
        }
        bundle_frame_=0;
    }
    return gpu_.command(action);
}
json RenderProbe::capture_views(const std::string& action) {
    std::lock_guard lock(control_);
    if(action=="arm") {
        if(!hook_.enabled()) throw std::runtime_error("Enable the render probe before arming capture");
        for(auto* camera:cameras()) {
            const auto phase=camera->command("status").at("phase");
            if(phase=="armed" || phase=="waiting_gpu") throw std::runtime_error("A camera capture is already pending");
        }
        // Skip the interval already in progress so every requested view has a
        // chance to render after all four requests have been armed.
        bundle_frame_=presents_.load()+2;
        try {for(auto* camera:cameras()) camera->command("arm",bundle_frame_);}
        catch(...) {for(auto* camera:cameras()) camera->cancel();bundle_frame_=0;throw;}
    } else if(action=="cancel") {
        for(auto* camera:cameras()) camera->cancel();
        bundle_frame_=0;
    } else if(action!="status" && action!="save") throw std::runtime_error("Unknown camera bundle action");
    json views=json::array();bool ready=true,error=false,pending=false;
    for(auto* camera:cameras()) {
        auto state=camera->command("status");const auto phase=state.at("phase");
        ready=ready && phase=="ready";error=error || phase=="error";
        pending=pending || phase=="armed" || phase=="waiting_gpu";
        views.push_back(std::move(state));
    }
    if(action=="save") {
        if(!bundle_frame_ || !ready) throw std::runtime_error("No complete camera bundle to save");
        views=json::array();for(auto* camera:cameras()) views.push_back(camera->command("save"));
    }
    return {{"phase",!bundle_frame_?"idle":error?"error":ready?"ready":pending?"pending":"idle"},
        {"render_frame_id",bundle_frame_},{"observation_session_qpc",observation_session_},{"views",views}};
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
        {"vehicle_metadata_enabled",vehicle_metadata_.load()},
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
             {"timing",compile_end_timing_.snapshot()}}})},
        {"last_error",last_error_},{"render_coherent",false},{"capture",gpu_.command("status")},
        {"recent_bindings",entries}};
}
}
