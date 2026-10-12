#pragma once
// Process stop and drive ownership shared by the GT baseline and drive_hold.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <process.h>
#else
#include <unistd.h>
#endif
#include <rclcpp/rclcpp.hpp>
#include <ets2_msgs/srv/drive_control.hpp>
#include <atomic>
#include <chrono>
#include <csignal>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

// Ends on Ctrl+C, or when the launcher sets the named event passed as
// --stop-event <name> (Windows). The caller then disarms before it exits, so
// the game reports "disarmed"; a killed process ends as "expired" instead.
class StopSignal {
public:
    StopSignal(int argc,char** argv) {
        std::signal(SIGINT,[](int){stopped_=true;});
        std::signal(SIGTERM,[](int){stopped_=true;});
        for(int i=1;i+1<argc;++i) if(std::string_view(argv[i])=="--stop-event") {
#ifdef _WIN32
            event_=OpenEventA(SYNCHRONIZE,FALSE,argv[i+1]);
            if(!event_) throw std::runtime_error("Launcher stop event unavailable");
            watch_=std::jthread([this](std::stop_token stop) {
                while(!stop.stop_requested()) if(WaitForSingleObject(event_,100)==WAIT_OBJECT_0) {stopped_=true;break;}
            });
#else
            throw std::runtime_error("--stop-event is a Windows launcher option; use Ctrl+C");
#endif
        }
    }
    StopSignal(const StopSignal&)=delete;
    ~StopSignal() {
#ifdef _WIN32
        watch_.request_stop();
        if(watch_.joinable()) watch_.join();
        if(event_) CloseHandle(event_);
#endif
    }
    bool requested() const {return stopped_;}
private:
    static inline std::atomic<bool> stopped_{false}; // Constant-initialized for the signal handler.
#ifdef _WIN32
    HANDLE event_{};
    std::jthread watch_;
#endif
};

inline std::string controller_owner(const char* role) {
#ifdef _WIN32
    const auto pid=_getpid();
#else
    const auto pid=getpid();
#endif
    return std::string(role)+"-"+std::to_string(pid)+"-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
}

// DriveCommand.sent_steady_ns: CLOCK_MONOTONIC on Linux, QPC on Windows (MSVC
// steady_clock), the clock the receiver compares against (docs/16).
inline uint64_t steady_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// One /ets2/drive/control round trip, spinning the node while it waits.
// Returns the armed epoch, or the given epoch after a disarm.
inline uint64_t request_drive_control(const rclcpp::Node::SharedPtr& node,const rclcpp::Client<ets2_msgs::srv::DriveControl>::SharedPtr& service,
                                      const std::string& owner,bool arm,uint64_t epoch,uint8_t axes) {
    using namespace std::chrono_literals;
    auto request=std::make_shared<ets2_msgs::srv::DriveControl::Request>();
    request->owner=owner;request->arm=arm;request->epoch=epoch;request->axes=arm?axes:0;
    auto future=service->async_send_request(request);
    const auto until=std::chrono::steady_clock::now()+1s;
    while(std::chrono::steady_clock::now()<until && future.wait_for(0s)!=std::future_status::ready) {
        rclcpp::spin_some(node);std::this_thread::sleep_for(2ms);
    }
    if(future.wait_for(0s)!=std::future_status::ready) throw std::runtime_error("DriveControl timed out");
    const auto response=future.get();
    if(!response->success) throw std::runtime_error(response->message);
    return arm?response->epoch:epoch;
}
