// RosEndpoint without the game: a fake core records what the endpoint forwards.
#include "ros_endpoint.hpp"
#include "ros_cdr.hpp"
#include <gtest/gtest.h>
#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/string.hpp>
#include <ets2_msgs/msg/drive_command.hpp>
#include <ets2_msgs/srv/drive_control.hpp>
#include <nav_msgs/msg/path.hpp>
#include <visualization_msgs/msg/marker_array.hpp>
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
    std::mutex paths_mutex;std::vector<bridge::OverlayMarks> paths;
    bridge::RosEndpoint endpoint(0,nullptr,[&](json request){return core(std::move(request));},
        [&](const bridge::OverlayMarks& marks){std::lock_guard lock(paths_mutex);paths.push_back(marks);});
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
    EXPECT_FALSE(sent.contains("retarder"));

    // With the auxiliary brakes the retarder step and engine brake travel along; a step
    // out of range or the retarder without the pedals is rejected here.
    const auto rejected_after=[&](uint8_t axes,int8_t step) {
        const auto before=endpoint.diagnostics().at("rejected_commands").get<uint64_t>();
        command.axes=axes;command.retarder_step=step;
        return spin_until(node,[&]{
            command.sent_steady_ns=steady_ns();commands->publish(command);
            return endpoint.diagnostics().at("rejected_commands").get<uint64_t>()>before;});
    };
    command.sequence=3;EXPECT_TRUE(rejected_after(4,0));EXPECT_TRUE(rejected_after(6,6));
    command.axes=6;
    command.retarder_step=3;command.engine_brake=true;command.sequence=5;
    const auto with_retarder=[&]{for(const auto& call:core.snapshot()) if(call.at("action")=="command" && call.contains("retarder")) return call;return json{};};
    ASSERT_TRUE(spin_until(node,[&]{
        if(!with_retarder().is_null()) return true;
        command.sent_steady_ns=steady_ns();commands->publish(command);return false;}));
    EXPECT_EQ(with_retarder().at("axes"),6);EXPECT_EQ(with_retarder().at("retarder"),3);EXPECT_EQ(with_retarder().at("engine_brake"),true);
    const auto diagnostics=endpoint.diagnostics();
    EXPECT_GE(diagnostics.at("stale_commands").get<uint64_t>(),1u);
    EXPECT_GE(diagnostics.at("forwarded_commands").get<uint64_t>(),1u);
    EXPECT_LT(diagnostics.at("command_age").at("max_ms").get<double>(),50.0);

    EXPECT_TRUE(endpoint.capture_wanted());

    // HUD path: a publisher requests the overlay; base_link paths reach the sink
    // with their stamp, other frames are rejected here.
    EXPECT_FALSE(endpoint.overlay_wanted());
    auto overlay=node->create_publisher<nav_msgs::msg::Path>("/ets2/overlay/path",rclcpp::QoS(1).best_effort());
    ASSERT_TRUE(spin_until(node,[&]{return endpoint.overlay_wanted();}));
    nav_msgs::msg::Path path;path.header.frame_id="map";path.poses.resize(2);path.poses[1].pose.position.x=80;
    path.header.stamp.sec=12;path.header.stamp.nanosec=345678000;
    const auto delivered=[&]{std::lock_guard lock(paths_mutex);return paths.size();};
    ASSERT_TRUE(spin_until(node,[&]{overlay->publish(path);return endpoint.diagnostics().at("rejected_overlay_paths").get<uint64_t>()>0;}));
    EXPECT_EQ(delivered(),0u);
    path.header.frame_id="base_link";
    ASSERT_TRUE(spin_until(node,[&]{overlay->publish(path);return delivered()>0;}));
    {
        std::lock_guard lock(paths_mutex);
        EXPECT_EQ(paths.front().frame,"base_link");EXPECT_EQ(paths.front().stamp_us,12345678u);
        ASSERT_EQ(paths.front().strokes.size(),1u);
        ASSERT_EQ(paths.front().strokes[0].points.size(),2u);EXPECT_EQ(paths.front().strokes[0].points[1][0],80);
        EXPECT_FLOAT_EQ(paths.front().strokes[0].width_m,3.5f);EXPECT_EQ(paths.front().ttl_ms,300u);
    }
    // An empty path is a clear request: it reaches the sink (any frame) instead of being rejected.
    const auto rejected=endpoint.diagnostics().at("rejected_overlay_paths").get<uint64_t>();
    path.header.frame_id="world";path.poses.clear();
    ASSERT_TRUE(spin_until(node,[&]{overlay->publish(path);std::lock_guard lock(paths_mutex);return paths.back().strokes.empty();}));
    EXPECT_EQ(endpoint.diagnostics().at("rejected_overlay_paths").get<uint64_t>(),rejected);
    // HUD marks: each LINE_STRIP marker is a stroke (scale.x road width, scale.y screen width
    // in pixels, the half of which is the stroke's minimum); other types are ignored.
    auto marks=node->create_publisher<visualization_msgs::msg::MarkerArray>("/ets2/hud/markers",rclcpp::QoS(1).best_effort());
    visualization_msgs::msg::MarkerArray array;
    // Two road strokes, a scene sphere and a scene line (Foxglove only: not painted).
    for(int i=0;i<4;++i) {
        visualization_msgs::msg::Marker m;m.header.frame_id="world";m.header.stamp.sec=20;
        m.type=i==2?visualization_msgs::msg::Marker::SPHERE:visualization_msgs::msg::Marker::LINE_STRIP;m.ns=i>=2?"scene/x":"road/test";m.action=visualization_msgs::msg::Marker::ADD;
        m.scale.x=i?.4:.2;m.scale.y=i?8:4;m.color.r=1;m.color.g=i?.6f:1;m.color.b=1;m.color.a=.7f;m.pose.orientation.w=1;m.pose.position.x=i?10:0;
        m.points.resize(2);m.points[1].y=i?3:0;m.points[1].x=i?0:50;
        array.markers.push_back(m);
    }
    ASSERT_TRUE(spin_until(node,[&]{marks->publish(array);std::lock_guard lock(paths_mutex);return paths.back().strokes.size()==2;}));
    {
        std::lock_guard lock(paths_mutex);const auto& m=paths.back();
        EXPECT_EQ(m.frame,"world");EXPECT_EQ(m.stamp_us,20000000u);
        EXPECT_FLOAT_EQ(m.strokes[1].width_m,.4f);EXPECT_FLOAT_EQ(m.strokes[1].min_px,4);EXPECT_FLOAT_EQ(m.strokes[1].rgba[1],.6f);
        EXPECT_EQ(m.strokes[1].points[0][0],10);EXPECT_EQ(m.strokes[1].points[1][1],3); // marker pose applied
    }
    array.markers.resize(1);array.markers[0].action=visualization_msgs::msg::Marker::DELETEALL;
    ASSERT_TRUE(spin_until(node,[&]{marks->publish(array);std::lock_guard lock(paths_mutex);return paths.back().strokes.empty();}));
}

int main(int argc,char** argv) {
    testing::InitGoogleTest(&argc,argv);
    return RUN_ALL_TESTS();
}
