// Planner (plan stage 2): the behavior tree (config/behavior.xml, planning/behavior.hpp) and
// the lane change state machine over the world model, ticked at 10 Hz, with each tick's tree
// and state machine on /ets2/planning/behavior (BehaviorStatus) for the Foxglove status board.
// Shadow mode (default): it decides and shows; drive_speed still decides and executes (the
// turn signal lane change), and nothing here commands the truck. With command:=true it sets
// the turn signal for its own lane changes (/ets2/planning/turn_signal): drive_speed presses
// the lever, and the assistance's signal starts drive_speed's turn signal lane change after
// 3 s when the gap allows.
// The world model comes from ground truth (the GT adapter below): drive_speed's assistance
// state, the driver's turn signal, the GT ego pose and traffic, and the map provider's lane
// files (the followed lane, the lanes beside, route events from --route).
#include "control/drive_session.hpp"
#include "planning/behavior.hpp"
#include <ament_index_cpp/get_package_share_directory.hpp>
#include <behaviortree_cpp/loggers/abstract_logger.h>
#include <ets2_msgs/msg/assist_state.hpp>
#include <ets2_msgs/msg/behavior_status.hpp>
#include <ets2_msgs/msg/powertrain_state.hpp>
#include <ets2_msgs/msg/traffic_state.hpp>
#include <ets2_msgs/msg/vehicle_state.hpp>
#include <geometry_msgs/msg/pose_stamped.hpp>
#include <std_msgs/msg/float32.hpp>
#include <std_msgs/msg/int8.hpp>
#include <nlohmann/json.hpp>
#include <rclcpp/rclcpp.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unordered_map>

using namespace std::chrono_literals;
using Clock=std::chrono::steady_clock;

namespace {
// What each node returned in a tick: control nodes put finished children back to idle before
// the tick ends, so the statuses are caught as they change.
class TickRecorder : public BT::StatusChangeLogger {
public:
    explicit TickRecorder(BT::TreeNode* root):StatusChangeLogger(root) {}
    std::unordered_map<const BT::TreeNode*,BT::NodeStatus> seen;
    void callback(BT::Duration,const BT::TreeNode& node,BT::NodeStatus,BT::NodeStatus status) override {
        if(status!=BT::NodeStatus::IDLE) seen[&node]=status;
    }
    void flush() override {}
};

// Depth-first nodes with their parent's index.
void flatten(const BT::TreeNode* node,int parent,std::vector<std::pair<const BT::TreeNode*,int>>& out) {
    const int index=int(out.size());
    out.push_back({node,parent});
    if(const auto* control=dynamic_cast<const BT::ControlNode*>(node)) for(const auto* child:control->children()) flatten(child,index,out);
    else if(const auto* decorator=dynamic_cast<const BT::DecoratorNode*>(node)) if(decorator->child()) flatten(decorator->child(),index,out);
}

std::optional<LanePath> read_lane(const std::string& file) {
    try {return LanePath(file);} catch(const std::exception&) {return std::nullopt;} // no lane on that side, or a file being replaced
}
std::string side_file(const std::string& path,const char* side) {
    const auto dot=path.rfind('.');
    return path.substr(0,dot)+"."+side+(dot==std::string::npos?"":path.substr(dot));
}
} // namespace

int main(int argc,char** argv) {
    rclcpp::init(argc,argv,rclcpp::InitOptions(),rclcpp::SignalHandlerOptions::None);
    int result=0;
    try {
        StopSignal stop(argc,argv);
        auto node=std::make_shared<rclcpp::Node>("ets2_planner");
        const auto tree_file=node->declare_parameter<std::string>("tree",ament_index_cpp::get_package_share_directory("ets2_autonomy")+"/config/behavior.xml");
        const auto path_file=node->declare_parameter<std::string>("path_file","");
        const bool command=node->declare_parameter("command",false);
        planning::Context c;
        c.overtake_policy=node->declare_parameter<std::string>("overtake_policy","confirm");
        if(c.overtake_policy!="auto" && c.overtake_policy!="confirm") throw std::runtime_error("overtake_policy must be auto or confirm");
        const auto started=Clock::now();
        const auto seconds=[&](Clock::time_point t) {return std::chrono::duration<double>(t-started).count();};
        // GT adapter: messages into the world model, each with its arrival time for staleness.
        ets2_msgs::msg::AssistState::SharedPtr assist;Clock::time_point assist_time{},pose_time{},traffic_time{},blocked_time{};
        int last_signal=0;
        auto as=node->create_subscription<ets2_msgs::msg::AssistState>("/ets2/drive/assist",rclcpp::QoS(10),
            [&](ets2_msgs::msg::AssistState::SharedPtr m){assist=std::move(m);assist_time=Clock::now();});
        auto ps=node->create_subscription<ets2_msgs::msg::PowertrainState>("/ets2/vehicle/powertrain",rclcpp::QoS(1),[&](ets2_msgs::msg::PowertrainState::SharedPtr m) {
            const int signal=m->turn_signal_left!=m->turn_signal_right?(m->turn_signal_left?-1:1):0;
            if(signal!=last_signal) {last_signal=signal;c.signal_since=seconds(Clock::now());}
            c.w.signal=signal;
        });
        auto vs=node->create_subscription<ets2_msgs::msg::VehicleState>("/ets2/vehicle/state",rclcpp::QoS(1),
            [&](ets2_msgs::msg::VehicleState::SharedPtr m){c.w.speed=m->speed_mps;});
        auto es=node->create_subscription<geometry_msgs::msg::PoseStamped>("/ets2/ground_truth/ego/pose",rclcpp::QoS(1),[&](geometry_msgs::msg::PoseStamped::SharedPtr m) {
            const auto& p=m->pose.position;const auto& q=m->pose.orientation;
            c.w.x=p.x;c.w.y=p.y;c.w.z=p.z;c.w.yaw=std::atan2(2*(q.w*q.z+q.x*q.y),1-2*(q.y*q.y+q.z*q.z));pose_time=Clock::now();
        });
        auto ts=node->create_subscription<ets2_msgs::msg::TrafficState>("/ets2/ground_truth/traffic",rclcpp::QoS(1),[&](ets2_msgs::msg::TrafficState::SharedPtr m) {
            c.w.objects.clear();
            if(!m->available) return;
            for(size_t i=0;i<m->id.size();++i)
                c.w.objects.push_back({m->id[i],m->x[i],m->y[i],m->yaw[i],m->speed_mps[i],m->length_m[i],m->width_m[i],
                                       i<m->z.size()?m->z[i]:NAN,i<m->height_m.size()?m->height_m[i]:NAN});
            traffic_time=Clock::now();
        });
        auto bs=node->create_subscription<std_msgs::msg::Float32>("/ets2/perception/blocked_m",rclcpp::QoS(1),[&](std_msgs::msg::Float32::SharedPtr m){c.w.blocked_m=m->data;blocked_time=Clock::now();});
        auto status=node->create_publisher<ets2_msgs::msg::BehaviorStatus>("/ets2/planning/behavior",rclcpp::QoS(1));
        auto turn_signal=node->create_publisher<std_msgs::msg::Int8>("/ets2/planning/turn_signal",rclcpp::QoS(1));

        BT::BehaviorTreeFactory factory;
        planning::register_nodes(factory,c);
        auto tree=factory.createTreeFromFile(tree_file);
        std::vector<std::pair<const BT::TreeNode*,int>> nodes;
        flatten(tree.rootNode(),-1,nodes);
        TickRecorder recorder(tree.rootNode());
        ets2_msgs::msg::BehaviorStatus message;
        for(const auto& [n,parent]:nodes) {message.node_name.push_back(n->name());message.node_kind.push_back(n->registrationName());message.node_parent.push_back(parent);}
        for(const auto* name:planning::LaneChangeFsm::names) message.fsm_state.push_back(name);
        message.shadow=!command;
        std::cout<<(command?"Planner (commands the turn signal): ":"Planner (shadow): ")<<tree_file<<", "<<nodes.size()<<" nodes -> /ets2/planning/behavior"<<std::endl;

        auto next_lanes=Clock::now();std::string last_decision;
        auto timer=node->create_wall_timer(100ms,[&] {
            const auto now=Clock::now();
            auto& w=c.w;
            w.t=seconds(now);
            // drive_speed's state is current only while it publishes (an armed session).
            const bool live=assist && now-assist_time<=500ms;
            w.acc=live && assist->acc_active;w.lcc=live && assist->lcc_state!=0;
            w.set_speed=live?assist->set_speed_mps:0;w.grade=live && std::isfinite(assist->grade)?assist->grade:0;
            w.lead=live && assist->lead_valid;w.lead_gap=live?assist->lead_gap_m:0;w.lead_speed=live?assist->lead_speed_mps:0;w.lead_id=live?assist->lead_id:0;
            w.change_request=live?assist->lane_change_request:0;w.change_active=live?assist->lane_change_active:0;
            w.signal_by=live?assist->turn_signal_by:(w.signal!=0?1:0);w.signal_cancelled=live && assist->turn_signal_cancelled;
            if(now-traffic_time>500ms) w.objects.clear();
            if(now-blocked_time>500ms) w.blocked_m=NAN;
            // The provider's lanes, once a second; the truck's place on the followed lane every tick.
            if(!path_file.empty() && now>=next_lanes) {
                next_lanes=now+1s;
                w.lane=read_lane(path_file);w.left=read_lane(side_file(path_file,"left"));w.right=read_lane(side_file(path_file,"right"));
                w.route.clear();
                try {
                    std::ifstream input(path_file);
                    const auto data=nlohmann::json::parse(input);
                    for(const auto& e:data.value("events",nlohmann::json::array()))
                        if(e.value("type","")=="route_lane_mismatch")
                            w.route.push_back({e.value("s",0.0),e.value("lanes_ok",std::vector<int>{}),e.value("lane",0)});
                } catch(const std::exception&) {}
            }
            if(w.lane && now-pose_time<=500ms) w.path_s=w.lane->start_s+w.lane->project(w.x,w.y).s;
            c.begin();recorder.seen.clear();
            tree.tickOnce();
            c.end();
            message.header.stamp=node->now();message.header.frame_id="";
            message.node_status.assign(nodes.size(),0);
            for(size_t i=0;i<nodes.size();++i) {
                const auto found=recorder.seen.find(nodes[i].first);
                const auto s=found!=recorder.seen.end()?found->second:nodes[i].first->status();
                message.node_status[i]=s==BT::NodeStatus::RUNNING?1:s==BT::NodeStatus::SUCCESS?2:s==BT::NodeStatus::FAILURE?3:0;
            }
            message.fsm_active=c.fsm.state();message.fsm_note=c.fsm.note();message.decision=c.decision;
            status->publish(message);
            if(command) {std_msgs::msg::Int8 wanted;wanted.data=int8_t(planning::turn_signal(c));turn_signal->publish(wanted);}
            if(c.decision!=last_decision) {std::cout<<"Decision: "<<c.decision<<std::endl;last_decision=c.decision;}
        });
        while(rclcpp::ok() && !stop.requested()) rclcpp::spin_some(node),std::this_thread::sleep_for(5ms);
    } catch(const std::exception& e) {std::cerr<<e.what()<<std::endl;result=1;}
    rclcpp::shutdown();
    return result;
}
