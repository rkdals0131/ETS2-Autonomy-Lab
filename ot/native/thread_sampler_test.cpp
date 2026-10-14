// Game-free check of ThreadSampler: attribution to the right module, stacks
// that match the system unwinder, and no deadlock while the target thread
// holds the process heap lock or the loader lock most of the time.
#include "thread_sampler.hpp"
#include <malloc.h>
#include <cstdio>
#include <cstdlib>

using ot::json;
namespace {
std::atomic<bool> stop{false};
std::atomic<uint64_t> sink{0};
int check(bool ok,const char* what) {std::printf("%s %s\n",ok?"PASS":"FAIL",what);return ok?0:1;}
// Incremental linking turns function addresses into jump thunks.
uintptr_t code_address(const void* function) {
    auto address=reinterpret_cast<uintptr_t>(function);
    if(*reinterpret_cast<const uint8_t*>(address)==0xE9) address+=5+*reinterpret_cast<const int32_t*>(address+1);
    return address;
}
uintptr_t function_begin(uintptr_t address) {
    DWORD64 base{};
    const auto* entry=RtlLookupFunctionEntry(address,&base,nullptr);
    return entry?static_cast<uintptr_t>(base+entry->BeginAddress):0;
}

// Oracle: walking a context captured in this thread must give the same
// return addresses as RtlCaptureStackBackTrace. The callers below make MSVC
// emit frame pointers (alloca), __chkstk with a large allocation, XMM saves
// and many pushed registers.
ot::UnwindTable oracle_table;
std::string oracle_detail;
bool oracle_ok=false;
__declspec(noinline) int oracle_check(int n) {
    CONTEXT context{};RtlCaptureContext(&context);
    void* trace[64]{};
    const auto traced=RtlCaptureStackBackTrace(0,64,trace,nullptr);
    uint64_t frames[64]{};ot::StackEnd end{};
    const auto walked=ot::walk_stack(oracle_table,context,reinterpret_cast<const NT_TIB*>(NtCurrentTeb()),frames,64,end);
    // Index 0 differs: both are points inside this function.
    oracle_ok=walked==traced && end==ot::StackEnd::top;
    char line[96];
    std::snprintf(line,sizeof(line),"walked %zu (%s), traced %u\n",walked,ot::stack_end_name(end),static_cast<unsigned>(traced));
    oracle_detail=line;
    for(size_t i=1;i<std::max<size_t>(walked,traced) && i<64;++i) {
        const auto ours=i<walked?frames[i]:0,system=i<traced?reinterpret_cast<uint64_t>(trace[i]):0;
        oracle_ok=oracle_ok && ours==system;
        std::snprintf(line,sizeof(line),"  %2zu %016llX %016llX%s\n",i,static_cast<unsigned long long>(ours),
                      static_cast<unsigned long long>(system),ours==system?"":" <-");
        oracle_detail+=line;
    }
    return n+static_cast<int>(walked);
}
__declspec(noinline) int with_alloca(int n) {
    auto* p=static_cast<volatile char*>(_alloca(static_cast<size_t>(n)+64));
    p[0]=static_cast<char>(n);
    const int r=oracle_check(n+1);
    return r+p[0];
}
__declspec(noinline) int with_large_frame(int n) {
    volatile char buffer[20000];
    buffer[n%100]=static_cast<char>(n);
    const int r=with_alloca(n+1);
    return r+buffer[n%100];
}
__declspec(noinline) double with_xmm(double x) {
    const double a=x*1.1,b=x*2.3,c=x*3.7,d=x*5.9,e=x*7.1,f=x*11.3;
    const int r=with_large_frame(static_cast<int>(x));
    return a*b+c*d+e*f+r;
}
__declspec(noinline) uint64_t with_registers(uint64_t x) {
    const uint64_t a=x*3,b=x*5,c=x*7,d=x*11,e=x*13,f=x*17,g=x*19;
    const double r=with_xmm(static_cast<double>(x%7));
    return a^b^c^d^e^f^g^static_cast<uint64_t>(r);
}

// Sampled target: many samples land in tiny()'s prologue and epilogue.
__declspec(noinline) uint64_t tiny_leaf(uint64_t x) {return x*6364136223846793005ULL+1;}
__declspec(noinline) uint64_t tiny(uint64_t x) {
    const uint64_t a=tiny_leaf(x),b=tiny_leaf(a^x);
    return a+b;
}
__declspec(noinline) void sampled_middle() {
    uint64_t x=1;
    while(!stop.load(std::memory_order_relaxed)) for(int i=0;i<1000;++i) x=tiny(x);
    sink+=x;
}
__declspec(noinline) void sampled_outer() {sampled_middle();sink+=1;}

__declspec(noinline) void spin() {
    uint64_t x=1;
    while(!stop.load(std::memory_order_relaxed)) for(int i=0;i<100000;++i) x=x*6364136223846793005ULL+1;
    sink+=x;
}
__declspec(noinline) void allocate() {
    while(!stop.load(std::memory_order_relaxed)) {
        std::vector<void*> blocks;
        for(int i=0;i<256;++i) blocks.push_back(std::malloc(64+i*16));
        for(auto* p:blocks) std::free(p);
    }
}
std::atomic<uint64_t> loads{0};
__declspec(noinline) void load_libraries() {
    while(!stop.load(std::memory_order_relaxed)) {
        if(auto* library=LoadLibraryW(L"msimg32.dll")) {FreeLibrary(library);++loads;}
    }
}
struct Run {json status,stacks;};
Run run(void(*work)(),double hz,double seconds,unsigned depth,size_t page_bytes=700000) {
    stop=false;DWORD id=0;
    std::thread target([&]{id=GetCurrentThreadId();work();});
    while(!id) Sleep(1);
    ot::ThreadSampler sampler;sampler.start(id,hz,seconds,depth);
    const auto deadline=GetTickCount64()+static_cast<uint64_t>(seconds*1000)+5000;
    json status=sampler.status();
    while(status.at("running").get<bool>() && GetTickCount64()<deadline) {Sleep(50);status=sampler.status();}
    stop=true;target.join();
    Run result{status,json::array()};
    if(depth && !status.at("running").get<bool>()) {
        json page=sampler.stacks(0,page_bytes);
        result.stacks=json{{"images",page.at("images")},{"pages",1},{"stacks",page.at("stacks")}};
        while(!page.at("next").is_null()) {
            page=sampler.stacks(page.at("next").get<size_t>(),page_bytes);
            for(auto& entry:page.at("stacks")) result.stacks["stacks"].push_back(entry);
            result.stacks["pages"]=result.stacks["pages"].get<int>()+1;
        }
    }
    return result;
}
}
int main() {
    int failures=0;
    // The table holds a reference to each module so none unmaps during a run.
    {
        auto* library=LoadLibraryW(L"msimg32.dll");
        ot::UnwindTable table;table.build();
        FreeLibrary(library);
        const bool held=GetModuleHandleW(L"msimg32.dll")!=nullptr;
        table.release();
        failures+=check(library && held && !GetModuleHandleW(L"msimg32.dll"),"unwind table keeps modules mapped until release");
    }
    oracle_table.build();
    std::thread([]{sink+=with_registers(5);}).join();
    std::printf("%s",oracle_detail.c_str());
    failures+=check(oracle_ok,"stack walk matches RtlCaptureStackBackTrace through alloca, __chkstk, XMM and pushed frames");

    const auto spinning=run(spin,1000,2,0).status;
    std::printf("%s\n",spinning.dump(1).substr(0,900).c_str());
    const auto& modules=spinning.at("modules");
    failures+=check(!spinning.at("running").get<bool>(),"spin run finished");
    failures+=check(spinning.at("samples").get<size_t>()>1000,"spin run collected samples near 1 kHz");
    failures+=check(!modules.empty() && modules[0].at("module")=="ot_sampler_test.exe" && modules[0].at("share").get<double>()>0.9,
                    "busy thread attributed to its own image");

    const auto sampled=run(sampled_outer,1000,2,48,300);
    std::printf("%s\n",sampled.status.dump(1).substr(0,1400).c_str());
    const auto& images=sampled.stacks.at("images");
    size_t self_image=SIZE_MAX;
    for(size_t i=0;i<images.size();++i) if(images[i].at("name")=="ot_sampler_test.exe") self_image=i;
    const auto self_base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto middle=code_address(reinterpret_cast<const void*>(&sampled_middle));
    const auto outer=code_address(reinterpret_cast<const void*>(&sampled_outer));
    uint64_t total=0,top=0,through=0;
    for(const auto& entry:sampled.stacks.at("stacks")) {
        const auto count=entry[0].get<uint64_t>();
        total+=count;
        if(entry[1]=="top") top+=count;
        bool saw_middle=false,saw_outer=false;
        for(size_t i=2;i+1<entry.size();i+=2) {
            if(entry[i].get<int64_t>()!=static_cast<int64_t>(self_image)) continue;
            const auto begin=function_begin(self_base+entry[i+1].get<uintptr_t>()-(i>2?1:0));
            saw_middle=saw_middle || begin==middle;saw_outer=saw_outer || begin==outer;
        }
        if(saw_middle && saw_outer) through+=count;
    }
    std::printf("stacks %llu in %d pages, top %llu, through middle and outer %llu\n",static_cast<unsigned long long>(total),
                sampled.stacks.at("pages").get<int>(),static_cast<unsigned long long>(top),static_cast<unsigned long long>(through));
    failures+=check(total==sampled.status.at("samples").get<uint64_t>() && sampled.stacks.at("pages").get<int>()>1,
                    "paged stacks add up to the sample count");
    failures+=check(total>1000 && top>=total*99/100,"at least 99% of stacks unwind to the thread start");
    failures+=check(through>=total*99/100,"at least 99% of stacks pass through both callers");

    const auto allocating=run(allocate,1000,3,48).status;
    std::printf("%s\n",allocating.dump(1).substr(0,700).c_str());
    failures+=check(!allocating.at("running").get<bool>() && allocating.at("samples").get<size_t>()>1500,
                    "heap-heavy target sampled with stacks to completion without deadlock");

    loads=0;
    const bool was_loaded=GetModuleHandleW(L"msimg32.dll")!=nullptr;
    const auto loading=run(load_libraries,1000,3,48).status;
    std::printf("%s\nlibrary loads %llu\n",loading.dump(1).substr(0,700).c_str(),static_cast<unsigned long long>(loads.load()));
    failures+=check(!was_loaded && loads>100 && !loading.at("running").get<bool>() && loading.at("samples").get<size_t>()>1500,
                    "loader-lock-heavy target sampled with stacks to completion without deadlock");
    return failures;
}
