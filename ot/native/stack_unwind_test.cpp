// Game-free check of unwind_one against RtlVirtualUnwind on real images: every
// .pdata entry (chained parts included) at its body start and at each prologue
// offset, on a synthetic stack whose slots hold distinct values. Usage:
//   ot_unwind_test [image ...]   (default: the game executable and system DLLs)
#include "stack_unwind.hpp"
#include <cstdio>
#include <cstdlib>

namespace {
// RtlVirtualUnwind may fault on a frame outside the synthetic stack.
bool system_unwind(DWORD64 base,DWORD64 pc,PRUNTIME_FUNCTION entry,CONTEXT* context) {
    __try {
        void* handler{};DWORD64 establisher{};
        RtlVirtualUnwind(UNW_FLAG_NHANDLER,base,pc,entry,context,&handler,&establisher,nullptr);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
struct Tally {uint64_t cases=0,same=0,both_failed=0,only_ours_failed=0,only_system_failed=0,different=0,jump_chunks=0;};
const char* const names[]{"rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi","r8","r9","r10","r11","r12","r13","r14","r15"};
DWORD64 reg(const CONTEXT& c,int i) {
    const DWORD64 values[16]{c.Rax,c.Rcx,c.Rdx,c.Rbx,c.Rsp,c.Rbp,c.Rsi,c.Rdi,c.R8,c.R9,c.R10,c.R11,c.R12,c.R13,c.R14,c.R15};
    return values[i];
}
void set_reg(CONTEXT& c,int i,DWORD64 v) {
    DWORD64* values[16]{&c.Rax,&c.Rcx,&c.Rdx,&c.Rbx,&c.Rsp,&c.Rbp,&c.Rsi,&c.Rdi,&c.R8,&c.R9,&c.R10,&c.R11,&c.R12,&c.R13,&c.R14,&c.R15};
    *values[i]=v;
}
Tally check_image(const wchar_t* path,uint8_t* stack,size_t stack_bytes,int& reported) {
    Tally tally;
    auto* module=LoadLibraryExW(path,nullptr,LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if(!module) {std::printf("SKIP %ls (LoadLibraryEx %lu)\n",path,GetLastError());return tally;}
    const auto base=reinterpret_cast<uintptr_t>(module)&~uintptr_t{3};
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
    const auto& directory=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    ot::UnwindImage image;
    image.base=base;image.end=base+nt->OptionalHeader.SizeOfImage;
    image.functions=reinterpret_cast<const RUNTIME_FUNCTION*>(base+directory.VirtualAddress);
    image.count=directory.Size/sizeof(RUNTIME_FUNCTION);
    const auto low=reinterpret_cast<uintptr_t>(stack),high=low+stack_bytes;
    for(size_t f=0;f<image.count;++f) {
        auto* entry=const_cast<RUNTIME_FUNCTION*>(&image.functions[f]);
        if(!entry->BeginAddress || (entry->UnwindData&1u)) continue;
        const auto* info=reinterpret_cast<const uint8_t*>(base+entry->UnwindData);
        const unsigned prolog=info[1],count=info[2],frame_reg=info[3]&0xFu,frame_offset=info[3]>>4u;
        const auto* codes=reinterpret_cast<const uint16_t*>(info+4);
        // Body start, function start, and the end of each prologue instruction.
        uint32_t offsets[260];size_t n=0;
        offsets[n++]=prolog;
        if(prolog) offsets[n++]=0;
        for(unsigned i=0;i<count && n<std::size(offsets);) {
            const unsigned op=(codes[i]>>8)&0xFu,offset=codes[i]&0xFFu;
            if(op!=6 && offset && offset<prolog) offsets[n++]=offset; // 6: v2 epilog marker, not an offset
            const unsigned slots=op==1?((codes[i]>>12)?3u:2u):(op==4 || op==8 || op==6)?2u:(op==5 || op==9 || op==7)?3u:1u;
            i+=slots;
        }
        for(size_t k=0;k<n;++k) {
            const DWORD64 pc=base+entry->BeginAddress+offsets[k];
            if(pc>=base+entry->EndAddress) continue;
            CONTEXT start{};
            for(int i=0;i<16;++i) set_reg(start,i,0x1111000000000000ull+0x0101010101ull*static_cast<unsigned>(i));
            start.Rsp=low+4096;
            if(frame_reg) set_reg(start,static_cast<int>(frame_reg),start.Rsp+16ull*frame_offset);
            start.Rip=pc;start.ContextFlags=CONTEXT_CONTROL|CONTEXT_INTEGER;
            CONTEXT ours=start,system=start;
            const bool ours_ok=ot::unwind_one(image,ours,low,high,false);
            const bool system_ok=system_unwind(base,pc,entry,&system);
            ++tally.cases;
            if(!ours_ok && !system_ok) {++tally.both_failed;continue;}
            if(!ours_ok) {++tally.only_ours_failed;}
            else if(!system_ok) {++tally.only_system_failed;}
            else {
                bool same=ours.Rip==system.Rip;
                for(int i=0;i<16;++i) same=same && reg(ours,i)==reg(system,i);
                if(same) {++tally.same;continue;}
                // A chained part that starts with jmp rel32 (a PGO cold fragment jumping
                // back): RtlVirtualUnwind takes the jmp for a tail-call epilogue and pops
                // only the return address; we undo the parent's prologue, which is right
                // while the frame is live. Only an asynchronous sample can land there.
                if(*reinterpret_cast<const uint8_t*>(pc)==0xE9 && ((info[0]>>3)&0x4u) && system.Rsp==start.Rsp+8) {
                    ++tally.jump_chunks;continue;
                }
                ++tally.different;
            }
            if(reported<12) {
                ++reported;
                std::printf("  %ls entry 0x%X..0x%X +0x%X prolog %u codes %u fp %u/%u: ours %s system %s\n",path,
                            entry->BeginAddress,entry->EndAddress,offsets[k],prolog,count,frame_reg,frame_offset,
                            ours_ok?"ok":"failed",system_ok?"ok":"failed");
                if(ours_ok && system_ok) {
                    if(ours.Rip!=system.Rip) std::printf("    rip %016llX %016llX\n",ours.Rip,system.Rip);
                    for(int i=0;i<16;++i) if(reg(ours,i)!=reg(system,i))
                        std::printf("    %s %016llX %016llX\n",names[i],reg(ours,i),reg(system,i));
                }
            }
        }
    }
    FreeLibrary(module);
    std::printf("%ls: %llu functions, %llu cases, same %llu, jmp chunks %llu, both failed %llu, only ours failed %llu, only system failed %llu, different %llu\n",
                path,static_cast<unsigned long long>(image.count),static_cast<unsigned long long>(tally.cases),
                static_cast<unsigned long long>(tally.same),static_cast<unsigned long long>(tally.jump_chunks),
                static_cast<unsigned long long>(tally.both_failed),
                static_cast<unsigned long long>(tally.only_ours_failed),static_cast<unsigned long long>(tally.only_system_failed),
                static_cast<unsigned long long>(tally.different));
    return tally;
}
}
int wmain(int argc,wchar_t** argv) {
    std::vector<std::wstring> paths;
    for(int i=1;i<argc;++i) paths.emplace_back(argv[i]);
    if(paths.empty()) {
        wchar_t directory[1024]{};
        if(!GetEnvironmentVariableW(L"ETS2_DIR",directory,static_cast<DWORD>(std::size(directory))))
            wcscpy_s(directory,L"D:\\SteamLibrary\\steamapps\\common\\Euro Truck Simulator 2");
        paths.push_back(std::wstring(directory)+L"\\bin\\win_x64\\eurotrucks2.exe");
        for(const auto* name:{L"ntdll.dll",L"kernel32.dll",L"KernelBase.dll",L"d3d11.dll"})
            paths.push_back(std::wstring(L"C:\\Windows\\System32\\")+name);
    }
    // Slots hold distinct values so a wrong offset shows up as a wrong register.
    const size_t bytes=size_t{64}<<20;
    auto* stack=static_cast<uint8_t*>(VirtualAlloc(nullptr,bytes,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    if(!stack) return 2;
    for(size_t i=0;i<bytes/8;++i) reinterpret_cast<uint64_t*>(stack)[i]=0xA5A5000000000000ull|(i*8);
    Tally total;int reported=0;
    for(const auto& path:paths) {
        const auto t=check_image(path.c_str(),stack,bytes,reported);
        total.cases+=t.cases;total.same+=t.same;total.both_failed+=t.both_failed;
        total.only_ours_failed+=t.only_ours_failed;total.only_system_failed+=t.only_system_failed;total.different+=t.different;
        total.jump_chunks+=t.jump_chunks;
    }
    const bool pass=total.cases>0 && !total.different && !total.only_ours_failed && !total.only_system_failed;
    std::printf("%s %llu cases agree with RtlVirtualUnwind, %llu differ only at chained jmp chunks, %llu both failed outside the synthetic stack\n",
                pass?"PASS":"FAIL",static_cast<unsigned long long>(total.same),static_cast<unsigned long long>(total.jump_chunks),
                static_cast<unsigned long long>(total.both_failed));
    return pass?0:1;
}
