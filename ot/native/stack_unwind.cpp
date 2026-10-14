#include "stack_unwind.hpp"
#include <psapi.h>
#include <algorithm>
#include <stdexcept>

namespace ot {
namespace {
// UNWIND_CODE operations; winnt.h does not export them.
enum : unsigned {push_nonvol=0,alloc_large=1,alloc_small=2,set_fpreg=3,save_nonvol=4,save_nonvol_far=5,
                 epilog=6,spare_code=7,save_xmm128=8,save_xmm128_far=9,push_machframe=10};
constexpr unsigned chain_info=0x4; // UNW_FLAG_CHAININFO
constexpr unsigned rsp=4;

struct Walk {
    uint64_t r[16]{}; // rax rcx rdx rbx rsp rbp rsi rdi r8-r15
    uint64_t rip=0;
    uintptr_t low=0,high=0; // readable target stack [low,high)
    bool read(uint64_t address,uint64_t& value) const noexcept {
        if(address<low || high-low<sizeof(value) || address>high-sizeof(value)) return false;
        std::memcpy(&value,reinterpret_cast<const void*>(address),sizeof(value));
        return true;
    }
};
bool code_byte(const UnwindImage& image,uintptr_t address,uint8_t& value) noexcept {
    if(address<image.base || address>=image.end) return false;
    value=*reinterpret_cast<const uint8_t*>(address);
    return true;
}
unsigned slots(unsigned op,unsigned info) noexcept {
    switch(op) {
    case push_nonvol: case alloc_small: case set_fpreg: case push_machframe: return 1;
    case alloc_large: return info?3:2;
    case save_nonvol: case save_xmm128: case epilog: return 2;
    case save_nonvol_far: case save_xmm128_far: case spare_code: return 3;
    default: return 0;
    }
}
const RUNTIME_FUNCTION* lookup(const UnwindImage& image,uintptr_t address) noexcept {
    if(!image.count) return nullptr;
    const auto rva=static_cast<DWORD>(address-image.base);
    size_t low=0,high=image.count;
    while(low<high) {const auto mid=(low+high)/2; if(image.functions[mid].BeginAddress<=rva) low=mid+1; else high=mid;}
    if(!low || rva>=image.functions[low-1].EndAddress) return nullptr;
    return &image.functions[low-1];
}
// An entry whose UnwindData has bit 0 set names another RUNTIME_FUNCTION.
const RUNTIME_FUNCTION* resolve(const UnwindImage& image,const RUNTIME_FUNCTION* entry) noexcept {
    const uint64_t size=image.end-image.base;
    for(int i=0;i<4 && entry && (entry->UnwindData&1u);++i) {
        const uint64_t rva=entry->UnwindData&~1u;
        entry=rva+sizeof(RUNTIME_FUNCTION)<=size?reinterpret_cast<const RUNTIME_FUNCTION*>(image.base+rva):nullptr;
    }
    return entry && !(entry->UnwindData&1u)?entry:nullptr;
}
// RIP on the pops or return of an MSVC epilog (add/lea rsp; pop...; ret|jmp):
// the frame is already released, so replay the remaining pops instead of the
// prologue codes, as RtlVirtualUnwind does.
bool unwind_epilog(const UnwindImage& image,Walk& walk) noexcept {
    Walk next=walk;uintptr_t pc=walk.rip;bool popped=false;
    for(int i=0;i<16;++i) {
        uint8_t b0{},b1{};unsigned reg=0;
        if(!code_byte(image,pc,b0)) return false;
        if(b0>=0x58 && b0<=0x5F) {reg=b0-0x58u;pc+=1;}
        else if(b0==0x41 && code_byte(image,pc+1,b1) && b1>=0x58 && b1<=0x5F) {reg=8+b1-0x58u;pc+=2;}
        else break;
        uint64_t value{};
        if(reg==rsp || !next.read(next.r[rsp],value)) return false;
        next.r[reg]=value;next.r[rsp]+=8;popped=true;
    }
    uint8_t b0{},b1{},b2{};
    if(!code_byte(image,pc,b0)) return false;
    const bool ret=b0==0xC3 || b0==0xC2 || (b0==0xF3 && code_byte(image,pc+1,b1) && b1==0xC3);
    const bool jump=b0==0xE9 || (b0==0xFF && code_byte(image,pc+1,b1) && b1==0x25) ||
                    (b0==0x48 && code_byte(image,pc+1,b1) && b1==0xFF && code_byte(image,pc+2,b2) && b2==0x25);
    if(!ret && !(popped && jump)) return false;
    uint64_t rip{};
    if(!next.read(next.r[rsp],rip)) return false;
    next.r[rsp]+=8;next.rip=rip;walk=next;
    return true;
}
// Undo the prologue described by the unwind codes (and chained parents), then
// pop the return address. Codes after RIP's prologue offset have not run yet.
bool unwind_frame(const UnwindImage& image,const RUNTIME_FUNCTION* entry,Walk& walk) noexcept {
    Walk next=walk;bool primary=true,machine=false;
    const uint64_t size=image.end-image.base;
    for(int chain=0;chain<32;++chain) {
        entry=resolve(image,entry);
        if(!entry || uint64_t(entry->UnwindData)+4>size) return false;
        const auto* info=reinterpret_cast<const uint8_t*>(image.base+entry->UnwindData);
        const unsigned version=info[0]&7u,flags=info[0]>>3,prolog=info[1],count=info[2],
                       frame_reg=info[3]&0xFu,frame_offset=info[3]>>4u;
        if(version!=1 && version!=2) return false;
        const bool chained=(flags&chain_info)!=0;
        const uint64_t aligned=(count+1u)&~1u;
        if(uint64_t(entry->UnwindData)+4+2*aligned+(chained?sizeof(RUNTIME_FUNCTION):0)>size) return false;
        const auto* codes=reinterpret_cast<const uint16_t*>(info+4);
        // A chained parent's prologue has fully run.
        const uint64_t offset=primary?next.rip-(image.base+entry->BeginAddress):~uint64_t{0};
        const bool in_prolog=offset<prolog;
        const auto executed=[&](unsigned i) {return !in_prolog || (codes[i]&0xFFu)<=offset;};
        uint64_t frame=next.r[rsp];
        if(frame_reg) {
            bool set=!in_prolog;
            for(unsigned i=0;in_prolog && i<count;) {
                const unsigned op=(codes[i]>>8)&0xFu,n=slots(op,codes[i]>>12);
                if(!n) return false;
                if(op==set_fpreg) {set=executed(i);break;}
                i+=n;
            }
            if(set) frame=next.r[frame_reg]-16ull*frame_offset;
        }
        for(unsigned i=0;i<count && !machine;) {
            const unsigned op=(codes[i]>>8)&0xFu,op_info=codes[i]>>12,n=slots(op,op_info);
            if(!n || i+n>count) return false;
            if(executed(i)) {
                uint64_t value{};
                switch(op) {
                case push_nonvol:
                    if(op_info==rsp || !next.read(next.r[rsp],value)) return false;
                    next.r[op_info]=value;next.r[rsp]+=8;break;
                case alloc_large:
                    next.r[rsp]+=op_info?(uint64_t(codes[i+1])|uint64_t(codes[i+2])<<16):uint64_t(codes[i+1])*8;break;
                case alloc_small: next.r[rsp]+=uint64_t(op_info)*8+8;break;
                case set_fpreg:
                    if(!frame_reg) return false;
                    next.r[rsp]=next.r[frame_reg]-16ull*frame_offset;break;
                case save_nonvol:
                    if(op_info==rsp || !next.read(frame+uint64_t(codes[i+1])*8,value)) return false;
                    next.r[op_info]=value;break;
                case save_nonvol_far:
                    if(op_info==rsp || !next.read(frame+(uint64_t(codes[i+1])|uint64_t(codes[i+2])<<16),value)) return false;
                    next.r[op_info]=value;break;
                case push_machframe: {
                    const uint64_t base=next.r[rsp]+(op_info?8:0);uint64_t rip{},stack{};
                    if(!next.read(base,rip) || !next.read(base+24,stack)) return false;
                    next.rip=rip;next.r[rsp]=stack;machine=true;break;
                }
                default: break; // XMM saves and v2 epilog markers do not affect frames.
                }
            }
            i+=n;
        }
        if(machine || !chained) break;
        entry=reinterpret_cast<const RUNTIME_FUNCTION*>(codes+aligned);
        primary=false;
    }
    if(!machine) {
        uint64_t rip{};
        if(!next.read(next.r[rsp],rip)) return false;
        next.r[rsp]+=8;next.rip=rip;
    }
    walk=next;
    return true;
}
bool unwind_leaf(Walk& walk) noexcept {
    uint64_t rip{};
    if(!walk.read(walk.r[rsp],rip)) return false;
    walk.r[rsp]+=8;walk.rip=rip;
    return true;
}
std::string utf8(const wchar_t* text) {
    const int bytes=WideCharToMultiByte(CP_UTF8,0,text,-1,nullptr,0,nullptr,nullptr);
    if(bytes<=1) return {};
    std::string result(static_cast<size_t>(bytes),'\0');
    WideCharToMultiByte(CP_UTF8,0,text,-1,result.data(),bytes,nullptr,nullptr);
    result.resize(static_cast<size_t>(bytes-1));
    return result;
}
}

UnwindTable::UnwindTable(UnwindTable&& other) noexcept
    :images_(std::move(other.images_)),pins_(std::move(other.pins_)) {other.pins_.clear();}
UnwindTable& UnwindTable::operator=(UnwindTable&& other) noexcept {
    if(this!=&other) {
        release();
        images_=std::move(other.images_);pins_=std::move(other.pins_);other.pins_.clear();
    }
    return *this;
}
void UnwindTable::release() noexcept {
    for(auto pin:pins_) FreeLibrary(pin);
    pins_.clear();
}
void UnwindTable::build() {
    release();
    images_.clear();
    std::vector<HMODULE> modules(512);DWORD needed=0;
    for(;;) {
        const auto bytes=static_cast<DWORD>(modules.size()*sizeof(HMODULE));
        if(!EnumProcessModulesEx(GetCurrentProcess(),modules.data(),bytes,&needed,LIST_MODULES_64BIT))
            throw std::runtime_error("EnumProcessModulesEx failed: "+std::to_string(GetLastError()));
        if(needed<=bytes) break;
        modules.resize(needed/sizeof(HMODULE)+32);
    }
    modules.resize(needed/sizeof(HMODULE));
    // Our own module is not referenced: it cannot unload while the sampler runs
    // (the core drains it first), and a last FreeLibrary from here would unmap
    // the code that is returning.
    HMODULE self{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&stack_end_name),&self);
    pins_.reserve(modules.size());
    for(auto handle:modules) {
        if(handle!=self) {
            // A module unloaded since the listing is skipped; one loaded at the
            // same address since then is read under its own reference.
            HMODULE pinned{};
            if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(handle),&pinned)) continue;
            pins_.push_back(pinned);
            handle=pinned;
        }
        MODULEINFO info{};wchar_t path[1024]{};
        if(!GetModuleInformation(GetCurrentProcess(),handle,&info,sizeof(info))) continue;
        UnwindImage image;
        image.base=reinterpret_cast<uintptr_t>(info.lpBaseOfDll);image.end=image.base+info.SizeOfImage;
        if(GetModuleFileNameW(handle,path,static_cast<DWORD>(std::size(path)))) {
            image.path=utf8(path);
            const auto slash=image.path.find_last_of("\\/");
            image.name=slash==std::string::npos?image.path:image.path.substr(slash+1);
        }
        const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(image.base);
        if(dos->e_magic==IMAGE_DOS_SIGNATURE && dos->e_lfanew>0 &&
           uint64_t(dos->e_lfanew)+sizeof(IMAGE_NT_HEADERS64)<=info.SizeOfImage) {
            const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(image.base+dos->e_lfanew);
            if(nt->Signature==IMAGE_NT_SIGNATURE && nt->OptionalHeader.Magic==IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
               nt->OptionalHeader.NumberOfRvaAndSizes>IMAGE_DIRECTORY_ENTRY_EXCEPTION) {
                const auto& directory=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
                if(directory.VirtualAddress && uint64_t(directory.VirtualAddress)+directory.Size<=info.SizeOfImage) {
                    image.functions=reinterpret_cast<const RUNTIME_FUNCTION*>(image.base+directory.VirtualAddress);
                    image.count=directory.Size/sizeof(RUNTIME_FUNCTION);
                    // Touch .pdata and every UNWIND_INFO header now; a first-touch
                    // page fault while the target is suspended would stretch the pause.
                    volatile uint8_t sink=0;
                    for(size_t i=0;i<image.count;++i) {
                        const auto unwind=image.functions[i].UnwindData&~1u;
                        if(unwind && unwind<info.SizeOfImage) sink=static_cast<uint8_t>(sink^*reinterpret_cast<const uint8_t*>(image.base+unwind));
                    }
                }
            }
        }
        images_.push_back(std::move(image));
    }
    std::sort(images_.begin(),images_.end(),[](const UnwindImage& a,const UnwindImage& b){return a.base<b.base;});
}
const UnwindImage* UnwindTable::find(uintptr_t address) const noexcept {
    const auto next=std::upper_bound(images_.begin(),images_.end(),address,
        [](uintptr_t value,const UnwindImage& image){return value<image.base;});
    if(next==images_.begin() || address>=std::prev(next)->end) return nullptr;
    return &*std::prev(next);
}
const char* stack_end_name(StackEnd end) noexcept {
    switch(end) {
    case StackEnd::top: return "top";
    case StackEnd::depth: return "depth";
    case StackEnd::unknown_code: return "unknown_code";
    case StackEnd::bad_frame: return "bad_frame";
    default: return "?";
    }
}
const NT_TIB* thread_tib(HANDLE thread) {
    struct Basic {LONG exit_status;PVOID teb;HANDLE process,thread;KAFFINITY affinity;LONG priority,base_priority;};
    using Query=LONG(NTAPI*)(HANDLE,ULONG,PVOID,ULONG,PULONG);
    const auto query=reinterpret_cast<Query>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtQueryInformationThread")));
    if(!query) throw std::runtime_error("NtQueryInformationThread is unavailable");
    Basic basic{};
    const LONG status=query(thread,0,&basic,sizeof(basic),nullptr); // ThreadBasicInformation
    if(status<0 || !basic.teb) throw std::runtime_error("NtQueryInformationThread failed: "+std::to_string(status));
    return static_cast<const NT_TIB*>(basic.teb);
}
bool unwind_one(const UnwindImage& image,CONTEXT& context,uintptr_t stack_low,uintptr_t stack_high,bool sampled) noexcept {
    Walk walk;
    DWORD64* registers[16]{&context.Rax,&context.Rcx,&context.Rdx,&context.Rbx,&context.Rsp,&context.Rbp,&context.Rsi,&context.Rdi,
                           &context.R8,&context.R9,&context.R10,&context.R11,&context.R12,&context.R13,&context.R14,&context.R15};
    for(int i=0;i<16;++i) walk.r[i]=*registers[i];
    walk.rip=context.Rip;walk.low=stack_low;walk.high=stack_high;
    bool ok=false;
    if(context.Rip>=image.base && context.Rip<image.end) {
        if(unwind_epilog(image,walk)) ok=true;
        else if(const auto* entry=lookup(image,walk.rip)) ok=unwind_frame(image,entry,walk);
        else ok=sampled && unwind_leaf(walk);
    }
    if(!ok) return false;
    for(int i=0;i<16;++i) *registers[i]=walk.r[i];
    context.Rip=walk.rip;
    return true;
}
size_t walk_stack(const UnwindTable& table,const CONTEXT& context,const NT_TIB* tib,
                  uint64_t* frames,size_t depth,StackEnd& end) noexcept {
    end=StackEnd::depth;
    if(!depth) return 0;
    frames[0]=context.Rip;size_t count=1;
    const auto base=reinterpret_cast<uintptr_t>(tib?static_cast<void*>(tib->StackBase):nullptr);
    const auto limit=reinterpret_cast<uintptr_t>(tib?static_cast<void*>(tib->StackLimit):nullptr);
    if(!tib || context.Rsp<limit || context.Rsp>=base) {end=StackEnd::bad_frame;return count;}
    Walk walk;
    const DWORD64 registers[16]{context.Rax,context.Rcx,context.Rdx,context.Rbx,context.Rsp,context.Rbp,context.Rsi,context.Rdi,
                                context.R8,context.R9,context.R10,context.R11,context.R12,context.R13,context.R14,context.R15};
    std::copy(std::begin(registers),std::end(registers),walk.r);
    walk.rip=context.Rip;walk.low=context.Rsp;walk.high=base;
    while(count<depth) {
        const auto* image=table.find(walk.rip);
        const uint64_t before=walk.r[rsp];
        bool ok=false;
        if(!image) {
            // A sampled RIP may be in a hook trampoline; a return address there
            // cannot be unwound without unwind data.
            if(count!=1) {end=StackEnd::unknown_code;return count;}
            ok=unwind_leaf(walk);
        } else if(unwind_epilog(*image,walk)) ok=true;
        else if(const auto* entry=lookup(*image,walk.rip)) ok=unwind_frame(*image,entry,walk);
        else ok=count==1 && unwind_leaf(walk); // Only the sampled frame can be a leaf function.
        if(!ok) {end=StackEnd::bad_frame;return count;}
        if(!walk.rip || walk.r[rsp]>=base) {end=StackEnd::top;return count;}
        if(walk.r[rsp]<=before) {end=StackEnd::bad_frame;return count;}
        frames[count++]=walk.rip;
    }
    return count;
}
}
