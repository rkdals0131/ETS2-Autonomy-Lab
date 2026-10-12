// RosEndpoint without the game: a fake core records what the endpoint forwards.
#include "ros_endpoint.hpp"
#include "ros_cdr.hpp"
#include <gtest/gtest.h>
#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/string.hpp>
#include <ets2_msgs/msg/drive_command.hpp>
#include <ets2_msgs/srv/drive_control.hpp>
#include <mutex>

using namespace std::chrono_literals;
using bridge::json;

namespace {
struct FakeCore {
    std::mutex mutex;std::vector<json> calls;
    json operator()(json request) {std::lock_guard lock(mutex);calls.push_back(request);return {{"accepted",true},{"epoch",7},{"reason","armed"}};}
    std::vector<json> snapshot() {std::lock_guard lock(mutex);return calls;}
};
template<class F> bool spin_until(const rclcpp::Node::SharedPtr& node,F done,std::chrono::milliseconds limit=3s) {
    const auto until=std::chrono::steady_clock::now()+limit;
    while(std::chrono::steady_clock::now()<until) {if(done()) return true;rclcpp::spin_some(node);std::this_thread::sleep_for(5ms);}
    return done();
}
uint64_t steady_ns(std::chrono::nanoseconds offset=0ns) {
    return static_cast<uint64_t>((std::chrono::steady_clock::now().time_since_epoch()+offset).count());
}
}

TEST(RosEndpoint,PublishesDemandsAndForwardsDrive) {
    FakeCore core;
    bridge::RosEndpoint endpoint(0,nullptr,[&](json request){return core(std::move(request));});
    auto node=std::make_shared<rclcpp::Node>("ros_endpoint_test");

    // A subscription becomes sensor demand within the 200 ms poll.
    std::string received;
    auto config=node->create_subscription<std_msgs::msg::String>("/ets2/sensors/config",rclcpp::QoS(1).reliable().transient_local(),
        [&](std_msgs::msg::String::SharedPtr message){received=message->data;});
    ASSERT_TRUE(spin_until(node,[&]{return endpoint.demand()->contains("/ets2/sensors/config");}));
    EXPECT_FALSE(endpoint.demand()->contains("/ets2/camera/C_FN/image_raw"));

    // Relay-serialized bytes arrive as the typed message.
    bridge::Packet packet;
    bridge::append_cdr(packet,"/ets2/sensors/config",64,[](bridge::Cdr& c){c<<std::string("relay-config");});
    endpoint.publish(packet);
    ASSERT_TRUE(spin_until(node,[&]{return received=="relay-config";}));
    bridge::Packet broken;broken.data.resize(8);
    broken.meta["messages"].push_back({{"topic","/ets2/sensors/config"},{"offset",4},{"length",8}});
    EXPECT_THROW(endpoint.publish(broken),std::runtime_error);

    // Arm goes through the service to the core and returns its epoch.
    auto client=node->create_client<ets2_msgs::srv::DriveControl>("/ets2/drive/control");
    ASSERT_TRUE(spin_until(node,[&]{return client->service_is_ready();}));
    auto arm=std::make_shared<ets2_msgs::srv::DriveControl::Request>();arm->owner="test";arm->arm=true;arm->axes=2;
    auto future=client->async_send_request(arm);
    ASSERT_TRUE(spin_until(node,[&]{return future.wait_for(0s)==std::future_status::ready;}));
    const auto reply=future.get();
    EXPECT_TRUE(reply->success);EXPECT_EQ(reply->epoch,7u);
    const auto armed=core.snapshot().back();
    EXPECT_EQ(armed.at("action"),"arm");EXPECT_EQ(armed.at("drive_owner"),"test");EXPECT_EQ(armed.at("axes"),2);

    // A fresh command is forwarded with a 200 ms window; a stale one is dropped.
    auto commands=node->create_publisher<ets2_msgs::msg::DriveCommand>("/ets2/drive/command",rclcpp::QoS(1).best_effort());
    ASSERT_TRUE(spin_until(node,[&]{return commands->get_subscription_count()>0;}));
    ets2_msgs::msg::DriveCommand command;command.owner="test";command.epoch=7;command.axes=2;command.throttle=.3f;
    command.sequence=1;
    ASSERT_TRUE(spin_until(node,[&]{
        command.sent_steady_ns=steady_ns(-200ms);commands->publish(command);
        return endpoint.diagnostics().at("stale_commands").get<uint64_t>()>0;}));
    EXPECT_TRUE(core.snapshot().back().at("action")=="arm");
    command.sequence=2;command.sent_steady_ns=steady_ns();
    const auto forwarded=[&]{for(const auto& call:core.snapshot()) if(call.at("action")=="command") return call;return json{};};
    ASSERT_TRUE(spin_until(node,[&]{
        if(!forwarded().is_null()) return true;
        command.sent_steady_ns=steady_ns();commands->publish(command);return false;}));
    const auto sent=forwarded();
    EXPECT_EQ(sent.at("sequence"),2);EXPECT_EQ(sent.at("axes"),2);EXPECT_TRUE(sent.contains("deadline_ms"));
    const auto diagnostics=endpoint.diagnostics();
    EXPECT_GE(diagnostics.at("stale_commands").get<uint64_t>(),1u);
    EXPECT_GE(diagnostics.at("forwarded_commands").get<uint64_t>(),1u);
    EXPECT_LT(diagnostics.at("command_age").at("max_ms").get<double>(),50.0);

    EXPECT_TRUE(endpoint.capture_wanted());
}

int main(int argc,char** argv) {
    testing::InitGoogleTest(&argc,argv);
    return RUN_ALL_TESTS();
}
