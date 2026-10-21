// HUD (docs/24 §0): one MarkerArray, /ets2/hud/markers at 30 Hz, from drive_speed's
// assistance state (/ets2/drive/assist), LCC's path (/ets2/drive/plan), the lane in use
// (/ets2/drive/lane) and GT traffic. Namespaces:
// - road/<kind>: the marks painted on the game's road (hud_marks.hpp; the relay draws
//   LINE_STRIP markers of road/ only): lcc_path, acc, lane_change.
// - scene/<kind>: Foxglove only, one question each:
//   truck: the truck and its trailer (articulation), outlines from the axles and estimates.
//   lane: the lane in use, centre and edges; green while a lane change goes to it.
//   lanes: the provider's other lanes beside (path_file's .left/.right); amber when a turn
//     signal lane change waits for it.
//   traffic: vehicles within 150 m; the lead green, the one holding a lane change back amber.
//   acc: the gap ACC keeps behind the lead (a band from the lead back to where the front
//     bumper should be).
//   curve: lane stretches whose curve speed (lateral acceleration limit) is below the set
//     speed, amber, with the lowest limit written there.
//   status: text above the truck (the layout hides it: the indicators say the same).
// - debug/mpc: the lateral MPC's plan for the rear point over 1.5 s (/ets2/drive/mpc_plan),
//   to compare what it intends with what the truck then does (layout default: off).
//   markings / junctions / signals: the map around the truck from the provider's scene file
//     (path_file's .scene, 120 m, read when it changes; hud_scene.hpp), drawn within
//     scene_view_m (60 m) of the truck each frame, grey background: lane dividers
//     dashed, the divider between the directions a double line, road edges solid; prefab
//     navigation curves faint; traffic lights as a pole and a head with a tick towards the
//     traffic they face (positions only: the game reports no light state).
// Foxglove's 3D panel toggles each namespace. A separate process, so presentation never
// touches the 50 Hz control loop. Assistance state older than 0.3 s (standby, released,
// paused) clears the road marks; the scene stays while GT arrives.
#include "control/drive_session.hpp"
#include "control/hud_marks.hpp"
#include "control/hud_scene.hpp"
#include <rclcpp/rclcpp.hpp>
#include <ets2_msgs/msg/assist_state.hpp>
#include <ets2_msgs/msg/traffic_state.hpp>
#include <geometry_msgs/msg/pose_stamped.hpp>
#include <nav_msgs/msg/path.hpp>
#include <visualization_msgs/msg/marker_array.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
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
        // The provider's lanes beside (Foxglove only), read once a second.
        const auto path_file=node->declare_parameter<std::string>("path_file","");
        std::array<hud::Points,2> sides; // left, right
        auto next_sides=Clock::now();
        // The provider's map around the truck (Foxglove only): read when the file changes (checked
        // every 0.1 s), drawn within scene_view_m of the truck.
        const double scene_view=node->declare_parameter("scene_view_m",60.0);
        hud::Scene scene;
        std::filesystem::file_time_type scene_written{};
        auto next_scene=Clock::now();
        // Truck outline, from the last armed AssistState (the off messages carry none).
        double truck_front=5.3,truck_rear=-2.5;
        double trailer_yaw=0;Clock::time_point trailer_time{};
        geometry_msgs::msg::PoseStamped::SharedPtr ego;Clock::time_point ego_time{};
        auto as=node->create_subscription<ets2_msgs::msg::AssistState>("/ets2/drive/assist",rclcpp::QoS(10),
            [&](ets2_msgs::msg::AssistState::SharedPtr m){assist=std::move(m);assist_time=Clock::now();});
        const auto points=[](const nav_msgs::msg::Path& m) {hud::Points p;for(const auto& q:m.poses) p.push_back({q.pose.position.x,q.pose.position.y});return p;};
        auto ps=node->create_subscription<nav_msgs::msg::Path>("/ets2/drive/plan",rclcpp::QoS(1).best_effort(),[&](nav_msgs::msg::Path::SharedPtr m){plan=points(*m);});
        hud::Points mpc;
        auto ms=node->create_subscription<nav_msgs::msg::Path>("/ets2/drive/mpc_plan",rclcpp::QoS(1).best_effort(),[&](nav_msgs::msg::Path::SharedPtr m){mpc=points(*m);});
        auto ls=node->create_subscription<nav_msgs::msg::Path>("/ets2/drive/lane",rclcpp::QoS(1),[&](nav_msgs::msg::Path::SharedPtr m){lane=points(*m);});
        auto ts=node->create_subscription<ets2_msgs::msg::TrafficState>("/ets2/ground_truth/traffic",rclcpp::QoS(1),
            [&](ets2_msgs::msg::TrafficState::SharedPtr m){traffic=std::move(m);traffic_time=Clock::now();});
        auto trs=node->create_subscription<geometry_msgs::msg::PoseStamped>("/ets2/ground_truth/trailer/pose",rclcpp::QoS(1),[&](geometry_msgs::msg::PoseStamped::SharedPtr m) {
            const auto& q=m->pose.orientation;trailer_yaw=std::atan2(2*(q.w*q.z+q.x*q.y),1-2*(q.y*q.y+q.z*q.z));trailer_time=Clock::now();
        });
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
            const bool on=assisted && (assist->acc_active || assist->lcc_state);
            if(assisted && assist->truck_front_m>0) {truck_front=assist->truck_front_m;truck_rear=assist->truck_rear_m;}
            const auto box=[&](const std::string& ns,double cx,double cy,double h,double length,double width,double height,std::array<float,4> c) {
                auto m=marker(ns,Marker::CUBE);
                m.pose.position.x=cx;m.pose.position.y=cy;m.pose.position.z=ez+height/2;
                m.pose.orientation.z=std::sin(h/2);m.pose.orientation.w=std::cos(h/2);
                m.scale.x=length;m.scale.y=width;m.scale.z=height;colour(m,c[0],c[1],c[2],c[3]);
                array.markers.push_back(std::move(m));
            };
            const auto offset_line=[](const hud::Points& centre,double offset) {
                hud::Points out;
                for(size_t i=0;i<centre.size();++i) {
                    const auto& a=centre[i==0?0:i-1];const auto& b=centre[i==0?1:i];
                    const double l=std::hypot(b[0]-a[0],b[1]-a[1]);
                    if(l>1e-6) out.push_back({centre[i][0]-offset*(b[1]-a[1])/l,centre[i][1]+offset*(b[0]-a[0])/l});
                }
                return out;
            };
            constexpr std::array<float,4> grey{.6f,.65f,.7f,.5f},green{.35f,.95f,.45f,.8f},amber{1,.62f,.15f,.8f};
            // Truck and trailer. The trailer hangs from the fifth wheel (0.5 m ahead of the
            // drive axle, estimate) along its own yaw: 13.6 m, front 1.6 m ahead of the kingpin.
            box("scene/truck",ex+(truck_front+truck_rear)/2*std::cos(yaw),ey+(truck_front+truck_rear)/2*std::sin(yaw),yaw,truck_front-truck_rear,2.55,3.8,{.3f,.55f,.95f,.6f});
            if(now-trailer_time<=500ms) {
                const double kx=ex+.5*std::cos(yaw),ky=ey+.5*std::sin(yaw),centre=1.6-13.6/2;
                box("scene/truck",kx+centre*std::cos(trailer_yaw),ky+centre*std::sin(trailer_yaw),trailer_yaw,13.6,2.55,4,{.3f,.55f,.95f,.35f});
            }
            const double width=on && assist->lane_width_m>0?assist->lane_width_m:3.5;
            // Lanes: the one in use (green while changing to it), the provider's beside (amber
            // while a lane change waits for that side).
            if(on && lane.size()>=2) {
                const auto c=assist->lane_change_active?green:grey;
                line("scene/lane",lane,.08,0,c,ez);
                for(const int side:{-1,1}) line("scene/lane",offset_line(lane,side*width/2),.05,0,{c[0],c[1],c[2],.3f},ez);
            }
            if(!path_file.empty() && now>=next_sides) {
                next_sides=now+1s;
                const auto dot=path_file.rfind('.');
                for(int k=0;k<2;++k) {
                    sides[k].clear();
                    try {
                        const LanePath side(path_file.substr(0,dot)+(k?".right":".left")+(dot==std::string::npos?"":path_file.substr(dot)));
                        for(const auto& q:side.points) {
                            const double dx=q.x-ex,dy=q.y-ey,ahead=dx*std::cos(yaw)+dy*std::sin(yaw);
                            if(ahead>-50 && ahead<400 && std::abs(-dx*std::sin(yaw)+dy*std::cos(yaw))<30) sides[k].push_back({q.x,q.y});
                        }
                    } catch(const std::exception&) {} // no lane on that side, or a file being replaced
                }
            }
            if(on) for(int k=0;k<2;++k) if(sides[k].size()>=2) {
                const int side=k?1:-1;
                const bool waiting=!assist->lane_change_active && assist->lane_change_request==side && assist->lane_source==0;
                line("scene/lanes",sides[k],waiting?.12:.05,0,waiting?amber:std::array<float,4>{.6f,.65f,.7f,.25f},ez);
            }
            // The map around the truck: lane markings, junction curves, traffic lights (grey: background).
            if(!path_file.empty() && now>=next_scene) {
                next_scene=now+100ms;
                const auto dot=path_file.rfind('.');
                const auto file=path_file.substr(0,dot)+".scene"+(dot==std::string::npos?"":path_file.substr(dot));
                std::error_code error;
                const auto written=std::filesystem::last_write_time(file,error);
                if(error) scene=hud::Scene{};
                else if(written!=scene_written) {
                    try {std::ifstream input(file);scene=hud::read_scene(nlohmann::json::parse(input));scene_written=written;}
                    catch(const std::exception&) {} // a file being replaced: keep the last scene, read it next time
                }
            }
            // Only while the truck is inside what the file covers (not a stopped provider's stale file).
            if(scene.valid && std::hypot(scene.x-ex,scene.y-ey)<=scene.radius) {
                const auto lines=[&](const std::string& ns,const std::vector<hud::Point3>& pairs,double width,std::array<float,4> c) {
                    const auto pts=hud::within(pairs,ex,ey,scene_view);
                    if(pts.size()<2) return;
                    auto m=marker(ns,Marker::LINE_LIST);m.scale.x=width;colour(m,c[0],c[1],c[2],c[3]);
                    for(const auto& [x,y,z]:pts) {geometry_msgs::msg::Point p;p.x=x;p.y=y;p.z=z;m.points.push_back(p);}
                    array.markers.push_back(std::move(m));
                };
                lines("scene/markings",scene.lane,.12,{.75f,.78f,.8f,.45f});
                lines("scene/markings",scene.centre,.1,{.85f,.87f,.9f,.6f});
                lines("scene/markings",scene.edge,.15,{.85f,.87f,.9f,.7f});
                lines("scene/junctions",scene.junction,.06,{.6f,.65f,.7f,.2f});
                if(std::any_of(scene.lights.begin(),scene.lights.end(),[&](const auto& l) {return std::hypot(l.position[0]-ex,l.position[1]-ey)<=scene_view;})) {
                    // Pole (light on the ground or on a pole head) or an overhead head alone; the tick
                    // points from the head towards the traffic it faces.
                    auto poles=marker("scene/signals",Marker::LINE_LIST);poles.scale.x=.15;colour(poles,.7f,.72f,.75f,.8f);
                    auto heads=marker("scene/signals",Marker::CUBE_LIST);heads.scale.x=heads.scale.y=.45;heads.scale.z=1;colour(heads,.7f,.72f,.75f,.9f);
                    for(const auto& l:scene.lights) {
                        const auto [lx,ly,lz]=l.position;
                        if(std::hypot(lx-ex,ly-ey)>scene_view) continue;
                        const bool overhead=l.height>=4.5;
                        const double top=lz+(overhead?l.height:std::max(l.height,3.2));
                        geometry_msgs::msg::Point p;p.x=lx;p.y=ly;
                        if(!overhead) {p.z=lz;poles.points.push_back(p);p.z=top-.5;poles.points.push_back(p);}
                        p.z=top;heads.points.push_back(p);
                        if(l.heading) {
                            poles.points.push_back(p);
                            p.x-=std::cos(*l.heading);p.y-=std::sin(*l.heading);poles.points.push_back(p);
                        }
                    }
                    if(!poles.points.empty()) array.markers.push_back(std::move(poles));
                    array.markers.push_back(std::move(heads));
                }
            }
            // Traffic: the lead green, the vehicle holding a lane change back amber, others grey.
            if(traffic && now-traffic_time<=500ms) for(size_t i=0;i<traffic->id.size();++i) {
                const double x=traffic->x[i],y=traffic->y[i];
                if(std::hypot(x-ex,y-ey)>150) continue;
                const auto id=traffic->id[i];
                const auto c=on && id==assist->lead_id?green:on && id==assist->lane_change_blocker_id?amber:std::array<float,4>{.7f,.7f,.75f,.3f};
                box("scene/traffic",x,y,traffic->yaw[i],traffic->length_m[i],traffic->width_m[i],3,c);
            }
            // ACC's gap behind the lead: from its rear bumper back to where the front bumper should be.
            if(on && in.mark==2 && assist->target_gap_m>0) {
                const double g=assist->target_gap_m,h=in.mark_heading;
                box("scene/acc",in.mark_x-g/2*std::cos(h),in.mark_y-g/2*std::sin(h),h,g,width-.5,.05,{.35f,.95f,.45f,.15f});
                const double tx=in.mark_x-g*std::cos(h),ty=in.mark_y-g*std::sin(h);
                line("scene/acc",{{tx+std::sin(h)*(width/2-.25),ty-std::cos(h)*(width/2-.25)},{tx-std::sin(h)*(width/2-.25),ty+std::cos(h)*(width/2-.25)}},.25,0,green,ez+.05);
            }
            // Curve speed below the set speed on the lane ahead (the same limit as drive_speed:
            // sqrt(a / |curvature|) over a 10 m window).
            if(on && assist->acc_active && lane.size()>=3 && assist->curve_lateral_acceleration_mps2>0) try {
                const LanePath path(lane,width);
                const auto p=path.project(ex,ey);
                const double a=assist->curve_lateral_acceleration_mps2,set=assist->set_speed_mps;
                hud::Points stretch;double lowest=1e9;std::array<double,2> at{};
                const auto flush=[&] {if(stretch.size()>=2) line("scene/curve",stretch,.35,0,amber,ez+.03);stretch.clear();};
                for(double s=p.s;s<path.points.back().s-5;s+=5) {
                    const double k=std::abs(path.curvature(s,10)),limit=k>1e-6?std::sqrt(a/k):1e9;
                    if(limit<set) {const auto q=path.at(s);stretch.push_back({q.x,q.y});if(limit<lowest) {lowest=limit;at={q.x,q.y};}}
                    else flush();
                }
                flush();
                if(lowest<1e8) {
                    auto m=marker("scene/curve",Marker::TEXT_VIEW_FACING);
                    m.pose.position.x=at[0];m.pose.position.y=at[1];m.pose.position.z=ez+3;m.scale.z=1.5;colour(m,1,.62f,.15f,.95f);
                    m.text="커브 "+std::to_string(int(std::lround(lowest*3.6)))+" km/h";
                    array.markers.push_back(std::move(m));
                }
            } catch(const std::exception&) {}
            // The MPC's plan, with a dot every 0.1 s.
            if(assisted && mpc.size()>=2) {
                line("debug/mpc",mpc,.1,0,{.4f,.75f,1,.9f},ez+.1);
                auto dots=marker("debug/mpc",Marker::SPHERE_LIST);dots.scale.x=dots.scale.y=dots.scale.z=.3;colour(dots,.4f,.75f,1,.9f);
                for(const auto& [x,y]:mpc) {geometry_msgs::msg::Point q;q.x=x;q.y=y;q.z=ez+.1;dots.points.push_back(q);}
                array.markers.push_back(std::move(dots));
            }
            // Status above the truck.
            if(assisted && (assist->acc_active || assist->lcc_state)) {
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
