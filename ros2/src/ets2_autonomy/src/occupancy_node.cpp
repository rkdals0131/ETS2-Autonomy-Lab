// LiDAR occupancy fused with GT traffic (plan stage 5, perception/occupancy.hpp), 10 Hz:
// - /ets2/perception/occupancy (OccupancyGrid, world, ego-aligned 120 x 40 m, 0.5 m): -1
//   unknown, 0 observed road, 60 an obstacle a GT vehicle explains, 100 one it does not;
// - /ets2/perception/obstacles (MarkerArray, Foxglove): unexplained obstacle points red,
//   explained grey, and a bar across the lane at the first blocked stretch;
// - /ets2/perception/blocked_m (Float32): distance along the followed lane (provider file)
//   to the first unexplained obstacle; inf when clear, NaN without LiDAR.
// The LiDAR renders only while its topics have subscribers and the relay's lidar_enabled is
// on, so without LiDAR this node publishes NaN and costs nothing in the game.
#include "control/drive_session.hpp"
#include "control/path_following.hpp"
#include "perception/occupancy.hpp"
#include <rclcpp/rclcpp.hpp>
#include <ets2_msgs/msg/assist_state.hpp>
#include <ets2_msgs/msg/traffic_state.hpp>
#include <geometry_msgs/msg/pose_stamped.hpp>
#include <nav_msgs/msg/occupancy_grid.hpp>
#include <sensor_msgs/msg/point_cloud2.hpp>
#include <std_msgs/msg/float32.hpp>
#include <tf2/LinearMath/Matrix3x3.h>
#include <tf2/LinearMath/Quaternion.h>
#include <tf2_ros/buffer.h>
#include <tf2_ros/transform_listener.h>
#include <visualization_msgs/msg/marker_array.hpp>
#include <cstring>
#include <iostream>
#include <map>

using namespace std::chrono_literals;
using Clock=std::chrono::steady_clock;

namespace {
double yaw_of(const geometry_msgs::msg::Quaternion& q) {return std::atan2(2*(q.w*q.z+q.x*q.y),1-2*(q.y*q.y+q.z*q.z));}
}

int main(int argc,char** argv) {
    rclcpp::init(argc,argv,rclcpp::InitOptions(),rclcpp::SignalHandlerOptions::None);
    int result=0;
    try {
        StopSignal stop(argc,argv);
        auto node=std::make_shared<rclcpp::Node>("ets2_occupancy");
        const auto path_file=node->declare_parameter<std::string>("path_file","");
        tf2_ros::Buffer tf(node->get_clock());tf2_ros::TransformListener listener(tf);
        // The latest cloud of each LiDAR in world coordinates, with its arrival.
        struct Cloud {std::vector<perception::Point> points;Clock::time_point at;};
        std::map<std::string,Cloud> clouds;
        std::vector<rclcpp::Subscription<sensor_msgs::msg::PointCloud2>::SharedPtr> lidar;
        for(const char* name:{"L_F","L_PL","L_PR"})
            lidar.push_back(node->create_subscription<sensor_msgs::msg::PointCloud2>(std::string("/ets2/lidar/")+name+"/points",rclcpp::SensorDataQoS(),
                [&,name](sensor_msgs::msg::PointCloud2::SharedPtr m) {
                    int ox=-1,oy=-1,oz=-1;
                    for(const auto& f:m->fields) if(f.datatype==sensor_msgs::msg::PointField::FLOAT32) {if(f.name=="x") ox=int(f.offset);else if(f.name=="y") oy=int(f.offset);else if(f.name=="z") oz=int(f.offset);}
                    if(ox<0 || oy<0 || oz<0) return;
                    geometry_msgs::msg::TransformStamped t;
                    try {t=tf.lookupTransform("world",m->header.frame_id,rclcpp::Time(m->header.stamp));}
                    catch(const tf2::TransformException&) {
                        try {t=tf.lookupTransform("world",m->header.frame_id,tf2::TimePointZero);} catch(const tf2::TransformException&) {return;}
                    }
                    const auto& r=t.transform.rotation;const auto& o=t.transform.translation;
                    const tf2::Matrix3x3 R(tf2::Quaternion(r.x,r.y,r.z,r.w));
                    auto& cloud=clouds[name];cloud.points.clear();cloud.points.reserve(size_t(m->width)*m->height);
                    for(size_t i=0;i+m->point_step<=m->data.size();i+=m->point_step) {
                        float x,y,z;std::memcpy(&x,&m->data[i+ox],4);std::memcpy(&y,&m->data[i+oy],4);std::memcpy(&z,&m->data[i+oz],4);
                        if(!std::isfinite(x)) continue;
                        const tf2::Vector3 w=R*tf2::Vector3(x,y,z);
                        cloud.points.push_back({w.x()+o.x,w.y()+o.y,w.z()+o.z});
                    }
                    cloud.at=Clock::now();
                }));
        geometry_msgs::msg::PoseStamped::SharedPtr ego,trailer;Clock::time_point ego_time{},trailer_time{},traffic_time{};
        ets2_msgs::msg::TrafficState::SharedPtr traffic;
        double truck_front=5.3,truck_rear=-2.5;
        auto es=node->create_subscription<geometry_msgs::msg::PoseStamped>("/ets2/ground_truth/ego/pose",rclcpp::QoS(1),[&](geometry_msgs::msg::PoseStamped::SharedPtr m){ego=std::move(m);ego_time=Clock::now();});
        auto trs=node->create_subscription<geometry_msgs::msg::PoseStamped>("/ets2/ground_truth/trailer/pose",rclcpp::QoS(1),[&](geometry_msgs::msg::PoseStamped::SharedPtr m){trailer=std::move(m);trailer_time=Clock::now();});
        auto ts=node->create_subscription<ets2_msgs::msg::TrafficState>("/ets2/ground_truth/traffic",rclcpp::QoS(1),[&](ets2_msgs::msg::TrafficState::SharedPtr m){traffic=std::move(m);traffic_time=Clock::now();});
        auto as=node->create_subscription<ets2_msgs::msg::AssistState>("/ets2/drive/assist",rclcpp::QoS(10),[&](ets2_msgs::msg::AssistState::SharedPtr m) {
            if(m->truck_front_m>0) {truck_front=m->truck_front_m;truck_rear=m->truck_rear_m;}
        });
        auto grid_pub=node->create_publisher<nav_msgs::msg::OccupancyGrid>("/ets2/perception/occupancy",rclcpp::QoS(1));
        auto marker_pub=node->create_publisher<visualization_msgs::msg::MarkerArray>("/ets2/perception/obstacles",rclcpp::QoS(1));
        auto blocked_pub=node->create_publisher<std_msgs::msg::Float32>("/ets2/perception/blocked_m",rclcpp::QoS(1));
        std::optional<LanePath> lane;auto next_lane=Clock::now();
        std::cout<<"Occupancy: /ets2/lidar/*/points + GT -> /ets2/perception/{occupancy,obstacles,blocked_m}"<<std::endl;
        double last_blocked=NAN;
        auto timer=node->create_wall_timer(100ms,[&] {
            const auto now=Clock::now();
            std_msgs::msg::Float32 blocked;blocked.data=NAN;
            std::vector<perception::Point> points;
            for(const auto& [name,cloud]:clouds) if(now-cloud.at<=300ms) points.insert(points.end(),cloud.points.begin(),cloud.points.end());
            if(points.empty() || !ego || now-ego_time>300ms) {blocked_pub->publish(blocked);return;}
            const auto& p=ego->pose.position;const double yaw=yaw_of(ego->pose.orientation);
            // The truck and its trailer (hud_node's estimates: fifth wheel 0.5 m ahead of the drive
            // axle, 13.6 m trailer whose front is 1.6 m ahead of the kingpin).
            std::vector<perception::Box> own{{p.x+(truck_front+truck_rear)/2*std::cos(yaw),p.y+(truck_front+truck_rear)/2*std::sin(yaw),yaw,truck_front-truck_rear,2.55}};
            if(trailer && now-trailer_time<=500ms) {
                const double ty=yaw_of(trailer->pose.orientation),kx=p.x+.5*std::cos(yaw),ky=p.y+.5*std::sin(yaw),centre=1.6-13.6/2;
                own.push_back({kx+centre*std::cos(ty),ky+centre*std::sin(ty),ty,13.6,2.55});
            }
            std::vector<perception::Box> objects;
            if(traffic && now-traffic_time<=500ms) for(size_t i=0;i<traffic->id.size();++i)
                objects.push_back({traffic->x[i],traffic->y[i],traffic->yaw[i],traffic->length_m[i],traffic->width_m[i]});
            if(!path_file.empty() && now>=next_lane) {
                next_lane=now+1s;
                try {lane=LanePath(path_file);} catch(const std::exception&) {lane.reset();}
            }
            std::vector<std::array<double,2>> path;
            if(lane) {
                const double s0=lane->project(p.x,p.y).s;
                for(double s=s0;s<=std::min(s0+120,lane->points.back().s);s+=2) {const auto q=lane->at(s);path.push_back({q.x,q.y});}
            } else for(double s=0;s<=120;s+=2) path.push_back({p.x+s*std::cos(yaw),p.y+s*std::sin(yaw)});
            const auto r=perception::analyze(points,p.x,p.y,yaw,objects,own,path);
            nav_msgs::msg::OccupancyGrid grid;
            grid.header.stamp=ego->header.stamp;grid.header.frame_id="world";
            grid.info.resolution=float(r.cell);grid.info.width=uint32_t(r.nx);grid.info.height=uint32_t(r.ny);
            grid.info.origin.position.x=r.origin_x;grid.info.origin.position.y=r.origin_y;grid.info.origin.position.z=p.z;
            grid.info.origin.orientation.z=std::sin(r.yaw/2);grid.info.origin.orientation.w=std::cos(r.yaw/2);
            grid.data=r.cells;
            grid_pub->publish(grid);
            visualization_msgs::msg::MarkerArray markers;
            {visualization_msgs::msg::Marker clear;clear.header.frame_id="world";clear.action=visualization_msgs::msg::Marker::DELETEALL;markers.markers.push_back(clear);}
            int id=0;
            const auto cloud_marker=[&](const std::vector<perception::Point>& pts,float r_,float g,float b,const char* ns) {
                if(pts.empty()) return;
                visualization_msgs::msg::Marker m;m.header=grid.header;m.ns=ns;m.id=id++;m.type=visualization_msgs::msg::Marker::POINTS;m.action=visualization_msgs::msg::Marker::ADD;
                m.pose.orientation.w=1;m.scale.x=m.scale.y=.25;m.color.r=r_;m.color.g=g;m.color.b=b;m.color.a=.9f;
                for(const auto& q:pts) {geometry_msgs::msg::Point x;x.x=q.x;x.y=q.y;x.z=q.z;m.points.push_back(x);}
                markers.markers.push_back(std::move(m));
            };
            cloud_marker(r.unexplained,1,.25f,.2f,"perception/unexplained");
            cloud_marker(r.explained,.6f,.62f,.66f,"perception/explained");
            if(std::isfinite(r.blocked_m) && path.size()>=2) {
                // A bar across the lane where the first blocked stretch begins.
                double s=0;size_t i=0;
                for(;i+1<path.size();++i) {const double l=std::hypot(path[i+1][0]-path[i][0],path[i+1][1]-path[i][1]);if(s+l>=r.blocked_m) break;s+=l;}
                const auto& a=path[std::min(i,path.size()-2)];const auto& b=path[std::min(i+1,path.size()-1)];
                const double h=std::atan2(b[1]-a[1],b[0]-a[0]),t=std::clamp(r.blocked_m-s,0.0,std::hypot(b[0]-a[0],b[1]-a[1]));
                const double cx=a[0]+t*std::cos(h),cy=a[1]+t*std::sin(h);
                visualization_msgs::msg::Marker m;m.header=grid.header;m.ns="perception/blocked";m.id=id++;m.type=visualization_msgs::msg::Marker::LINE_LIST;
                m.action=visualization_msgs::msg::Marker::ADD;m.pose.orientation.w=1;m.scale.x=.3;m.color.r=1;m.color.g=.25f;m.color.b=.2f;m.color.a=1;
                for(const double side:{-2.0,2.0}) {geometry_msgs::msg::Point x;x.x=cx-side*std::sin(h);x.y=cy+side*std::cos(h);x.z=p.z+.1;m.points.push_back(x);}
                markers.markers.push_back(std::move(m));
            }
            marker_pub->publish(markers);
            blocked.data=float(r.blocked_m);blocked_pub->publish(blocked);
            if(std::isfinite(r.blocked_m)!=std::isfinite(last_blocked)) std::cout<<(std::isfinite(r.blocked_m)?"Path blocked at "+std::to_string(int(r.blocked_m))+" m":"Path clear")<<std::endl;
            last_blocked=r.blocked_m;
        });
        while(rclcpp::ok() && !stop.requested()) rclcpp::spin_some(node),std::this_thread::sleep_for(5ms);
    } catch(const std::exception& e) {std::cerr<<e.what()<<std::endl;result=1;}
    rclcpp::shutdown();
    return result;
}
