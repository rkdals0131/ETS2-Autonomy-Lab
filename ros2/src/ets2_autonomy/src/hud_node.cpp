// HUD (docs/24 §0): turns drive_speed's assistance state (/ets2/drive/assist) and LCC's path
// (/ets2/drive/plan) into marks (hud_marks.hpp) on /ets2/overlay/marks, 30 Hz: LINE_STRIP
// markers the relay paints on the road and Foxglove shows in 3D. A separate process, so
// presentation never touches the 50 Hz control loop. Assistance state older than 0.3 s
// (standby, released, paused) clears the marks once.
#include "control/drive_session.hpp"
#include "control/hud_marks.hpp"
#include <rclcpp/rclcpp.hpp>
#include <ets2_msgs/msg/assist_state.hpp>
#include <geometry_msgs/msg/pose_stamped.hpp>
#include <nav_msgs/msg/path.hpp>
#include <visualization_msgs/msg/marker_array.hpp>
#include <iostream>
#include <optional>

using namespace std::chrono_literals;
using Clock=std::chrono::steady_clock;

int main(int argc,char** argv) {
    rclcpp::init(argc,argv,rclcpp::InitOptions(),rclcpp::SignalHandlerOptions::None);
    int result=0;
    try {
        StopSignal stop(argc,argv);
        auto node=std::make_shared<rclcpp::Node>("ets2_hud");
        ets2_msgs::msg::AssistState::SharedPtr assist;Clock::time_point assist_time{};
        hud::Points plan;
        double ego_x=0,ego_y=0,ego_z=0,ego_yaw=0;bool pose=false;
        auto as=node->create_subscription<ets2_msgs::msg::AssistState>("/ets2/drive/assist",rclcpp::QoS(10),
            [&](ets2_msgs::msg::AssistState::SharedPtr m){assist=std::move(m);assist_time=Clock::now();});
        auto ps=node->create_subscription<nav_msgs::msg::Path>("/ets2/drive/plan",rclcpp::QoS(1).best_effort(),[&](nav_msgs::msg::Path::SharedPtr m) {
            plan.clear();for(const auto& p:m->poses) plan.push_back({p.pose.position.x,p.pose.position.y});
        });
        auto es=node->create_subscription<geometry_msgs::msg::PoseStamped>("/ets2/ground_truth/ego/pose",rclcpp::QoS(1),[&](geometry_msgs::msg::PoseStamped::SharedPtr m) {
            const auto& q=m->pose.orientation;
            ego_x=m->pose.position.x;ego_y=m->pose.position.y;ego_z=m->pose.position.z;
            ego_yaw=std::atan2(2*(q.w*q.z+q.x*q.y),1-2*(q.y*q.y+q.z*q.z));pose=true;
        });
        auto marks=node->create_publisher<visualization_msgs::msg::MarkerArray>("/ets2/overlay/marks",rclcpp::QoS(1).best_effort());
        hud::Composer composer;
        bool shown=false;auto previous=Clock::now();
        const auto clear=[&](const builtin_interfaces::msg::Time& stamp) {
            visualization_msgs::msg::MarkerArray array;visualization_msgs::msg::Marker m;
            m.header.frame_id="world";m.header.stamp=stamp;m.action=visualization_msgs::msg::Marker::DELETEALL;
            array.markers.push_back(m);marks->publish(array);
        };
        std::cout<<"HUD: /ets2/drive/assist + /ets2/drive/plan -> /ets2/overlay/marks"<<std::endl;
        auto timer=node->create_wall_timer(33ms,[&] {
            const auto now=Clock::now();
            const auto dt=std::chrono::duration<double>(now-previous).count();previous=now;
            if(!assist || !pose || now-assist_time>300ms) {
                if(shown) {clear(assist?assist->header.stamp:builtin_interfaces::msg::Time{});shown=false;}
                composer.update({},dt);
                return;
            }
            const auto& a=*assist;
            hud::Inputs in;
            in.lcc_steering=a.lcc_state==1;in.acc=a.acc_active;in.driver_throttle=a.driver_throttle;
            in.mark=a.acc_mark;in.mark_x=a.acc_mark_x;in.mark_y=a.acc_mark_y;in.mark_heading=a.acc_mark_heading;in.lane_width=a.lane_width_m;
            in.emergency=a.acc_mode==6; // AccMpc::Emergencyin.accel_target=a.accel_target_mps2;
            in.change_request=a.lane_change_active?0:a.lane_change_request;
            in.plan=plan;in.ego_x=ego_x;in.ego_y=ego_y;in.ego_yaw=ego_yaw;
            const auto strokes=composer.update(in,dt);
            if(strokes.empty()) {if(shown) {clear(a.header.stamp);shown=false;}return;}
            visualization_msgs::msg::MarkerArray array;
            // DELETEALL first: Foxglove keeps markers by id, and the count changes.
            {visualization_msgs::msg::Marker m;m.header.frame_id="world";m.header.stamp=a.header.stamp;m.action=visualization_msgs::msg::Marker::DELETEALL;array.markers.push_back(m);}
            int id=0;
            for(const auto& s:strokes) {
                visualization_msgs::msg::Marker m;
                m.header.frame_id="world";m.header.stamp=a.header.stamp;m.ns="hud";m.id=id++;
                m.type=visualization_msgs::msg::Marker::LINE_STRIP;m.action=visualization_msgs::msg::Marker::ADD;
                m.pose.orientation.w=1;m.scale.x=s.width_m;m.scale.y=s.screen_px;
                m.color.r=s.rgba[0];m.color.g=s.rgba[1];m.color.b=s.rgba[2];m.color.a=s.rgba[3];
                // z at the truck: Foxglove draws them on the road; the decal uses its own ground height.
                for(const auto& [x,y]:s.points) {geometry_msgs::msg::Point p;p.x=x;p.y=y;p.z=ego_z;m.points.push_back(p);}
                array.markers.push_back(std::move(m));
            }
            marks->publish(array);shown=true;
        });
        while(rclcpp::ok() && !stop.requested()) rclcpp::spin_some(node),std::this_thread::sleep_for(5ms);
        if(shown) clear(assist?assist->header.stamp:builtin_interfaces::msg::Time{});
    } catch(const std::exception& e) {std::cerr<<e.what()<<std::endl;result=1;}
    rclcpp::shutdown();
    return result;
}
