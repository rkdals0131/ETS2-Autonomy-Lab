#pragma once
#include "messages.hpp"
#include <functional>
#include <memory>

namespace bridge {
// The relay's ROS 2 node (docs/23). Publishes relay-serialized XCDRv1 messages,
// derives sensor demand from subscriptions, owns /ets2/capture and the drive
// interface. Accepted drive commands reach the core through `control` with a
// fresh 200 ms window; arm/disarm calls it from the service callback.
class RosEndpoint {
public:
    using Control=std::function<json(json)>;
    // Receives each valid /ets2/overlay/path on the executor thread.
    using PathSink=std::function<void(const OverlayPath&)>;
    RosEndpoint(int argc,char** argv,Control control,PathSink overlay={});
    ~RosEndpoint();
    RosEndpoint(const RosEndpoint&)=delete;
    // Any thread. Copies each message range once into a reused SerializedMessage.
    void publish(const Packet& packet);
    std::shared_ptr<const Demand> demand() const;
    bool capture_wanted() const;
    // A /ets2/overlay/path publisher exists or the overlay_enabled parameter is set.
    bool overlay_wanted() const;
    // Sender-stamp age of forwarded commands (p50/p95/p99/max ms) and drops.
    json diagnostics();
    // Count and newest records of publishes that took 50 ms or more.
    json publish_stats();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
