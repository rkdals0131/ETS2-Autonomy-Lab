// W02 command source (docs/21): arm the requested axes, then hold fixed
// normalized commands at 50 Hz with a fresh sender stamp until Ctrl+C or the
// launcher's stop event. Every DriveState reason change is printed for the
// checklist record. Exit 0: disarmed here; 2: the game released ownership.
#include "control/drive_session.hpp"
#include <rclcpp/rclcpp.hpp>
#include <ets2_msgs/msg/drive_command.hpp>
#include <ets2_msgs/msg/drive_state.hpp>
#include <cmath>
#include <iostream>
#include <optional>

using namespace std::chrono_literals;
using Clock=std::chrono::steady_clock;

int main(int argc,char** argv) {
    rclcpp::init(argc,argv,rclcpp::InitOptions(),rclcpp::SignalHandlerOptions::None);
    int result=0;uint64_t epoch=0;
    std::shared_ptr<rclcpp::Node> node;
    rclcpp::Client<ets2_msgs::srv::DriveControl>::SharedPtr service;
    std::string owner;
    try {
        StopSignal stop(argc,argv);
        node=std::make_shared<rclcpp::Node>("ets2_drive_hold");
        const auto axes=node->declare_parameter<int>("axes",3);
        const auto steering=node->declare_parameter<double>("steering",0.0);
        const auto throttle=node->declare_parameter<double>("throttle",0.0);
        const auto brake=node->declare_parameter<double>("brake",0.0);
        if(axes<1 || axes>3 || !std::isfinite(steering) || std::abs(steering)>1 ||
           !std::isfinite(throttle) || throttle<0 || throttle>1 || !std::isfinite(brake) || brake<0 || brake>1)
            throw std::runtime_error("Use axes 1 (LCC), 2 (ACC) or 3, steering [-1,1] and pedals [0,1]");
        owner=controller_owner("hold");
        ets2_msgs::msg::DriveState::SharedPtr drive;
        auto state=node->create_subscription<ets2_msgs::msg::DriveState>("/ets2/drive/state",rclcpp::QoS(1),
            [&](ets2_msgs::msg::DriveState::SharedPtr value) {
                if(!drive || drive->reason!=value->reason || drive->axes!=value->axes || drive->armed!=value->armed)
                    std::cout<<"sim "<<value->header.stamp.sec<<"."<<value->header.stamp.nanosec/1000000<<" reason="<<value->reason
                             <<" armed="<<value->armed<<" axes="<<unsigned(value->axes)<<" owner="<<value->owner<<std::endl;
                drive=std::move(value);
            });
        auto publisher=node->create_publisher<ets2_msgs::msg::DriveCommand>("/ets2/drive/command",rclcpp::QoS(1).best_effort());
        service=node->create_client<ets2_msgs::srv::DriveControl>("/ets2/drive/control");
        const auto discovery=Clock::now()+10s;
        while(!stop.requested() && Clock::now()<discovery && !(drive && service->service_is_ready() && publisher->get_subscription_count())) {
            rclcpp::spin_some(node);std::this_thread::sleep_for(10ms);
        }
        if(stop.requested()) throw std::runtime_error("Stopped before arm");
        if(!drive || !service->service_is_ready() || !publisher->get_subscription_count()) throw std::runtime_error("Bridge discovery timed out");
        if(!drive->available || !drive->permitted || !drive->profile_supported)
            throw std::runtime_error("Game input is unavailable: "+(drive->error.empty()?drive->reason:drive->error));
        if(axes!=3 && !drive->independent_axes) throw std::runtime_error("Independent ACC/LCC requires the updated loader; restart the game");
        epoch=request_drive_control(node,service,owner,true,0,static_cast<uint8_t>(axes));
        std::cout<<"armed owner="<<owner<<" epoch="<<epoch<<" axes="<<axes<<"; Ctrl+C disarms"<<std::endl;
        uint64_t sequence=0;bool confirmed=false;
        const auto confirm_until=Clock::now()+150ms;
        for(auto next=Clock::now();!stop.requested() && rclcpp::ok();next+=20ms) {
            rclcpp::spin_some(node);
            const bool owned=drive->armed && drive->owner==owner && drive->epoch==epoch;
            confirmed=confirmed || owned;
            if(confirmed && !owned) {std::cout<<"released by game: "<<drive->reason<<std::endl;epoch=0;result=2;break;}
            if(!confirmed && Clock::now()>confirm_until) throw std::runtime_error("Arm was not observed in DriveState");
            ets2_msgs::msg::DriveCommand command;
            command.owner=owner;command.epoch=epoch;command.sequence=++sequence;command.axes=static_cast<uint8_t>(axes);
            command.steering=static_cast<float>(steering);command.throttle=static_cast<float>(throttle);command.brake=static_cast<float>(brake);
            command.sent_steady_ns=steady_ns();
            publisher->publish(command);
            std::this_thread::sleep_until(next+20ms);
        }
    } catch(const std::exception& error) {std::cerr<<error.what()<<std::endl;result=1;}
    if(epoch) try {request_drive_control(node,service,owner,false,epoch,0);std::cout<<"disarmed"<<std::endl;}
        catch(const std::exception& error) {std::cerr<<"Release: "<<error.what()<<std::endl;result=1;}
    rclcpp::shutdown();return result;
}
