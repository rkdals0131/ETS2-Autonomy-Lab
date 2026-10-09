#include <rclcpp/rclcpp.hpp>
#include <ets2_msgs/msg/drive_command.hpp>
#include <ets2_msgs/msg/drive_state.hpp>
#include <ets2_msgs/msg/vehicle_state.hpp>
#include <ets2_msgs/msg/traffic_state.hpp>
#include <geometry_msgs/msg/pose_stamped.hpp>
#include <std_msgs/msg/string.hpp>
#include "control/path_following.hpp"
#include "control/parameters.hpp"
#include <ets2_msgs/srv/drive_control.hpp>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <csignal>
#include <iostream>
#include <thread>
#include <unistd.h>

using namespace std::chrono_literals;
using Clock=std::chrono::steady_clock;
static std::atomic<bool> stopped{false};
static void stop(int) {stopped=true;}

int main(int argc,char** argv) {
    rclcpp::init(argc,argv,rclcpp::InitOptions(),rclcpp::SignalHandlerOptions::None);
    std::signal(SIGINT,stop);std::signal(SIGTERM,stop);
    auto node=std::make_shared<rclcpp::Node>("ets2_drive_speed");
    const auto initial=declare_driving_parameters(*node);
    std::atomic<std::shared_ptr<const DrivingParameters>> configuration{std::make_shared<const DrivingParameters>(initial)};
    auto parameters=node->add_on_set_parameters_callback([](const auto& values) {
        return check_driving_parameter_changes(values);
    });
    auto parameter_updates=node->add_post_set_parameters_callback([&](const auto& values) {
        auto next=std::make_shared<DrivingParameters>(*configuration.load());
        apply_driving_parameter_changes(*next,values);
        configuration.store(std::move(next));
    });
    std::unique_ptr<LanePath> lane;
    if(!initial.path_file.empty()) try {lane=std::make_unique<LanePath>(initial.path_file);} catch(const std::exception& e) {
        std::cerr<<e.what()<<std::endl;rclcpp::shutdown();return 1;
    }
    if(!initial.arm || !valid_driving_parameters(initial)) {
        std::cerr<<(initial.arm?"Invalid driving parameters":"No driving enabled; start explicitly with -p arm:=true")<<std::endl;
        rclcpp::shutdown();return initial.arm?1:0;
    }
    const auto owner="speed-"+std::to_string(getpid())+"-"+std::to_string(Clock::now().time_since_epoch().count());
    ets2_msgs::msg::DriveState::SharedPtr drive;
    ets2_msgs::msg::VehicleState::SharedPtr vehicle;
    Clock::time_point drive_time{},vehicle_time{};
    auto ds=node->create_subscription<ets2_msgs::msg::DriveState>("/ets2/drive/state",rclcpp::QoS(1),
        [&](ets2_msgs::msg::DriveState::SharedPtr value){drive=std::move(value);drive_time=Clock::now();});
    auto vs=node->create_subscription<ets2_msgs::msg::VehicleState>("/ets2/vehicle/state",rclcpp::QoS(1),
        [&](ets2_msgs::msg::VehicleState::SharedPtr value){vehicle=std::move(value);vehicle_time=Clock::now();});
    double ego_x=0,ego_y=0,ego_yaw=0,wheelbase=0;bool pose_received=false,traffic_received=false;
    Clock::time_point pose_time{},traffic_time{};
    struct Actor {double x,y,yaw,speed,length,width;};std::vector<Actor> actors;
    rclcpp::Subscription<geometry_msgs::msg::PoseStamped>::SharedPtr ps;
    rclcpp::Subscription<std_msgs::msg::String>::SharedPtr config;
    rclcpp::Subscription<ets2_msgs::msg::TrafficState>::SharedPtr ts;
    if(lane) {
        ps=node->create_subscription<geometry_msgs::msg::PoseStamped>("/ets2/ground_truth/ego/pose",rclcpp::QoS(1),[&](geometry_msgs::msg::PoseStamped::SharedPtr msg) {
            const auto& p=msg->pose.position;const auto& q=msg->pose.orientation;
            if(msg->header.frame_id!="world" || !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z) || !std::isfinite(q.w))
                throw std::runtime_error("Invalid GT ego pose");
            ego_x=p.x;ego_y=p.y;ego_yaw=std::atan2(2*(q.w*q.z+q.x*q.y),1-2*(q.y*q.y+q.z*q.z));pose_received=true;pose_time=Clock::now();
        });
        config=node->create_subscription<std_msgs::msg::String>("/ets2/sensors/config",rclcpp::QoS(1).transient_local(),[&](std_msgs::msg::String::SharedPtr msg) {
            const auto data=nlohmann::json::parse(msg->data);double front=0;
            for(const auto& w:data.at("vehicle").at("wheels")) if(w.value("steerable",false)) front=std::max(front,w.at("position_base_m").at(0).template get<double>());
            if(!std::isfinite(front) || front<=0) throw std::runtime_error("Current truck steering axle geometry is unavailable");
            wheelbase=front;
        });
        ts=node->create_subscription<ets2_msgs::msg::TrafficState>("/ets2/ground_truth/traffic",rclcpp::QoS(1),[&](ets2_msgs::msg::TrafficState::SharedPtr msg) {
            if(!msg->available || msg->header.frame_id!="world") throw std::runtime_error("GT traffic unavailable: "+msg->error);
            const auto n=msg->id.size();
            if(msg->x.size()!=n || msg->y.size()!=n || msg->yaw.size()!=n || msg->speed_mps.size()!=n || msg->length_m.size()!=n || msg->width_m.size()!=n) throw std::runtime_error("Misaligned GT traffic arrays");
            actors.clear();
            for(size_t i=0;i<n;++i) {
                Actor a{msg->x[i],msg->y[i],msg->yaw[i],msg->speed_mps[i],msg->length_m[i],msg->width_m[i]};
                if(!std::isfinite(a.x)||!std::isfinite(a.y)||!std::isfinite(a.yaw)||!std::isfinite(a.speed)||!std::isfinite(a.length)||!std::isfinite(a.width)||a.length<=0||a.width<=0) throw std::runtime_error("Invalid GT traffic actor");
                actors.push_back(a);
            }
            traffic_received=true;traffic_time=Clock::now();
        });
    }
    auto publisher=node->create_publisher<ets2_msgs::msg::DriveCommand>("/ets2/drive/command",rclcpp::QoS(1).best_effort());
    auto service=node->create_client<ets2_msgs::srv::DriveControl>("/ets2/drive/control");
    uint64_t epoch=0,sequence=0;int result=0;
    const auto axes=[&]() {return configuration.load()->axes();};
    const auto control=[&](bool enabled) {
        auto request=std::make_shared<ets2_msgs::srv::DriveControl::Request>();
        request->owner=owner;request->arm=enabled;request->epoch=epoch;
        request->axes=enabled?axes():0;
        auto future=service->async_send_request(request);
        const auto until=Clock::now()+1s;
        while(Clock::now()<until && future.wait_for(0s)!=std::future_status::ready) {
            rclcpp::spin_some(node);std::this_thread::sleep_for(2ms);
        }
        if(future.wait_for(0s)!=std::future_status::ready) throw std::runtime_error("DriveControl timed out");
        const auto response=future.get();
        if(!response->success) throw std::runtime_error(response->message);
        if(enabled) epoch=response->epoch;
    };
    try {
        const auto until=Clock::now()+10s;
        while(!stopped && Clock::now()<until && !(drive && vehicle && (!lane || (pose_received && traffic_received && wheelbase>0)) && service->service_is_ready() && publisher->get_subscription_count())) {
            rclcpp::spin_some(node);std::this_thread::sleep_for(10ms);
        }
        if(stopped) throw std::runtime_error("Stopped before arm");
        if(!drive || !vehicle || (lane && (!pose_received || !traffic_received || wheelbase<=0)) || !service->service_is_ready() || !publisher->get_subscription_count()) throw std::runtime_error("Bridge discovery timed out");
        if(!drive->available || !drive->permitted || !drive->profile_supported || vehicle->paused) throw std::runtime_error("Game input is unavailable or paused");
        if(!axes()) throw std::runtime_error("Enable ACC or LCC explicitly before starting");
        if(axes()!=3 && !drive->independent_axes) throw std::runtime_error("Independent ACC/LCC requires the updated loader; restart the game");
        control(true);
        const auto first_window_until=Clock::now()+150ms;
        while(!stopped && Clock::now()<first_window_until && !(drive->armed && drive->owner==owner && drive->epoch==epoch)) {
            rclcpp::spin_some(node);std::this_thread::sleep_for(2ms);
        }
        std::cout<<"Driving armed; owner="<<owner<<", epoch="<<epoch<<". Ctrl+C releases control."<<std::endl;
        auto report=Clock::now(),previous=report;double steering_integral=0,speed_integral=0,previous_speed=vehicle->speed_mps;
        while(!stopped && rclcpp::ok()) {
            rclcpp::spin_some(node);const auto now=Clock::now();
            if(!drive->armed || drive->owner!=owner || drive->epoch!=epoch) throw std::runtime_error("Driving released: "+drive->reason);
            const auto settings=configuration.load();
            const auto enabled_axes=settings->axes();
            if(!enabled_axes) {std::cout<<"ACC and LCC disabled; releasing control"<<std::endl;break;}
            const bool acc=enabled_axes&2,lcc=enabled_axes&1;
            if(!acc) speed_integral=0;
            if(!lcc) steering_integral=0;
            if(now-drive_time>200ms || now-vehicle_time>200ms || vehicle->paused || !std::isfinite(vehicle->speed_mps))
                throw std::runtime_error("Current game state is unavailable or paused");
            const auto dt=std::chrono::duration<double>(now-previous).count();
            auto speed_target=settings->target_speed_mps;
            auto steering_target=settings->steering_target;
            double path_error=0,remaining=0,lead_gap=std::numeric_limits<double>::infinity();
            if(lane) {
                if(now-pose_time>200ms || now-traffic_time>200ms) throw std::runtime_error("Current GT path/traffic observation is unavailable");
                const auto goal=lane->follow(ego_x,ego_y,ego_yaw,settings->lookahead_m+std::max(0.0,vehicle->speed_mps)*settings->lookahead_time_s);
                path_error=goal.error;remaining=goal.remaining;
                if(lcc) {
                    steering_target=std::clamp(std::atan(wheelbase*goal.curvature)/settings->steering_angle_per_unit_rad,-1.0,1.0);
                    if(path_error>lane->width/2) throw std::runtime_error("LCC lane reference lost; releasing assistance");
                    if(acc) {
                        speed_target=std::min(speed_target,std::sqrt(2*settings->stopping_deceleration_mps2*std::max(0.0,remaining-1)));
                        if(std::abs(goal.curvature)>0) speed_target=std::min(speed_target,std::sqrt(settings->lateral_acceleration_mps2/std::abs(goal.curvature)));
                    }
                }
                for(const auto& actor:actors) {
                    double ahead,lateral,heading;
                    if(lcc) {
                        const auto p=lane->project(actor.x,actor.y,lane->progress);
                        const auto a=lane->points[p.segment],b=lane->points[p.segment+1];
                        ahead=p.s-lane->progress;lateral=p.d;heading=std::atan2(b.y-a.y,b.x-a.x);
                    } else {
                        // Human steering owns the current direction. Do not brake
                        // because the original LCC path ends or the driver changes lane.
                        const auto dx=actor.x-ego_x,dy=actor.y-ego_y;
                        ahead=dx*std::cos(ego_yaw)+dy*std::sin(ego_yaw);
                        lateral=std::abs(-dx*std::sin(ego_yaw)+dy*std::cos(ego_yaw));heading=ego_yaw;
                    }
                    if(!acc || ahead<=0 || lateral>(lane->width+actor.width)/2 || std::cos(actor.yaw-heading)<=0) continue;
                    const auto gap=ahead-actor.length/2;
                    const auto desired=wheelbase+settings->standstill_gap_m+std::max(0.0,vehicle->speed_mps)*settings->following_time_s;
                    speed_target=std::min(speed_target,std::max(0.0,actor.speed+settings->following_gain*(gap-desired)));
                    lead_gap=std::min(lead_gap,gap);
                }
                if(lcc && remaining<=1 && (!acc || std::abs(vehicle->speed_mps)<0.1)) {std::cout<<"Lane reference ended; releasing control"<<std::endl;break;}
            }
            const auto error=speed_target-vehicle->speed_mps;
            ets2_msgs::msg::DriveCommand command;
            command.owner=owner;command.epoch=epoch;command.sequence=++sequence;command.command_window_ms=drive->command_window_ms;
            command.axes=enabled_axes;
            command.steering=lcc?steering_target:0;
            if(lcc && settings->relative_steering) {
                if(!std::isfinite(drive->steering_applied)) throw std::runtime_error("Applied steering feedback is unavailable");
                const auto steering_error=steering_target-drive->steering_applied;
                const auto proportional=steering_error*settings->steering_gain;
                const auto candidate=steering_integral+steering_error*settings->steering_integral_gain*dt;
                // Do not accumulate error pushing further into a saturated input.
                if(std::abs(proportional+candidate)<=1 || steering_error*(proportional+candidate)<0) steering_integral=candidate;
                command.steering=std::clamp(proportional+steering_integral,-1.0,1.0);
            }
            const auto speed_kp=(error>=0?settings->throttle_gain:settings->brake_gain);
            if(speed_target==0) speed_integral=0;
            const auto candidate=speed_integral+error*settings->speed_integral_gain*dt;
            const auto derivative=dt>0?-(vehicle->speed_mps-previous_speed)/dt*settings->speed_derivative_gain:0;
            if(speed_target>0 && (std::abs(speed_kp*error+candidate+derivative)<=1 || error*(speed_kp*error+candidate+derivative)<0)) speed_integral=candidate;
            const auto effort=std::clamp(speed_kp*error+speed_integral+derivative,-1.0,1.0);
            previous=now;previous_speed=vehicle->speed_mps;
            command.throttle=acc?std::max(0.0,effort):0;command.brake=acc?std::max(0.0,-effort):0;
            publisher->publish(command);
            if(now>=report) {
                std::cout<<"speed="<<vehicle->speed_mps<<" target="<<speed_target<<" path_error="<<path_error<<" remaining="<<remaining<<" lead_gap="<<lead_gap
                    <<" steering_target="<<steering_target<<" steering_applied="<<drive->steering_applied<<" steering_input="<<command.steering<<" throttle="<<command.throttle<<" brake="<<command.brake
                    <<" axes="<<unsigned(drive->axes)<<" accepted_sequence="<<drive->sequence<<std::endl;
                report=now+1s;
            }
            std::this_thread::sleep_for(20ms);
        }
    } catch(const std::exception& error) {std::cerr<<error.what()<<std::endl;result=1;}
    if(epoch) try {control(false);} catch(const std::exception& error) {std::cerr<<"Release: "<<error.what()<<std::endl;}
    rclcpp::shutdown();return result;
}
