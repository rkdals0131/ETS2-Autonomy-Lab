// Game-free check of ThreadSampler: attribution to the right module and no
// deadlock while the target thread holds the process heap lock most of the time.
#include "thread_sampler.hpp"
#include <cstdio>
#include <cstdlib>

using ot::json;
namespace {
std::atomic<bool> stop{false};
std::atomic<uint64_t> sink{0};
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
int check(bool ok,const char* what) {std::printf("%s %s\n",ok?"PASS":"FAIL",what);return ok?0:1;}
json run(void(*work)(),double hz,double seconds) {
    stop=false;DWORD id=0;
    std::thread target([&]{id=GetCurrentThreadId();work();});
    while(!id) Sleep(1);
    ot::ThreadSampler sampler;sampler.start(id,hz,seconds);
    const auto deadline=GetTickCount64()+static_cast<uint64_t>(seconds*1000)+5000;
    json status=sampler.status();
    while(status.at("running").get<bool>() && GetTickCount64()<deadline) {Sleep(50);status=sampler.status();}
    stop=true;target.join();
    return status;
}
}
int main() {
    int failures=0;
    const auto spinning=run(spin,1000,2);
    std::printf("%s\n",spinning.dump(1).substr(0,900).c_str());
    const auto& modules=spinning.at("modules");
    failures+=check(!spinning.at("running").get<bool>(),"spin run finished");
    failures+=check(spinning.at("samples").get<size_t>()>1000,"spin run collected samples near 1 kHz");
    failures+=check(!modules.empty() && modules[0].at("module")=="ot_sampler_test.exe" && modules[0].at("share").get<double>()>0.9,
                    "busy thread attributed to its own image");
    const auto allocating=run(allocate,1000,3);
    std::printf("%s\n",allocating.dump(1).substr(0,700).c_str());
    failures+=check(!allocating.at("running").get<bool>() && allocating.at("samples").get<size_t>()>1500,
                    "heap-heavy target sampled to completion without deadlock");
    return failures;
}
