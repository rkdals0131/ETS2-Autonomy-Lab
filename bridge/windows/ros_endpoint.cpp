#include "ros_endpoint.hpp"
#include "relay_workers.hpp"
#include <rclcpp/rclcpp.hpp>
#include <std_srvs/srv/set_bool.hpp>
#include <ets2_msgs/msg/drive_command.hpp>
#include <ets2_msgs/srv/drive_control.hpp>
#include <cmath>
#include <condition_variable>
#include <cstring>
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
    rclcpp::TimerBase::SharedPtr demand_timer;
    std::atomic<std::shared_ptr<const Demand>> demand{std::make_shared<const Demand>()};
    std::atomic<bool> capture{true};
    std::chrono::nanoseconds max_age{};
    // Newest validated command; older ones are superseded, like the old TCP slot.
    std::mutex drive_mutex;std::condition_variable drive_ready;
    std::optional<json> drive_request;uint64_t drive_sent_ns=0;bool stopping=false;
    Latency command_age;
    std::atomic<uint64_t> stale{0},rejected{0},forwarded{0};
    // Created after rclcpp::init: an executor takes the default context when constructed.
    std::unique_ptr<rclcpp::executors::SingleThreadedExecutor> executor;
    std::jthread spin,forward;
};

RosEndpoint::RosEndpoint(int argc,char** argv,Control control):impl_(std::make_unique<Impl>()) {
    auto& m=*impl_;m.control=std::move(control);
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
            if(!valid_owner(request->owner) || request->axes>3) {response->message="Invalid owner or axes";return;}
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
               std::abs(command->steering)>1 || command->throttle<0 || command->throttle>1 || command->brake<0 || command->brake>1 || command->axes>3) {++m.rejected;return;}
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
                m.drive_sent_ns=command->sent_steady_ns;
            }
            m.drive_ready.notify_one();
        });
    m.demand_timer=m.node->create_wall_timer(200ms,[&m] {
        auto demand=std::make_shared<Demand>();
        for(const auto& [name,entry]:m.publishers) if(entry->publisher->get_subscription_count()>0) demand->insert(name);
        m.demand.store(std::move(demand));
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
    m.demand_timer.reset();m.drive_commands.reset();m.drive_service.reset();m.capture_service.reset();
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
        std::lock_guard lock(entry.mutex);
        entry.message.reserve(length);
        auto& raw=entry.message.get_rcl_serialized_message();
        std::memcpy(raw.buffer,packet.data.data()+offset,length);raw.buffer_length=length;
        entry.publisher->publish(entry.message);
    }
}
std::shared_ptr<const Demand> RosEndpoint::demand() const {return impl_->demand.load();}
bool RosEndpoint::capture_wanted() const {return impl_->capture.load();}
json RosEndpoint::diagnostics() {
    return {{"command_age",impl_->command_age.snapshot()},{"forwarded_commands",impl_->forwarded.load()},
        {"stale_commands",impl_->stale.load()},{"rejected_commands",impl_->rejected.load()}};
}
}
