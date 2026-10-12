#include "thread_sampler.hpp"
#include <psapi.h>
#include <algorithm>
#include <cstdio>
#include <map>
#include <stdexcept>

namespace ot {
namespace {
struct Range {uintptr_t begin,end;std::string name;};
std::vector<Range> module_ranges() {
    std::vector<HMODULE> modules(1024);DWORD needed=0;
    if(!EnumProcessModulesEx(GetCurrentProcess(),modules.data(),static_cast<DWORD>(modules.size()*sizeof(HMODULE)),&needed,LIST_MODULES_64BIT))
        return {};
    modules.resize(std::min<size_t>(needed/sizeof(HMODULE),modules.size()));
    std::vector<Range> ranges;
    for(auto handle:modules) {
        MODULEINFO info{};char name[MAX_PATH]{};
        if(GetModuleInformation(GetCurrentProcess(),handle,&info,sizeof(info)) &&
           GetModuleBaseNameA(GetCurrentProcess(),handle,name,MAX_PATH)) {
            const auto begin=reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
            ranges.push_back({begin,begin+info.SizeOfImage,name});
        }
    }
    std::sort(ranges.begin(),ranges.end(),[](const Range& a,const Range& b){return a.begin<b.begin;});
    return ranges;
}
std::string hex(uintptr_t value) {char text[24];std::snprintf(text,sizeof(text),"0x%llX",static_cast<unsigned long long>(value));return text;}
}
ThreadSampler::ThreadSampler():stop_event_(CreateEventW(nullptr,TRUE,FALSE,nullptr)) {
    if(!stop_event_) throw std::runtime_error("Cannot create sampler stop event");
}
void ThreadSampler::start(DWORD thread,double hz,double seconds) {
    if(running_.load()) throw std::runtime_error("A main-thread profile is already running");
    if(worker_.joinable()) worker_.join();
    {
        std::lock_guard lock(mutex_);
        thread_=thread;hz_=hz;seconds_=seconds;samples_.clear();error_.clear();
        suspend_ticks_=suspend_max_=missed_=ended_=0;started_=qpc_now();
    }
    ResetEvent(stop_event_.h);
    running_=true;
    try {worker_=std::thread([this,thread,hz,seconds]{run(thread,hz,seconds);});}
    catch(...) {running_=false;throw;}
}
void ThreadSampler::stop() noexcept {
    SetEvent(stop_event_.h);
    if(worker_.joinable()) worker_.join();
}
void ThreadSampler::run(DWORD id,double hz,double seconds) noexcept {
    std::vector<uint64_t> rips;
    uint64_t total=0,maximum=0,missed=0;std::string error;
    try {
        const auto capacity=static_cast<size_t>(hz*seconds)+16;
        rips.reserve(capacity); // No allocation may happen while the target is suspended.
        Handle thread(OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT,FALSE,id));
        if(!thread) throw std::runtime_error("OpenThread failed: "+std::to_string(GetLastError()));
        Handle timer(CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS));
        if(!timer) throw std::runtime_error("CreateWaitableTimerExW failed: "+std::to_string(GetLastError()));
        LARGE_INTEGER due{};due.QuadPart=-static_cast<LONGLONG>(1e7/hz);
        const auto end=qpc_now()+static_cast<uint64_t>(seconds*static_cast<double>(qpc_frequency()));
        const HANDLE waits[]{stop_event_.h,timer.h};
        while(qpc_now()<end && rips.size()<capacity) {
            if(!SetWaitableTimer(timer.h,&due,0,nullptr,nullptr,FALSE)) throw std::runtime_error("SetWaitableTimer failed");
            if(WaitForMultipleObjects(2,waits,FALSE,INFINITE)!=WAIT_OBJECT_0+1) break;
            CONTEXT context{};context.ContextFlags=CONTEXT_CONTROL;
            const auto begin=qpc_now();
            if(SuspendThread(thread.h)==static_cast<DWORD>(-1)) {++missed;continue;}
            const bool ok=GetThreadContext(thread.h,&context)!=0;
            ResumeThread(thread.h);
            const auto spent=qpc_now()-begin;
            total+=spent;maximum=std::max(maximum,spent);
            if(ok) rips.push_back(context.Rip); else ++missed;
        }
    } catch(const std::exception& e) {error=e.what();}
    catch(...) {error="Main-thread profile failed";}
    std::lock_guard lock(mutex_);
    samples_=std::move(rips);suspend_ticks_=total;suspend_max_=maximum;missed_=missed;error_=error;ended_=qpc_now();
    running_=false;
}
json ThreadSampler::status() {
    std::lock_guard lock(mutex_);
    const double frequency=static_cast<double>(qpc_frequency());
    const bool running=running_.load();
    json result={{"running",running},{"thread_id",thread_},{"hz",hz_},{"seconds",seconds_},
        {"elapsed_s",started_?static_cast<double>((running || !ended_?qpc_now():ended_)-started_)/frequency:0.0},
        {"samples",samples_.size()},{"missed",missed_},{"error",error_}};
    if(running || samples_.empty()) return result;
    const auto attempts=static_cast<double>(samples_.size()+missed_);
    result["suspend_us_mean"]=static_cast<double>(suspend_ticks_)/frequency*1e6/attempts;
    result["suspend_us_max"]=static_cast<double>(suspend_max_)/frequency*1e6;
    // Modules are listed now; one loaded or unloaded during the run can be
    // misattributed. Code outside any image (hook trampolines, JIT) is anonymous.
    const auto ranges=module_ranges();
    std::map<std::string,uint64_t> modules;
    std::map<std::pair<std::string,uintptr_t>,uint64_t> buckets;
    for(const auto rip:samples_) {
        const auto next=std::upper_bound(ranges.begin(),ranges.end(),static_cast<uintptr_t>(rip),
            [](uintptr_t value,const Range& range){return value<range.begin;});
        if(next!=ranges.begin() && rip<std::prev(next)->end) {
            const auto& range=*std::prev(next);
            ++modules[range.name];++buckets[{range.name,(rip-range.begin)&~uintptr_t{0xFF}}];
        } else ++modules["(anonymous)"];
    }
    const double total=static_cast<double>(samples_.size());
    std::vector<std::pair<std::string,uint64_t>> by_module(modules.begin(),modules.end());
    std::sort(by_module.begin(),by_module.end(),[](const auto& a,const auto& b){return a.second>b.second;});
    json module_list=json::array();
    for(const auto& [name,count]:by_module) module_list.push_back({{"module",name},{"samples",count},{"share",count/total}});
    std::vector<std::pair<std::pair<std::string,uintptr_t>,uint64_t>> by_bucket(buckets.begin(),buckets.end());
    std::sort(by_bucket.begin(),by_bucket.end(),[](const auto& a,const auto& b){return a.second>b.second;});
    json bucket_list=json::array();
    for(size_t i=0;i<by_bucket.size() && i<40;++i) {
        const auto& [key,count]=by_bucket[i];
        bucket_list.push_back({{"module",key.first},{"rva_256",hex(key.second)},{"samples",count},{"share",count/total}});
    }
    result["modules"]=std::move(module_list);result["hot_rva_256"]=std::move(bucket_list);
    return result;
}
}
