#pragma once
#include "wire.hpp"
#include "windows_support.hpp"
#include <atomic>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>

namespace bridge {
struct Latency {
    std::mutex mutex;std::deque<double> values;
    void add(uint64_t ping) {const double ms=(microseconds()-ping)/1000.0;std::lock_guard lock(mutex);if(values.size()==4096) values.pop_front();values.push_back(ms);}
    json snapshot() {std::vector<double> v;{std::lock_guard lock(mutex);v.assign(values.begin(),values.end());}
        if(v.empty()) return nullptr;std::sort(v.begin(),v.end());
        return {{"samples",v.size()},{"p50_ms",v[(v.size()-1)/2]},{"p95_ms",v[(v.size()-1)*95/100]},{"p99_ms",v[(v.size()-1)*99/100]},{"max_ms",v.back()}};
    }
};
template<class T> class LatestQueue {
    std::mutex mutex_;std::deque<T> queue_;
public:
    bool push(T value,T* retired=nullptr) {std::lock_guard lock(mutex_);bool dropped=queue_.size()==2;
        if(dropped) {if(retired) *retired=std::move(queue_.front());queue_.pop_front();}queue_.push_back(std::move(value));return dropped;}
    bool pop(T& value) {std::lock_guard lock(mutex_);if(queue_.empty()) return false;value=std::move(queue_.front());queue_.pop_front();return true;}
    void clear() {std::lock_guard lock(mutex_);queue_.clear();}
};
// At most one buffer is copying, two are queued, and one is encoding. Return
// their storage after use instead of zero-initializing a new 24 MB bundle.
class ReadBuffers {
    std::mutex mutex_;std::vector<Bytes> free_;
public:
    Bytes take() {std::lock_guard lock(mutex_);if(free_.empty()) return {};auto b=std::move(free_.back());free_.pop_back();return b;}
    void put(Bytes b) {if(!b.capacity()) return;std::lock_guard lock(mutex_);free_.push_back(std::move(b));}
};
struct Workers {
    std::atomic<bool> alive{true},network_failed{false},fatal{false};const Socket& state;const Socket& bulk;std::vector<std::jthread> threads;
    Workers(const Socket& a,const Socket& b):state(a),bulk(b) {}
    void stop(){alive=false;state.interrupt();bulk.interrupt();for(auto& t:threads) if(t.joinable()) t.join();}
    ~Workers(){stop();}
    template<class F> void start(F f) {threads.emplace_back([this,f]{
        try{f();}
        catch(const TransportError& e) {if(alive.exchange(false)) {network_failed=true;std::cerr<<e.what()<<std::endl;}state.interrupt();bulk.interrupt();}
        catch(const std::exception& e) {if(alive.exchange(false)) {fatal=true;std::cerr<<e.what()<<std::endl;}state.interrupt();bulk.interrupt();}
    });}
};
}
