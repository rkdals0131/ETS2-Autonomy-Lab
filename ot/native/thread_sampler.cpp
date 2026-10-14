#include "thread_sampler.hpp"
#include <algorithm>
#include <cstdio>
#include <map>
#include <stdexcept>

namespace ot {
namespace {
std::string hex(uintptr_t value) {char text[24];std::snprintf(text,sizeof(text),"0x%llX",static_cast<unsigned long long>(value));return text;}
}
ThreadSampler::ThreadSampler():stop_event_(CreateEventW(nullptr,TRUE,FALSE,nullptr)) {
    if(!stop_event_) throw std::runtime_error("Cannot create sampler stop event");
}
void ThreadSampler::start(DWORD thread,double hz,double seconds,unsigned depth) {
    if(running_.load()) throw std::runtime_error("A main-thread profile is already running");
    if(depth>64) throw std::runtime_error("Profile depth must be 0-64");
    if(worker_.joinable()) worker_.join();
    {
        std::lock_guard lock(mutex_);
        thread_=thread;hz_=hz;seconds_=seconds;depth_=depth;
        // Free the previous run's buffers (up to about 60 MB at 1 kHz, 120 s, depth 64).
        samples_={};frames_={};frame_counts_={};ends_={};distinct_={};table_=UnwindTable();error_.clear();
        suspend_ticks_=suspend_max_=missed_=ended_=0;started_=qpc_now();
    }
    ResetEvent(stop_event_.h);
    running_=true;
    try {worker_=std::thread([this,thread,hz,seconds,depth]{run(thread,hz,seconds,depth);});}
    catch(...) {running_=false;throw;}
}
void ThreadSampler::stop() noexcept {
    SetEvent(stop_event_.h);
    if(worker_.joinable()) worker_.join();
}
void ThreadSampler::run(DWORD id,double hz,double seconds,unsigned depth) noexcept {
    std::vector<uint64_t> rips,frames;std::vector<uint8_t> counts,ends;UnwindTable table;
    uint64_t total=0,maximum=0,missed=0;std::string error;
    try {
        const auto capacity=static_cast<size_t>(hz*seconds)+16;
        // No allocation may happen while the target is suspended.
        rips.reserve(capacity);counts.reserve(capacity);ends.reserve(capacity);frames.resize(capacity*depth);
        table.build(); // Modules loaded later count as unknown code.
        if(id==GetCurrentThreadId()) throw std::runtime_error("The sampler cannot sample its own thread");
        Handle thread(OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_QUERY_LIMITED_INFORMATION,FALSE,id));
        if(!thread) throw std::runtime_error("OpenThread failed: "+std::to_string(GetLastError()));
        const NT_TIB* tib=depth?thread_tib(thread.h):nullptr;
        Handle timer(CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS));
        if(!timer) throw std::runtime_error("CreateWaitableTimerExW failed: "+std::to_string(GetLastError()));
        LARGE_INTEGER due{};due.QuadPart=-static_cast<LONGLONG>(1e7/hz);
        const auto end=qpc_now()+static_cast<uint64_t>(seconds*static_cast<double>(qpc_frequency()));
        const HANDLE waits[]{stop_event_.h,timer.h};
        while(qpc_now()<end && rips.size()<capacity) {
            if(!SetWaitableTimer(timer.h,&due,0,nullptr,nullptr,FALSE)) throw std::runtime_error("SetWaitableTimer failed");
            if(WaitForMultipleObjects(2,waits,FALSE,INFINITE)!=WAIT_OBJECT_0+1) break;
            CONTEXT context{};context.ContextFlags=depth?CONTEXT_CONTROL|CONTEXT_INTEGER:CONTEXT_CONTROL;
            size_t count=0;StackEnd stack_end=StackEnd::depth;
            const auto begin=qpc_now();
            if(SuspendThread(thread.h)==static_cast<DWORD>(-1)) {++missed;continue;}
            const bool ok=GetThreadContext(thread.h,&context)!=0;
            if(ok && depth) count=walk_stack(table,context,tib,frames.data()+rips.size()*depth,depth,stack_end);
            ResumeThread(thread.h);
            const auto spent=qpc_now()-begin;
            total+=spent;maximum=std::max(maximum,spent);
            if(!ok) {++missed;continue;}
            rips.push_back(context.Rip);
            if(depth) {counts.push_back(static_cast<uint8_t>(count));ends.push_back(static_cast<uint8_t>(stack_end));}
        }
    } catch(const std::exception& e) {error=e.what();}
    catch(...) {error="Main-thread profile failed";}
    frames.resize(rips.size()*depth);
    table.release(); // Kept only for address lookups from here on.
    std::lock_guard lock(mutex_);
    samples_=std::move(rips);frames_=std::move(frames);frame_counts_=std::move(counts);ends_=std::move(ends);table_=std::move(table);
    suspend_ticks_=total;suspend_max_=maximum;missed_=missed;error_=error;ended_=qpc_now();
    running_=false;
}
json ThreadSampler::status() {
    std::lock_guard lock(mutex_);
    const double frequency=static_cast<double>(qpc_frequency());
    const bool running=running_.load();
    json result={{"running",running},{"thread_id",thread_},{"hz",hz_},{"seconds",seconds_},{"depth",depth_},
        {"elapsed_s",started_?static_cast<double>((running || !ended_?qpc_now():ended_)-started_)/frequency:0.0},
        {"samples",samples_.size()},{"missed",missed_},{"error",error_}};
    if(running || samples_.empty()) return result;
    const auto attempts=static_cast<double>(samples_.size()+missed_);
    result["suspend_us_mean"]=static_cast<double>(suspend_ticks_)/frequency*1e6/attempts;
    result["suspend_us_max"]=static_cast<double>(suspend_max_)/frequency*1e6;
    // Modules are those listed when the run started; code outside any image
    // (hook trampolines, JIT) or loaded during the run is anonymous.
    std::map<std::string,uint64_t> modules;
    std::map<std::pair<std::string,uintptr_t>,uint64_t> buckets;
    for(const auto rip:samples_) {
        if(const auto* image=table_.find(static_cast<uintptr_t>(rip))) {
            ++modules[image->name];++buckets[{image->name,(rip-image->base)&~uintptr_t{0xFF}}];
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
    if(depth_ && !frame_counts_.empty()) {
        uint64_t frames=0,ends[static_cast<size_t>(StackEnd::count)]{};
        for(size_t i=0;i<frame_counts_.size();++i) {frames+=frame_counts_[i];++ends[std::min<size_t>(ends_[i],std::size(ends)-1)];}
        json end_counts=json::object();
        for(size_t i=0;i<std::size(ends);++i) end_counts[stack_end_name(static_cast<StackEnd>(i))]=ends[i];
        result["stack_frames_mean"]=static_cast<double>(frames)/static_cast<double>(frame_counts_.size());
        result["stack_end"]=std::move(end_counts);
    }
    return result;
}
json ThreadSampler::stacks(size_t offset,size_t max_bytes) {
    std::lock_guard lock(mutex_);
    if(running_.load()) throw std::runtime_error("The profile is still running");
    if(!depth_) throw std::runtime_error("The last profile recorded no stacks; start it with a depth");
    if(distinct_.empty() && !frame_counts_.empty()) {
        std::map<std::vector<uint64_t>,uint64_t> counts;
        for(size_t i=0;i<frame_counts_.size();++i) {
            const auto* first=frames_.data()+i*depth_;
            std::vector<uint64_t> key(first,first+frame_counts_[i]);
            key.push_back(ends_[i]);
            ++counts[std::move(key)];
        }
        distinct_.assign(std::make_move_iterator(counts.begin()),std::make_move_iterator(counts.end()));
        std::stable_sort(distinct_.begin(),distinct_.end(),[](const auto& a,const auto& b){return a.second>b.second;});
    }
    const auto& images=table_.images();
    json list=json::array();size_t bytes=0,index=offset;
    for(;index<distinct_.size();++index) {
        const auto& [key,count]=distinct_[index];
        json entry=json::array({count,stack_end_name(static_cast<StackEnd>(key.back()))});
        for(size_t i=0;i+1<key.size();++i) {
            if(const auto* image=table_.find(static_cast<uintptr_t>(key[i]))) {
                entry.push_back(image-images.data());entry.push_back(key[i]-image->base);
            } else {entry.push_back(-1);entry.push_back(key[i]);}
        }
        bytes+=entry.dump().size()+1;
        if(bytes>max_bytes && index>offset) break;
        list.push_back(std::move(entry));
    }
    json result={{"samples",frame_counts_.size()},{"distinct",distinct_.size()},{"offset",offset},
                 {"next",index<distinct_.size()?json(index):json(nullptr)},{"stacks",std::move(list)}};
    if(!offset) {
        json image_list=json::array();
        for(const auto& image:images) image_list.push_back({{"name",image.name},{"path",image.path},{"size",image.end-image.base}});
        result["images"]=std::move(image_list);
    }
    return result;
}
}
