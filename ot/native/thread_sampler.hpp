#pragma once
#include "ot.hpp"

namespace ot {
// Self-time profile of one thread from its instruction pointer. The worker
// suspends the target only around GetThreadContext and does not allocate,
// log or lock while it is suspended, so a target that holds the heap or
// loader lock cannot deadlock it. Off unless a bounded run is requested.
class ThreadSampler {
public:
    ThreadSampler();
    ~ThreadSampler() { stop(); }
    void start(DWORD thread,double hz,double seconds);
    void stop() noexcept;
    json status();
private:
    void run(DWORD thread,double hz,double seconds) noexcept;
    Handle stop_event_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::mutex mutex_;
    DWORD thread_=0;
    double hz_=0,seconds_=0;
    std::vector<uint64_t> samples_;
    uint64_t suspend_ticks_=0,suspend_max_=0,missed_=0,started_=0,ended_=0;
    std::string error_;
};
}
