// HUD (docs/24 §0): one MarkerArray, /ets2/hud/markers at 30 Hz, from drive_speed's
// assistance state (/ets2/drive/assist), LCC's path (/ets2/drive/plan), the lane in use
// (/ets2/drive/lane) and GT traffic. Namespaces:
// - road/<kind>: the marks painted on the game's road (hud_marks.hpp; the relay draws
//   LINE_STRIP markers of road/ only): lcc_path, acc, lane_change.
// - scene/<kind>: Foxglove only: lane (centre and edges), traffic (boxes, the lead
//   highlighted), ego (box), status (text above the truck).
// Foxglove's 3D panel toggles each namespace. A separate process, so presentation never
// touches the 50 Hz control loop. Assistance state older than 0.3 s (standby, released,
// paused) clears the road marks; the scene stays while GT arrives.
#include "control/drive_session.hpp"
#include "control/hud_marks.hpp"
#include <rclcpp/rclcpp.hpp>
#include <ets2_msgs/msg/assist_state.hpp>
#include <ets2_msgs/msg/traffic_state.hpp>
#include <geometry_msgs/msg/pose_stamped.hpp>
#include <nav_msgs/msg/path.hpp>
#include <visualization_msgs/msg/marker_array.hpp>
#include <cstdio>
#include <iostream>
#include <optional>

using namespace std::chrono_literals;
using Clock=std::chrono::steady_clock;
using visualization_msgs::msg::Marker;

int main(int argc,char** argv) {
    rclcpp::init(argc,argv,rclcpp::InitOptions(),rclcpp::SignalHandlerOptions::None);
    int result=0;
    try {
        StopSignal stop(argc,argv);
        auto node=std::make_shared<rclcpp::Node>("ets2_hud");
        ets2_msgs::msg::AssistState::SharedPtr assist;Clock::time_point assist_time{};
        ets2_msgs::msg::TrafficState::SharedPtr traffic;Clock::time_point traffic_time{};
        hud::Points plan,lane;
        geometry_msgs::msg::PoseStamped::SharedPtr ego;Clock::time_point ego_time{};
        auto as=node->create_subscription<ets2_msgs::msg::AssistState>("/ets2/drive/assist",rclcpp::QoS(10),
            [&](ets2_msgs::msg::AssistState::SharedPtr m){assist=std::move(m);assist_time=Clock::now();});
        const auto points=[](const nav_msgs::msg::Path& m) {hud::Points p;for(const auto& q:m.poses) p.push_back({q.pose.position.x,q.pose.position.y});return p;};
        auto ps=node->create_subscription<nav_msgs::msg::Path>("/ets2/drive/plan",rclcpp::QoS(1).best_effort(),[&](nav_msgs::msg::Path::SharedPtr m){plan=points(*m);});
        auto ls=node->create_subscription<nav_msgs::msg::Path>("/ets2/drive/lane",rclcpp::QoS(1),[&](nav_msgs::msg::Path::SharedPtr m){lane=points(*m);});
        auto ts=node->create_subscription<ets2_msgs::msg::TrafficState>("/ets2/ground_truth/traffic",rclcpp::QoS(1),
            [&](ets2_msgs::msg::TrafficState::SharedPtr m){traffic=std::move(m);traffic_time=Clock::now();});
        auto es=node->create_subscription<geometry_msgs::msg::PoseStamped>("/ets2/ground_truth/ego/pose",rclcpp::QoS(1),
            [&](geometry_msgs::msg::PoseStamped::SharedPtr m){ego=std::move(m);ego_time=Clock::now();});
        auto markers=node->create_publisher<visualization_msgs::msg::MarkerArray>("/ets2/hud/markers",rclcpp::QoS(1).best_effort());
        hud::Composer composer;
        bool shown=false;auto previous=Clock::now();
        std::cout<<"HUD: /ets2/drive/assist, plan, lane + GT -> /ets2/hud/markers (road/* painted, scene/* Foxglove)"<<std::endl;
        auto timer=node->create_wall_timer(33ms,[&] {
            const auto now=Clock::now();
            const auto dt=std::chrono::duration<double>(now-previous).count();previous=now;
            const bool live=ego && now-ego_time<=300ms;
            const bool assisted=live && assist && now-assist_time<=300ms;
            if(!live) {
                if(shown) {
                    visualization_msgs::msg::MarkerArray array;Marker m;m.header.frame_id="world";m.action=Marker::DELETEALL;
                    array.markers.push_back(m);markers->publish(array);shown=false;
                }
                composer.update({},dt);
                return;
            }
            const auto& pose=ego->pose;const auto& q=pose.orientation;
            const double ex=pose.position.x,ey=pose.position.y,ez=pose.position.z;
            const double yaw=std::atan2(2*(q.w*q.z+q.x*q.y),1-2*(q.y*q.y+q.z*q.z));
            // Simulation time: the relay's TTL and stamp checks run on it.
            const auto stamp=assisted?assist->header.stamp:ego->header.stamp;
            visualization_msgs::msg::MarkerArray array;
            // DELETEALL first: the number of markers per namespace changes from frame to frame.
            {Marker m;m.header.frame_id="world";m.header.stamp=stamp;m.action=Marker::DELETEALL;array.markers.push_back(m);}
            int id=0;
            const auto marker=[&](const std::string& ns,int32_t type) {
                Marker m;m.header.frame_id="world";m.header.stamp=stamp;m.ns=ns;m.id=id++;m.type=type;m.action=Marker::ADD;m.pose.orientation.w=1;
                return m;
            };
            const auto colour=[](Marker& m,float r,float g,float b,float a) {m.color.r=r;m.color.g=g;m.color.b=b;m.color.a=a;};
            const auto line=[&](const std::string& ns,const hud::Points& pts,double width,double px,std::array<float,4> c,double z) {
                auto m=marker(ns,Marker::LINE_STRIP);m.scale.x=width;m.scale.y=px;colour(m,c[0],c[1],c[2],c[3]);
                for(const auto& [x,y]:pts) {geometry_msgs::msg::Point p;p.x=x;p.y=y;p.z=z;m.points.push_back(p);}
                array.markers.push_back(std::move(m));
            };
            // Road marks (painted on the game's road too).
            hud::Inputs in;
            if(assisted) {
                const auto& a=*assist;
                in.lcc_steering=a.lcc_state==1;in.acc=a.acc_active;in.driver_throttle=a.driver_throttle;
                in.mark=a.acc_mark;in.mark_x=a.acc_mark_x;in.mark_y=a.acc_mark_y;in.mark_heading=a.acc_mark_heading;in.lane_width=a.lane_width_m;
                in.emergency=a.acc_mode==6; // AccMpc::Emergency
                in.accel_target=a.accel_target_mps2;
                in.change_request=a.lane_change_active?0:a.lane_change_request;
                in.plan=plan;
            }
            in.ego_x=ex;in.ego_y=ey;in.ego_yaw=yaw;
            for(const auto& s:composer.update(in,dt)) line("road/"+s.kind,s.points,s.width_m,s.screen_px,s.rgba,ez);
            // Scene (Foxglove only): the lane in use with its edges.
            if(assisted && lane.size()>=2) {
                line("scene/lane",lane,.08,0,{.6f,.65f,.7f,.5f},ez);
                if(const auto w=assist->lane_width_m;w>0) for(const int side:{-1,1}) {
                    hud::Points edge;
                    for(size_t i=0;i<lane.size();++i) {
                        const auto& a=lane[i==0?0:i-1];const auto& b=lane[i==0?1:i];
                        const double l=std::hypot(b[0]-a[0],b[1]-a[1]);
                        if(!(l>1e-6)) continue;
                        edge.push_back({lane[i][0]-side*w/2*(b[1]-a[1])/l,lane[i][1]+side*w/2*(b[0]-a[0])/l});
                    }
                    line("scene/lane",edge,.05,0,{.6f,.65f,.7f,.3f},ez);
                }
            }
            // Traffic within 150 m; the lead (rear bumper at the ACC mark) in the mark's colour.
            if(traffic && now-traffic_time<=500ms) for(size_t i=0;i<traffic->id.size();++i) {
                const double x=traffic->x[i],y=traffic->y[i],h=traffic->yaw[i],length=traffic->length_m[i],width=traffic->width_m[i];
                if(std::hypot(x-ex,y-ey)>150) continue;
                const bool lead=in.mark==2 && std::hypot(x-length/2*std::cos(h)-in.mark_x,y-length/2*std::sin(h)-in.mark_y)<3;
                auto m=marker("scene/traffic",Marker::CUBE);
                m.pose.position.x=x;m.pose.position.y=y;m.pose.position.z=ez+1.5;
                m.pose.orientation.z=std::sin(h/2);m.pose.orientation.w=std::cos(h/2);
                m.scale.x=length;m.scale.y=width;m.scale.z=3;
                if(lead) colour(m,.35f,.95f,.45f,.7f);else colour(m,.7f,.7f,.75f,.35f);
                array.markers.push_back(std::move(m));
            }
            // The truck (cab and chassis, about 7 x 2.5 m ahead of the drive axle).
            {
                auto m=marker("scene/ego",Marker::CUBE);
                m.pose.position.x=ex+1.5*std::cos(yaw);m.pose.position.y=ey+1.5*std::sin(yaw);m.pose.position.z=ez+1.75;
                m.pose.orientation=q;m.scale.x=7;m.scale.y=2.5;m.scale.z=3.5;colour(m,.3f,.55f,.95f,.6f);
                array.markers.push_back(std::move(m));
            }
            // Status above the truck.
            if(assisted) {
                const auto& a=*assist;
                char text[160];
                const char* lcc[]={"LCC off","LCC","LCC yields","LCC searching"};
                std::snprintf(text,sizeof text,"%s %.0f km/h%s%s | %s%s",a.acc_active?"ACC":"ACC off",a.set_speed_mps*3.6,
                    a.lead_valid?"  lead ":"",a.lead_valid?(std::to_string(int(a.lead_gap_m))+" m").c_str():"",
                    lcc[std::min<int>(a.lcc_state,3)],a.lane_change_active?(a.lane_change_active<0?"  <- lane change":"  lane change ->"):"");
                auto m=marker("scene/status",Marker::TEXT_VIEW_FACING);
                m.pose.position.x=ex;m.pose.position.y=ey;m.pose.position.z=ez+6;m.scale.z=1.2;colour(m,1,1,1,.95f);m.text=text;
                array.markers.push_back(std::move(m));
            }
            markers->publish(array);shown=true;
        });
        while(rclcpp::ok() && !stop.requested()) rclcpp::spin_some(node),std::this_thread::sleep_for(5ms);
        if(shown) {visualization_msgs::msg::MarkerArray array;Marker m;m.header.frame_id="world";m.action=Marker::DELETEALL;array.markers.push_back(m);markers->publish(array);}
    } catch(const std::exception& e) {std::cerr<<e.what()<<std::endl;result=1;}
    rclcpp::shutdown();
    return result;
}
