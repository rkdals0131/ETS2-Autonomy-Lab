#pragma once
#include "ot.hpp"
#include "stack_unwind.hpp"

namespace ot {
// Self-time profile of one thread from its instruction pointer, and with a
// depth its call stacks. The worker suspends the target only around
// GetThreadContext and the lock-free stack walk, and does not allocate, log or
// lock while it is suspended, so a target that holds the heap or loader lock
// cannot deadlock it. Off unless a bounded run is requested.
class ThreadSampler {
public:
    ThreadSampler();
    ~ThreadSampler() { stop(); }
    void start(DWORD thread,double hz,double seconds,unsigned depth=0);
    void stop() noexcept;
    json status();
    // Distinct stacks of the last run, most frequent first, as
    // [count,end,image,rva,image,rva,...]; image -1 means rva is an address.
    // Pages stay below max_bytes; "next" is the following offset or null.
    json stacks(size_t offset,size_t max_bytes=700000);
private:
    void run(DWORD thread,double hz,double seconds,unsigned depth) noexcept;
    Handle stop_event_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::mutex mutex_;
    DWORD thread_=0;
    double hz_=0,seconds_=0;
    unsigned depth_=0;
    std::vector<uint64_t> samples_,frames_; // RIPs; depth_ frame slots per sample
    std::vector<uint8_t> frame_counts_,ends_;
    std::vector<std::pair<std::vector<uint64_t>,uint64_t>> distinct_; // Built on the first stacks page.
    UnwindTable table_;
    uint64_t suspend_ticks_=0,suspend_max_=0,missed_=0,started_=0,ended_=0;
    std::string error_;
};
}
