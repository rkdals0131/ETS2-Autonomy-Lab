#include "ros_endpoint.hpp"
#include "relay_workers.hpp"
#include <rclcpp/rclcpp.hpp>
#include <std_srvs/srv/set_bool.hpp>
#include <ets2_msgs/msg/drive_command.hpp>
#include <ets2_msgs/msg/overlay_style.hpp>
#include <ets2_msgs/srv/drive_control.hpp>
#include <nav_msgs/msg/path.hpp>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <optional>
#include <unordered_map>

namespace bridge {
using namespace std::chrono_literals;
static bool valid_owner(const std::string& owner) {return !owner.empty() && owner.size()<=128 && owner.find('\0')==std::string::npos;}

struct RosEndpoint::Impl {
    struct Publisher {rclcpp::GenericPublisher::SharedPtr publisher;std::mutex mutex;rclcpp::SerializedMessage message;};
    Control control;
    rclcpp::Node::SharedPtr node;
    std::unordered_map<std::string,std::unique_ptr<Publisher>> publishers;
    rclcpp::Service<std_srvs::srv::SetBool>::SharedPtr capture_service;
    rclcpp::Service<ets2_msgs::srv::DriveControl>::SharedPtr drive_service;
    rclcpp::Subscription<ets2_msgs::msg::DriveCommand>::SharedPtr drive_commands;
    // HUD path (docs/24): forwarded to the relay's pose transform and shared memory.
    PathSink overlay_sink;
    rclcpp::Subscription<nav_msgs::msg::Path>::SharedPtr overlay_paths;
    rclcpp::Subscription<ets2_msgs::msg::OverlayStyle>::SharedPtr overlay_styles;
    std::optional<ets2_msgs::msg::OverlayStyle> overlay_style; // executor thread only
    std::chrono::steady_clock::time_point overlay_style_time{};
    std::atomic<bool> overlay{false};
    std::atomic<uint64_t> overlay_received{0},overlay_rejected{0};
    rclcpp::TimerBase::SharedPtr demand_timer;
    std::atomic<std::shared_ptr<const Demand>> demand{std::make_shared<const Demand>()};
    std::atomic<bool> capture{true};
    std::chrono::nanoseconds max_age{};
    // Newest validated command; older ones are superseded, like the old TCP slot.
    std::mutex drive_mutex;std::condition_variable drive_ready;
    std::optional<json> drive_request;uint64_t drive_sent_ns=0;bool stopping=false;
    Latency command_age;
    std::atomic<uint64_t> stale{0},rejected{0},forwarded{0};
    // Publishes that took 50 ms or more, newest 16 with wall time, to line
    // up with subscriber joins/exits when send_elapsed spikes.
    std::mutex slow_mutex;std::deque<json> slow;std::atomic<uint64_t> slow_count{0};
    // Created after rclcpp::init: an executor takes the default context when constructed.
    std::unique_ptr<rclcpp::executors::SingleThreadedExecutor> executor;
    std::jthread spin,forward;
};

RosEndpoint::RosEndpoint(int argc,char** argv,Control control,PathSink overlay):impl_(std::make_unique<Impl>()) {
    auto& m=*impl_;m.control=std::move(control);m.overlay_sink=std::move(overlay);
    // The relay's console handler and stop event own shutdown.
    rclcpp::init(argc,argv,rclcpp::InitOptions(),rclcpp::SignalHandlerOptions::None);
    m.node=std::make_shared<rclcpp::Node>("ets2_relay");
    for(const auto& topic:topics()) {
        auto qos=rclcpp::QoS(rclcpp::KeepLast(topic.latched?1:3));
        if(topic.latched) qos.reliable().transient_local();
        else if(topic.state) qos.reliable();
        else qos.best_effort();
        auto entry=std::make_unique<Impl::Publisher>();
        entry->publisher=m.node->create_generic_publisher(topic.name,topic.type,qos);
        m.publishers.emplace(topic.name,std::move(entry));
    }
    // The core grants each forwarded command a fresh 200 ms window, so a command
    // delayed on this host must not get one: drop it by its sender steady stamp.
    m.max_age=std::chrono::nanoseconds(static_cast<int64_t>(m.node->declare_parameter("max_command_age_ms",50.0)*1e6));
    m.capture_service=m.node->create_service<std_srvs::srv::SetBool>("/ets2/capture",
        [&m](const std_srvs::srv::SetBool::Request::SharedPtr request,std_srvs::srv::SetBool::Response::SharedPtr response) {
            m.capture=request->data;response->success=true;
            response->message="Capture request updated; F11 still requires restarting the relay";
        });
    m.drive_service=m.node->create_service<ets2_msgs::srv::DriveControl>("/ets2/drive/control",
        [&m](const ets2_msgs::srv::DriveControl::Request::SharedPtr request,ets2_msgs::srv::DriveControl::Response::SharedPtr response) {
            if(!valid_owner(request->owner) || request->axes>7 || ((request->axes&4) && !(request->axes&2))) {response->message="Invalid owner or axes";return;}
            try {
                // Arm and disarm are rare; the pipe call may hold this executor briefly.
                const auto state=m.control({{"cmd","drive"},{"action",request->arm?"arm":"disarm"},{"drive_owner",request->owner},
                    {"epoch",request->epoch},{"axes",request->axes?request->axes:3}});
                response->success=state.value("accepted",false);response->epoch=state.value("epoch",uint64_t{0});
                response->message=state.value("error",std::string{});
                if(response->message.empty()) response->message=state.value("reason",std::string{});
            } catch(const std::exception& e) {response->message=e.what();}
        });
    m.drive_commands=m.node->create_subscription<ets2_msgs::msg::DriveCommand>("/ets2/drive/command",rclcpp::QoS(1).best_effort(),
        [&m](const ets2_msgs::msg::DriveCommand::SharedPtr command) {
            if(!valid_owner(command->owner) || !std::isfinite(command->steering) || !std::isfinite(command->throttle) || !std::isfinite(command->brake) ||
               std::abs(command->steering)>1 || command->throttle<0 || command->throttle>1 || command->brake<0 || command->brake>1 || command->axes>7 ||
               ((command->axes&4) && (!(command->axes&2) || command->retarder_step<0 || command->retarder_step>5))) {++m.rejected;return;}
            const auto age=std::chrono::steady_clock::now().time_since_epoch()-std::chrono::nanoseconds(command->sent_steady_ns);
            if(!command->sent_steady_ns || age<0ns || age>m.max_age) {
                ++m.stale;
                RCLCPP_WARN_THROTTLE(m.node->get_logger(),*m.node->get_clock(),1000,"Dropped stale drive command: age %.1f ms",age.count()*1e-6);
                return;
            }
            {
                std::lock_guard lock(m.drive_mutex);
                m.drive_request=json{{"cmd","drive"},{"action","command"},{"drive_owner",command->owner},{"epoch",command->epoch},
                    {"sequence",command->sequence},{"steering",command->steering},{"throttle",command->throttle},{"brake",command->brake},
                    {"axes",command->axes?command->axes:3}};
                if(command->axes&4) {(*m.drive_request)["retarder"]=command->retarder_step;(*m.drive_request)["engine_brake"]=bool(command->engine_brake);}
                m.drive_sent_ns=command->sent_steady_ns;
            }
            m.drive_ready.notify_one();
        });
    // The decal is requested while a path publisher exists, or always with overlay_enabled.
    m.node->declare_parameter("overlay_enabled",false);
    m.node->declare_parameter("overlay_width_m",3.5);
    m.node->declare_parameter("overlay_alpha",0.22);
    // The GT node publishes at 20 Hz: a path it stopped sending disappears within 0.3 s.
    m.node->declare_parameter("overlay_ttl_ms",300);
    // Optional style (width, colour) for the following paths, e.g. by assistance state.
    m.overlay_styles=m.node->create_subscription<ets2_msgs::msg::OverlayStyle>("/ets2/overlay/style",rclcpp::QoS(1).best_effort(),
        [&m](const ets2_msgs::msg::OverlayStyle::SharedPtr message) {m.overlay_style=*message;m.overlay_style_time=std::chrono::steady_clock::now();});
    m.overlay_paths=m.node->create_subscription<nav_msgs::msg::Path>("/ets2/overlay/path",rclcpp::QoS(1).best_effort(),
        [&m](const nav_msgs::msg::Path::SharedPtr message) {
            OverlayPath path;path.frame=message->header.frame_id;
            const auto& stamp=message->header.stamp;
            path.stamp_us=stamp.sec<0?0:uint64_t(stamp.sec)*1000000+stamp.nanosec/1000;
            path.width_m=static_cast<float>(m.node->get_parameter("overlay_width_m").as_double());
            path.rgba[3]=static_cast<float>(m.node->get_parameter("overlay_alpha").as_double());
            path.ttl_ms=static_cast<uint64_t>(std::max<int64_t>(0,m.node->get_parameter("overlay_ttl_ms").as_int()));
            if(m.overlay_style && std::chrono::steady_clock::now()-m.overlay_style_time<=std::chrono::seconds(1)) {
                const auto& style=*m.overlay_style;
                path.width_m=style.width_m;path.rgba={style.color.r,style.color.g,style.color.b,style.color.a};
            }
            for(const auto& pose:message->poses) path.points.push_back({pose.pose.position.x,pose.pose.position.y,pose.pose.position.z});
            // An empty path clears the decal now (the writer marks the content invalid).
            if(path.points.empty()) {++m.overlay_received;if(m.overlay_sink) m.overlay_sink(path);return;}
            if((path.frame!="base_link" && path.frame!="world") || path.points.size()<2 || !std::isfinite(path.width_m) || path.width_m<=0 ||
               path.width_m>16 || !std::isfinite(path.rgba[3]) || path.rgba[3]<0 || path.rgba[3]>1) {
                ++m.overlay_rejected;
                RCLCPP_WARN_THROTTLE(m.node->get_logger(),*m.node->get_clock(),2000,"Rejected overlay path: frame '%s', %zu points",path.frame.c_str(),path.points.size());
                return;
            }
            ++m.overlay_received;
            if(m.overlay_sink) m.overlay_sink(path);
        });
    m.demand_timer=m.node->create_wall_timer(200ms,[&m] {
        auto demand=std::make_shared<Demand>();
        for(const auto& [name,entry]:m.publishers) if(entry->publisher->get_subscription_count()>0) demand->insert(name);
        m.demand.store(std::move(demand));
        m.overlay=m.node->get_parameter("overlay_enabled").as_bool() || m.overlay_paths->get_publisher_count()>0;
    });
    m.forward=std::jthread([&m] {
        for(;;) {
            json request;uint64_t sent=0;
            {
                std::unique_lock lock(m.drive_mutex);
                m.drive_ready.wait(lock,[&]{return m.stopping || m.drive_request.has_value();});
                if(m.stopping) return;
                request=std::move(*m.drive_request);m.drive_request.reset();sent=m.drive_sent_ns;
            }
            // The window starts at receipt on the core's clock (GetTickCount64),
            // so it never exceeds now+200.
            request["deadline_ms"]=ticks()+200;
            try {
                if(m.control(std::move(request)).value("accepted",false)) {m.command_age.add(sent/1000);++m.forwarded;}
                else ++m.rejected;
            } catch(const std::exception& e) {
                ++m.rejected;
                RCLCPP_WARN_THROTTLE(m.node->get_logger(),*m.node->get_clock(),1000,"Drive command failed: %s",e.what());
            }
        }
    });
    m.executor=std::make_unique<rclcpp::executors::SingleThreadedExecutor>();
    m.executor->add_node(m.node);
    m.spin=std::jthread([&m]{m.executor->spin();});
}

RosEndpoint::~RosEndpoint() {
    auto& m=*impl_;
    {std::lock_guard lock(m.drive_mutex);m.stopping=true;}
    m.drive_ready.notify_all();
    if(m.forward.joinable()) m.forward.join();
    m.executor->cancel();
    if(m.spin.joinable()) m.spin.join();
    m.executor->remove_node(m.node);m.executor.reset();
    m.demand_timer.reset();m.overlay_paths.reset();m.overlay_styles.reset();m.drive_commands.reset();m.drive_service.reset();m.capture_service.reset();
    m.publishers.clear();m.node.reset();
    rclcpp::shutdown();
}

void RosEndpoint::publish(const Packet& packet) {
    if(!packet.meta.contains("messages")) return;
    for(const auto& message:packet.meta.at("messages")) {
        const auto& topic=message.at("topic").get_ref<const std::string&>();
        const auto offset=message.at("offset").get<size_t>(),length=message.at("length").get<size_t>();
        const auto found=impl_->publishers.find(topic);
        // The relay itself produced these ranges; a mismatch is a relay bug.
        if(found==impl_->publishers.end() || length<4 || offset>packet.data.size() || length>packet.data.size()-offset)
            throw std::runtime_error("Relay produced an invalid message range for "+topic);
        auto& entry=*found->second;
        const auto queued=std::chrono::steady_clock::now();
        std::lock_guard lock(entry.mutex);
        const auto begin=std::chrono::steady_clock::now();
        entry.message.reserve(length);
        auto& raw=entry.message.get_rcl_serialized_message();
        std::memcpy(raw.buffer,packet.data.data()+offset,length);raw.buffer_length=length;
        entry.publisher->publish(entry.message);
        const auto end=std::chrono::steady_clock::now();
        if(end-queued>=std::chrono::milliseconds(50)) {
            const auto ms=[](auto d){return std::chrono::duration<double,std::milli>(d).count();};
            const auto wall=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            std::lock_guard slow_lock(impl_->slow_mutex);
            impl_->slow.push_back({{"wall_ms",wall},{"topic",topic},{"wait_ms",ms(begin-queued)},{"publish_ms",ms(end-begin)},{"bytes",length}});
            if(impl_->slow.size()>16) impl_->slow.pop_front();
            ++impl_->slow_count;
        }
    }
}
json RosEndpoint::publish_stats() {
    std::lock_guard lock(impl_->slow_mutex);
    return {{"count",impl_->slow_count.load()},{"recent",json(impl_->slow)}};
}
std::shared_ptr<const Demand> RosEndpoint::demand() const {return impl_->demand.load();}
bool RosEndpoint::capture_wanted() const {return impl_->capture.load();}
bool RosEndpoint::overlay_wanted() const {return impl_->overlay.load();}
json RosEndpoint::diagnostics() {
    return {{"command_age",impl_->command_age.snapshot()},{"forwarded_commands",impl_->forwarded.load()},
        {"stale_commands",impl_->stale.load()},{"rejected_commands",impl_->rejected.load()},
        {"overlay_paths",impl_->overlay_received.load()},{"rejected_overlay_paths",impl_->overlay_rejected.load()}};
}
}
