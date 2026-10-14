#include "hook_support.hpp"
#include <tlhelp32.h>
#include <algorithm>
#include <stdexcept>

namespace ot {
namespace {
bool contains(const CodeRanges& ranges,DWORD64 ip) noexcept {
    for(const auto& [begin,end]:ranges) if(ip>=begin && ip<end) return true;
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

bool threads_outside(const CodeRanges& ranges) noexcept {
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
    return GetLastError()==ERROR_NO_MORE_FILES;
}
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
}
