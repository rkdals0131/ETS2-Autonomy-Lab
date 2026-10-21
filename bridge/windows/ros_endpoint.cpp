#include "ros_endpoint.hpp"
#include "relay_workers.hpp"
#include <rclcpp/rclcpp.hpp>
#include <std_srvs/srv/set_bool.hpp>
#include <ets2_msgs/msg/drive_command.hpp>
#include <ets2_msgs/msg/overlay_style.hpp>
#include <ets2_msgs/srv/drive_control.hpp>
#include <nav_msgs/msg/path.hpp>
#include <visualization_msgs/msg/marker_array.hpp>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <vector>

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
    // HUD marks (docs/24): forwarded to the relay's pose transform and shared memory.
    MarksSink overlay_sink;
    rclcpp::Subscription<visualization_msgs::msg::MarkerArray>::SharedPtr overlay_marks;
    rclcpp::Subscription<nav_msgs::msg::Path>::SharedPtr overlay_paths;
    rclcpp::Subscription<ets2_msgs::msg::OverlayStyle>::SharedPtr overlay_styles;
    std::optional<ets2_msgs::msg::OverlayStyle> overlay_style; // executor thread only
    std::chrono::steady_clock::time_point overlay_style_time{};
    std::atomic<bool> overlay{false};
    std::atomic<uint64_t> overlay_received{0},overlay_rejected{0};
    rclcpp::TimerBase::SharedPtr demand_timer;
    std::atomic<std::shared_ptr<const Demand>> demand{std::make_shared<const Demand>()};
    std::atomic<bool> capture{true};
    std::atomic<bool> camera_enabled{true},lidar_enabled{true};
    std::chrono::nanoseconds max_age{};
    // Newest validated command; older ones are superseded, like the old TCP slot.
    std::mutex drive_mutex;std::condition_variable drive_ready;
    std::optional<json> drive_request;uint64_t drive_sent_ns=0;bool stopping=false;
    // Turn signal presses: the counts seen in this owner's epoch, and the presses (-1 left,
    // 1 right) waiting for the forward thread.
    std::string signal_owner;uint64_t signal_epoch=0;uint32_t signal_left=0,signal_right=0;
    std::vector<std::tuple<std::string,uint64_t,int>> signal_presses;
    Latency command_age;
    std::atomic<uint64_t> stale{0},rejected{0},forwarded{0},signals{0},signals_rejected{0};
    // Publishes that took 50 ms or more, newest 16 with wall time, to line
    // up with subscriber joins/exits when send_elapsed spikes.
    std::mutex slow_mutex;std::deque<json> slow;std::atomic<uint64_t> slow_count{0};
    // Created after rclcpp::init: an executor takes the default context when constructed.
    std::unique_ptr<rclcpp::executors::SingleThreadedExecutor> executor;
    std::jthread spin,forward;
};

RosEndpoint::RosEndpoint(int argc,char** argv,Control control,MarksSink overlay):impl_(std::make_unique<Impl>()) {
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
                if(command->owner!=m.signal_owner || command->epoch!=m.signal_epoch) {
                    m.signal_owner=command->owner;m.signal_epoch=command->epoch;m.signal_left=command->left_signal_presses;m.signal_right=command->right_signal_presses;
                }
                // At most the loader's queue (4) per command; the rest follow with later commands.
                for(int side:{-1,1}) {
                    auto& seen=side<0?m.signal_left:m.signal_right;const auto count=side<0?command->left_signal_presses:command->right_signal_presses;
                    while(seen!=count && m.signal_presses.size()<4) {++seen;m.signal_presses.emplace_back(command->owner,command->epoch,side);}
                }
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
    const auto stamp_us=[](const builtin_interfaces::msg::Time& stamp) {return stamp.sec<0?uint64_t{0}:uint64_t(stamp.sec)*1000000+stamp.nanosec/1000;};
    const auto ttl_ms=[&m] {return static_cast<uint64_t>(std::max<int64_t>(0,m.node->get_parameter("overlay_ttl_ms").as_int()));};
    const auto deliver=[&m](const OverlayMarks& marks) {
        // No strokes clears the decal now (the writer marks the content invalid).
        bool valid=marks.strokes.empty() || marks.frame=="base_link" || marks.frame=="world";
        for(const auto& s:marks.strokes) valid=valid && s.points.size()>=2 && std::isfinite(s.width_m) && s.width_m>0 && s.width_m<=16 &&
            std::isfinite(s.min_px) && s.min_px>=0 && s.min_px<=64 && std::all_of(s.rgba.begin(),s.rgba.end(),[](float c){return std::isfinite(c) && c>=0 && c<=1;});
        if(!valid) {
            ++m.overlay_rejected;
            RCLCPP_WARN_THROTTLE(m.node->get_logger(),*m.node->get_clock(),2000,"Rejected overlay marks: frame '%s', %zu strokes",marks.frame.c_str(),marks.strokes.size());
            return;
        }
        ++m.overlay_received;
        if(m.overlay_sink) m.overlay_sink(marks);
    };
    // HUD marks: /ets2/hud/markers, the one MarkerArray of the HUD (hud_node.cpp). The road
    // is painted with its LINE_STRIP markers in namespaces road/*; everything else there is
    // for Foxglove. Markers in one frame. scale.x is the width on the road (m),
    // scale.y the minimum width on screen (pixels at 1080 lines; 0 none), color the paint
    // (a: strength), a nonzero lifetime the TTL. Other marker types are ignored. DELETEALL drops
    // the strokes before it (rviz semantics); an array that ends without strokes clears.
    m.overlay_marks=m.node->create_subscription<visualization_msgs::msg::MarkerArray>("/ets2/hud/markers",rclcpp::QoS(1).best_effort(),
        [stamp_us,ttl_ms,deliver](const visualization_msgs::msg::MarkerArray::SharedPtr message) {
            OverlayMarks marks;marks.ttl_ms=ttl_ms();
            bool frame_set=false;
            for(const auto& marker:message->markers) {
                if(marker.action==visualization_msgs::msg::Marker::DELETEALL) {marks.strokes.clear();continue;}
                if(marker.action!=visualization_msgs::msg::Marker::ADD || marker.type!=visualization_msgs::msg::Marker::LINE_STRIP || !marker.ns.starts_with("road/")) continue;
                if(!frame_set) {
                    marks.frame=marker.header.frame_id;marks.stamp_us=stamp_us(marker.header.stamp);frame_set=true;
                    const auto& life=marker.lifetime;
                    if(life.sec>0 || life.nanosec>0) marks.ttl_ms=uint64_t(std::max(0,life.sec))*1000+life.nanosec/1000000;
                } else if(marker.header.frame_id!=marks.frame) marks.frame="mixed";
                OverlayStrokeInput stroke;
                stroke.width_m=static_cast<float>(marker.scale.x);stroke.min_px=static_cast<float>(marker.scale.y/2);
                stroke.rgba={marker.color.r,marker.color.g,marker.color.b,marker.color.a};
                const auto& o=marker.pose.orientation;const auto& t=marker.pose.position;
                const auto rotation=from_quat({o.w,o.x,o.y,o.z});
                for(const auto& p:marker.points) stroke.points.push_back(add(V{t.x,t.y,t.z},mul(rotation,V{p.x,p.y,p.z})));
                marks.strokes.push_back(std::move(stroke));
            }
            deliver(marks);
        });
    m.overlay_paths=m.node->create_subscription<nav_msgs::msg::Path>("/ets2/overlay/path",rclcpp::QoS(1).best_effort(),
        [&m,stamp_us,ttl_ms,deliver](const nav_msgs::msg::Path::SharedPtr message) {
            OverlayMarks marks;marks.frame=message->header.frame_id;marks.stamp_us=stamp_us(message->header.stamp);marks.ttl_ms=ttl_ms();
            if(!message->poses.empty()) {
                OverlayStrokeInput stroke;
                stroke.width_m=static_cast<float>(m.node->get_parameter("overlay_width_m").as_double());
                stroke.rgba[3]=static_cast<float>(m.node->get_parameter("overlay_alpha").as_double());
                if(m.overlay_style && std::chrono::steady_clock::now()-m.overlay_style_time<=std::chrono::seconds(1)) {
                    const auto& style=*m.overlay_style;
                    stroke.width_m=style.width_m;stroke.rgba={style.color.r,style.color.g,style.color.b,style.color.a};
                }
                for(const auto& pose:message->poses) stroke.points.push_back({pose.pose.position.x,pose.pose.position.y,pose.pose.position.z});
                marks.strokes.push_back(std::move(stroke));
            }
            deliver(marks);
        });
    // Rendered sensors the launcher allows (settable while running): a subscriber to a disabled
    // camera or LiDAR topic does not start rendering. State sensors are not affected.
    m.node->declare_parameter("camera_enabled",true);
    m.node->declare_parameter("lidar_enabled",true);
    m.demand_timer=m.node->create_wall_timer(200ms,[&m] {
        const bool camera=m.node->get_parameter("camera_enabled").as_bool(),lidar=m.node->get_parameter("lidar_enabled").as_bool();
        m.camera_enabled=camera;m.lidar_enabled=lidar;
        const auto allowed=[&](const std::string& name) {
            if(name.starts_with("/ets2/lidar/")) return lidar;
            const bool rendered=name.starts_with("/ets2/camera/") || name.starts_with("/ets2/frame_info") ||
                (name.starts_with("/ets2/ground_truth/C_") && (name.ends_with("/objects") || name.ends_with("/markers")));
            return camera || !rendered;
        };
        auto demand=std::make_shared<Demand>();
        for(const auto& [name,entry]:m.publishers) if(entry->publisher->get_subscription_count()>0 && allowed(name)) demand->insert(name);
        m.demand.store(std::move(demand));
        m.overlay=m.node->get_parameter("overlay_enabled").as_bool() || m.overlay_paths->get_publisher_count()>0 || m.overlay_marks->get_publisher_count()>0;
    });
    m.forward=std::jthread([&m] {
        for(;;) {
            json request;uint64_t sent=0;
            std::vector<std::tuple<std::string,uint64_t,int>> presses;
            {
                std::unique_lock lock(m.drive_mutex);
                m.drive_ready.wait(lock,[&]{return m.stopping || m.drive_request.has_value();});
                if(m.stopping) return;
                request=std::move(*m.drive_request);m.drive_request.reset();sent=m.drive_sent_ns;
                presses.swap(m.signal_presses);
            }
            for(const auto& [owner,epoch,side]:presses) {
                try {
                    if(m.control({{"cmd","drive"},{"action","signal"},{"drive_owner",owner},{"epoch",epoch},{"side",side}}).value("accepted",false)) ++m.signals;
                    else ++m.signals_rejected;
                } catch(const std::exception& e) {
                    ++m.signals_rejected;
                    RCLCPP_WARN_THROTTLE(m.node->get_logger(),*m.node->get_clock(),1000,"Turn signal press failed: %s",e.what());
                }
            }
            presses.clear();
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
    m.demand_timer.reset();m.overlay_marks.reset();m.overlay_paths.reset();m.overlay_styles.reset();m.drive_commands.reset();m.drive_service.reset();m.capture_service.reset();
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
json RosEndpoint::sensors_enabled() const {return {{"camera",impl_->camera_enabled.load()},{"lidar",impl_->lidar_enabled.load()}};}
json RosEndpoint::diagnostics() {
    return {{"command_age",impl_->command_age.snapshot()},{"forwarded_commands",impl_->forwarded.load()},
        {"stale_commands",impl_->stale.load()},{"rejected_commands",impl_->rejected.load()},
        {"signal_presses",impl_->signals.load()},{"rejected_signal_presses",impl_->signals_rejected.load()},
        {"overlay_paths",impl_->overlay_received.load()},{"rejected_overlay_paths",impl_->overlay_rejected.load()}};
}
}
