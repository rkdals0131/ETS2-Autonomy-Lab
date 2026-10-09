#include <rclcpp/rclcpp.hpp>
#include <ets2_msgs/msg/drive_command.hpp>
#include <ets2_msgs/msg/drive_state.hpp>
#include <ets2_msgs/msg/vehicle_state.hpp>
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
    const auto arm=node->declare_parameter("arm",false);
    node->declare_parameter("target_speed_mps",0.0);
    node->declare_parameter("steering_target",0.0);
    node->declare_parameter("relative_steering",true);
    node->declare_parameter("steering_gain",2.0);
    node->declare_parameter("steering_integral_gain",1.0);
    node->declare_parameter("throttle_gain",0.2);
    node->declare_parameter("brake_gain",0.2);
    const auto owner="speed-"+std::to_string(getpid())+"-"+std::to_string(Clock::now().time_since_epoch().count());
    auto parameters=node->add_on_set_parameters_callback([](const auto& values) {
        rcl_interfaces::msg::SetParametersResult result;result.successful=true;
        for(const auto& p:values) {
            if(p.get_name()=="arm" || p.get_name()=="relative_steering") {result.successful=false;result.reason="Input mode and arm are startup options; restart explicitly";break;}
            if(p.get_name()=="target_speed_mps" || p.get_name()=="steering_target" || p.get_name()=="throttle_gain" || p.get_name()=="brake_gain" || p.get_name()=="steering_gain" || p.get_name()=="steering_integral_gain") {
                if(p.get_type()!=rclcpp::ParameterType::PARAMETER_DOUBLE || !std::isfinite(p.as_double()) ||
                   (p.get_name()=="steering_target"?std::abs(p.as_double())>1:p.as_double()<0)) {
                    result.successful=false;result.reason="Speed/gains must be finite and nonnegative; steering must be in [-1,1]";break;
                }
            }
        }
        return result;
    });
    const auto settings_valid=[&] {
        for(const auto* name:{"target_speed_mps","steering_target","throttle_gain","brake_gain","steering_gain","steering_integral_gain"}) {
            const auto value=node->get_parameter(name).as_double();
            if(!std::isfinite(value) || (std::string_view(name)=="steering_target"?std::abs(value)>1:value<0)) return false;
        }
        return true;
    };
    if(!arm || !settings_valid()) {
        std::cerr<<(arm?"Invalid driving parameters":"No driving enabled; start explicitly with -p arm:=true")<<std::endl;
        rclcpp::shutdown();return arm?1:0;
    }
    ets2_msgs::msg::DriveState::SharedPtr drive;
    ets2_msgs::msg::VehicleState::SharedPtr vehicle;
    Clock::time_point drive_time{},vehicle_time{};
    auto ds=node->create_subscription<ets2_msgs::msg::DriveState>("/ets2/drive/state",rclcpp::QoS(1),
        [&](ets2_msgs::msg::DriveState::SharedPtr value){drive=std::move(value);drive_time=Clock::now();});
    auto vs=node->create_subscription<ets2_msgs::msg::VehicleState>("/ets2/vehicle/state",rclcpp::QoS(1),
        [&](ets2_msgs::msg::VehicleState::SharedPtr value){vehicle=std::move(value);vehicle_time=Clock::now();});
    auto publisher=node->create_publisher<ets2_msgs::msg::DriveCommand>("/ets2/drive/command",rclcpp::QoS(1).best_effort());
    auto service=node->create_client<ets2_msgs::srv::DriveControl>("/ets2/drive/control");
    uint64_t epoch=0,sequence=0;int result=0;
    const auto control=[&](bool enabled) {
        auto request=std::make_shared<ets2_msgs::srv::DriveControl::Request>();
        request->owner=owner;request->arm=enabled;request->epoch=epoch;
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
        while(!stopped && Clock::now()<until && !(drive && vehicle && service->service_is_ready() && publisher->get_subscription_count())) {
            rclcpp::spin_some(node);std::this_thread::sleep_for(10ms);
        }
        if(stopped) throw std::runtime_error("Stopped before arm");
        if(!drive || !vehicle || !service->service_is_ready() || !publisher->get_subscription_count()) throw std::runtime_error("Bridge discovery timed out");
        if(!drive->available || !drive->permitted || !drive->profile_supported || vehicle->paused) throw std::runtime_error("Game input is unavailable or paused");
        control(true);
        const auto first_window_until=Clock::now()+150ms;
        while(!stopped && Clock::now()<first_window_until && !(drive->armed && drive->owner==owner && drive->epoch==epoch)) {
            rclcpp::spin_some(node);std::this_thread::sleep_for(2ms);
        }
        std::cout<<"Driving armed; owner="<<owner<<", epoch="<<epoch<<". Ctrl+C releases control."<<std::endl;
        auto report=Clock::now(),previous=report;double steering_integral=0;
        while(!stopped && rclcpp::ok()) {
            rclcpp::spin_some(node);const auto now=Clock::now();
            if(!drive->armed || drive->owner!=owner || drive->epoch!=epoch) throw std::runtime_error("Driving released: "+drive->reason);
            if(now-drive_time>200ms || now-vehicle_time>200ms || vehicle->paused || !std::isfinite(vehicle->speed_mps))
                throw std::runtime_error("Current game state is unavailable or paused");
            const auto error=node->get_parameter("target_speed_mps").as_double()-vehicle->speed_mps;
            ets2_msgs::msg::DriveCommand command;
            command.owner=owner;command.epoch=epoch;command.sequence=++sequence;command.command_window_ms=drive->command_window_ms;
            const auto steering_target=node->get_parameter("steering_target").as_double();
            command.steering=steering_target;
            if(node->get_parameter("relative_steering").as_bool()) {
                if(!std::isfinite(drive->steering_applied)) throw std::runtime_error("Applied steering feedback is unavailable");
                const auto steering_error=steering_target-drive->steering_applied;
                const auto proportional=steering_error*node->get_parameter("steering_gain").as_double();
                const auto candidate=steering_integral+steering_error*node->get_parameter("steering_integral_gain").as_double()*std::chrono::duration<double>(now-previous).count();
                // Do not accumulate error pushing further into a saturated input.
                if(std::abs(proportional+candidate)<=1 || steering_error*(proportional+candidate)<0) steering_integral=candidate;
                command.steering=std::clamp(proportional+steering_integral,-1.0,1.0);
            }
            previous=now;
            command.throttle=std::clamp(error*node->get_parameter("throttle_gain").as_double(),0.0,1.0);
            command.brake=std::clamp(-error*node->get_parameter("brake_gain").as_double(),0.0,1.0);
            publisher->publish(command);
            if(now>=report) {
                std::cout<<"speed="<<vehicle->speed_mps<<" target="<<node->get_parameter("target_speed_mps").as_double()
                    <<" steering_target="<<steering_target<<" steering_applied="<<drive->steering_applied<<" steering_input="<<command.steering<<" throttle="<<command.throttle<<" brake="<<command.brake
                    <<" accepted_sequence="<<drive->sequence<<std::endl;
                report=now+1s;
            }
            std::this_thread::sleep_for(20ms);
        }
    } catch(const std::exception& error) {std::cerr<<error.what()<<std::endl;result=1;}
    if(epoch) try {control(false);} catch(const std::exception& error) {std::cerr<<"Release: "<<error.what()<<std::endl;}
    rclcpp::shutdown();return result;
}
