#include "control/drive_session.hpp"
#include <rclcpp/rclcpp.hpp>
#include <ets2_msgs/msg/drive_command.hpp>
#include <ets2_msgs/msg/drive_state.hpp>
#include <ets2_msgs/msg/vehicle_state.hpp>
#include <ets2_msgs/msg/traffic_state.hpp>
#include <geometry_msgs/msg/pose_stamped.hpp>
#include <std_msgs/msg/string.hpp>
#include "control/parameters.hpp"
#include "control/kinematic_axles.hpp"
#include "control/lead_tracker.hpp"
#include "control/acc_mpc.hpp"
#include "control/lcc_mpc.hpp"
#include <ets2_msgs/msg/actuation_state.hpp>
#include <ets2_msgs/msg/wheel_state.hpp>
#include <ets2_msgs/srv/drive_control.hpp>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <iostream>
#include <numbers>
#include <optional>
#include <thread>

using namespace std::chrono_literals;
using Clock=std::chrono::steady_clock;

int main(int argc,char** argv) {
    rclcpp::init(argc,argv,rclcpp::InitOptions(),rclcpp::SignalHandlerOptions::None);
    std::optional<StopSignal> stop;
    try {stop.emplace(argc,argv);} catch(const std::exception& e) {std::cerr<<e.what()<<std::endl;rclcpp::shutdown();return 1;}
    const auto stopped=[&]{return stop->requested();};
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
    const auto owner=controller_owner("speed");
    ets2_msgs::msg::DriveState::SharedPtr drive;
    ets2_msgs::msg::VehicleState::SharedPtr vehicle;
    ets2_msgs::msg::ActuationState::SharedPtr actuation;
    Clock::time_point drive_time{},vehicle_time{},actuation_time{};
    auto ds=node->create_subscription<ets2_msgs::msg::DriveState>("/ets2/drive/state",rclcpp::QoS(1),
        [&](ets2_msgs::msg::DriveState::SharedPtr value){drive=std::move(value);drive_time=Clock::now();});
    auto vs=node->create_subscription<ets2_msgs::msg::VehicleState>("/ets2/vehicle/state",rclcpp::QoS(1),
        [&](ets2_msgs::msg::VehicleState::SharedPtr value){vehicle=std::move(value);vehicle_time=Clock::now();});
    // The MPC's allocator uses the engine gear (throttle map, shift freeze).
    auto as=node->create_subscription<ets2_msgs::msg::ActuationState>("/ets2/vehicle/actuation",rclcpp::QoS(1),
        [&](ets2_msgs::msg::ActuationState::SharedPtr value){actuation=std::move(value);actuation_time=Clock::now();});
    const bool mpc=initial.longitudinal=="mpc",lateral_mpc=initial.lateral=="mpc";
    // ego pose is base_link (drive axle); the controller's rear point and wheelbase follow
    // the grounded axles (kinematic_axles.hpp), so a lowered lift axle moves them.
    double ego_x=0,ego_y=0,ego_yaw=0,front_axle=0;bool pose_received=false,traffic_received=false,wheels_received=false,liftable=false;
    std::map<uint32_t,AxleWheel> axle_wheels;std::map<uint32_t,bool> grounded;
    Clock::time_point pose_time{},traffic_time{};
    struct Actor {uint64_t id;double x,y,yaw,speed,length,width;};std::vector<Actor> actors;
    rclcpp::Subscription<geometry_msgs::msg::PoseStamped>::SharedPtr ps;
    rclcpp::Subscription<std_msgs::msg::String>::SharedPtr config;
    rclcpp::Subscription<ets2_msgs::msg::WheelState>::SharedPtr ws;
    rclcpp::Subscription<ets2_msgs::msg::TrafficState>::SharedPtr ts;
    // Connected trailer (relay: SDK placement, only while connected) for the MPC's hitch angle.
    rclcpp::Subscription<geometry_msgs::msg::PoseStamped>::SharedPtr trs;
    double trailer_yaw=0;Clock::time_point trailer_time{};
    if(lane) {
        ps=node->create_subscription<geometry_msgs::msg::PoseStamped>("/ets2/ground_truth/ego/pose",rclcpp::QoS(1),[&](geometry_msgs::msg::PoseStamped::SharedPtr msg) {
            const auto& p=msg->pose.position;const auto& q=msg->pose.orientation;
            if(msg->header.frame_id!="world" || !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z) || !std::isfinite(q.w))
                throw std::runtime_error("Invalid GT ego pose");
            ego_x=p.x;ego_y=p.y;ego_yaw=std::atan2(2*(q.w*q.z+q.x*q.y),1-2*(q.y*q.y+q.z*q.z));pose_received=true;pose_time=Clock::now();
        });
        config=node->create_subscription<std_msgs::msg::String>("/ets2/sensors/config",rclcpp::QoS(1).transient_local(),[&](std_msgs::msg::String::SharedPtr msg) {
            const auto data=nlohmann::json::parse(msg->data);double front=0;
            std::map<uint32_t,AxleWheel> wheels;
            for(const auto& w:data.at("vehicle").at("wheels")) {
                const AxleWheel wheel{w.at("position_base_m").at(0).template get<double>(),w.value("steerable",false),w.value("liftable",false)};
                if(!std::isfinite(wheel.x)) throw std::runtime_error("Current truck wheel geometry is unavailable");
                wheels[w.at("index").template get<uint32_t>()]=wheel;
                if(wheel.steerable) front=std::max(front,wheel.x);
            }
            if(!std::isfinite(front) || front<=0) throw std::runtime_error("Current truck steering axle geometry is unavailable");
            front_axle=front;axle_wheels=std::move(wheels);
            liftable=std::any_of(axle_wheels.begin(),axle_wheels.end(),[](const auto& w){return w.second.liftable;});
        });
        trs=node->create_subscription<geometry_msgs::msg::PoseStamped>("/ets2/ground_truth/trailer/pose",rclcpp::QoS(1),[&](geometry_msgs::msg::PoseStamped::SharedPtr msg) {
            const auto& q=msg->pose.orientation;
            if(msg->header.frame_id!="world" || !std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z) || !std::isfinite(q.w)) throw std::runtime_error("Invalid GT trailer pose");
            trailer_yaw=std::atan2(2*(q.w*q.z+q.x*q.y),1-2*(q.y*q.y+q.z*q.z));trailer_time=Clock::now();
        });
        ws=node->create_subscription<ets2_msgs::msg::WheelState>("/ets2/wheels/state",rclcpp::QoS(1),[&](ets2_msgs::msg::WheelState::SharedPtr msg) {
            if(msg->index.size()!=msg->on_ground.size()) throw std::runtime_error("Misaligned wheel state arrays");
            for(size_t i=0;i<msg->index.size();++i) grounded[msg->index[i]]=msg->on_ground[i];
            wheels_received=true;
        });
        ts=node->create_subscription<ets2_msgs::msg::TrafficState>("/ets2/ground_truth/traffic",rclcpp::QoS(1),[&](ets2_msgs::msg::TrafficState::SharedPtr msg) {
            if(!msg->available || msg->header.frame_id!="world") throw std::runtime_error("GT traffic unavailable: "+msg->error);
            const auto n=msg->id.size();
            if(msg->x.size()!=n || msg->y.size()!=n || msg->yaw.size()!=n || msg->speed_mps.size()!=n || msg->length_m.size()!=n || msg->width_m.size()!=n) throw std::runtime_error("Misaligned GT traffic arrays");
            actors.clear();
            for(size_t i=0;i<n;++i) {
                Actor a{msg->id[i],msg->x[i],msg->y[i],msg->yaw[i],msg->speed_mps[i],msg->length_m[i],msg->width_m[i]};
                if(!std::isfinite(a.x)||!std::isfinite(a.y)||!std::isfinite(a.yaw)||!std::isfinite(a.speed)||!std::isfinite(a.length)||!std::isfinite(a.width)||a.length<=0||a.width<=0) throw std::runtime_error("Invalid GT traffic actor");
                actors.push_back(a);
            }
            traffic_received=true;traffic_time=Clock::now();
        });
    }
    auto publisher=node->create_publisher<ets2_msgs::msg::DriveCommand>("/ets2/drive/command",rclcpp::QoS(1).best_effort());
    auto service=node->create_client<ets2_msgs::srv::DriveControl>("/ets2/drive/control");
    uint64_t epoch=0,sequence=0;int result=0;
    // With loader 0.42+ the MPC also owns the retarder and engine brake (axes bit 4, only
    // together with the pedals); otherwise its retarder steps are folded into the brake.
    bool auxiliary=false;
    // Axes the driver took back in this session: the brake ends ACC (loader 0.44+ keeps
    // LCC), and a lane lost under ACC hands steering back. Neither re-engages by itself.
    uint8_t taken=0;
    const auto axes=[&]() {const auto a=configuration.load()->axes()&~taken;return uint8_t(a|(auxiliary && (a&2)?4:0));};
    const auto control=[&](bool enabled) {epoch=request_drive_control(node,service,owner,enabled,epoch,enabled?axes():0);};
    try {
        const auto until=Clock::now()+10s;
        while(!stopped() && Clock::now()<until && !(drive && vehicle && (!lane || (pose_received && traffic_received && front_axle>0 && (wheels_received || !liftable))) && service->service_is_ready() && publisher->get_subscription_count())) {
            rclcpp::spin_some(node);std::this_thread::sleep_for(10ms);
        }
        if(stopped()) throw std::runtime_error("Stopped before arm");
        if(!drive || !vehicle || (lane && (!pose_received || !traffic_received || front_axle<=0 || (liftable && !wheels_received))) || !service->service_is_ready() || !publisher->get_subscription_count()) throw std::runtime_error("Bridge discovery timed out");
        if(!drive->available || !drive->permitted || !drive->profile_supported || vehicle->paused) throw std::runtime_error("Game input is unavailable or paused");
        if(!axes()) throw std::runtime_error("Enable ACC or LCC explicitly before starting");
        if(axes()!=3 && !drive->independent_axes) throw std::runtime_error("Independent ACC/LCC requires the updated loader; restart the game");
        auxiliary=mpc && drive->auxiliary_brakes;
        if(mpc) std::cout<<"Longitudinal: MPC, retarder "<<(auxiliary?"owned":"folded into the brake (loader < 0.42)")<<std::endl;
        control(true);
        const auto first_window_until=Clock::now()+150ms;
        while(!stopped() && Clock::now()<first_window_until && !(drive->armed && drive->owner==owner && drive->epoch==epoch)) {
            rclcpp::spin_some(node);std::this_thread::sleep_for(2ms);
        }
        std::cout<<"Driving armed; owner="<<owner<<", epoch="<<epoch<<". Ctrl+C releases control."<<std::endl;
        auto report=Clock::now(),previous=report;double speed_integral=0,previous_speed=vehicle->speed_mps;
        VirtualEps eps;
        LeadTracker leads;AccMpc acc_mpc;LccMpc lcc_mpc;const auto started=Clock::now();
        if(lane) std::cout<<"Lateral: "<<(lateral_mpc?"MPC":"pure pursuit")<<std::endl;
        uint64_t pedal_sequence=0;Clock::time_point driver_steering_until{};
        constexpr uint64_t lane_end_id=std::numeric_limits<uint64_t>::max();
        // Fixed 50 Hz schedule: sleep_until the next 20 ms deadline keeps the mean
        // period at 20 ms even with Windows' coarse sleep granularity; after an
        // overrun the schedule restarts from now instead of bursting to catch up.
        for(auto next=Clock::now();!stopped() && rclcpp::ok();) {
            rclcpp::spin_some(node);const auto now=Clock::now();
            if(!drive->armed || drive->owner!=owner || drive->epoch!=epoch) {
                if(drive->reason!="manual") throw std::runtime_error("Driving released: "+drive->reason);
                std::cout<<"The driver took over; control released"<<std::endl;epoch=0;break;
            }
            // Loader 0.44+: the driver's brake revokes the pedals after a command that held them.
            if(pedal_sequence && drive->sequence>=pedal_sequence && !(drive->axes&2) && !(taken&2)) {
                taken|=2;std::cout<<"ACC cancelled by the driver's brake"<<(configuration.load()->lcc_enabled && !(taken&1)?"; LCC continues":"")<<std::endl;
            }
            const auto settings=configuration.load();
            auto enabled_axes=axes();
            if(!enabled_axes) {std::cout<<"ACC and LCC disabled; releasing control"<<std::endl;break;}
            const bool acc=enabled_axes&2;bool lcc=enabled_axes&1;
            // The driver's steering overrides LCC (the loader passes it alone); LCC resumes 1 s
            // after the stick is let go. ACC keeps running throughout.
            if(lcc && std::isfinite(drive->steering_manual) && drive->steering_manual!=0) driver_steering_until=now+1s;
            const bool driver_steering=lcc && now<driver_steering_until;
            if(!acc) speed_integral=0;
            if(!lcc || driver_steering) eps.reset();
            if(now-drive_time>200ms || now-vehicle_time>200ms || vehicle->paused || !std::isfinite(vehicle->speed_mps))
                throw std::runtime_error("Current game state is unavailable or paused");
            const auto dt=std::chrono::duration<double>(now-previous).count();
            auto speed_target=settings->target_speed_mps;
            auto steering_target=settings->steering_target;double steering_rate=0;
            double path_error=0,path_offset=0,remaining=0,lead_gap=std::numeric_limits<double>::infinity();
            std::optional<uint64_t> lead_id;double lead_speed=0;
            // MPC lead: bumper-to-bumper gap; front_offset is the gap reference point to the front bumper.
            double front_offset=front_axle+settings->front_overhang_m,cruise_target=speed_target;
            if(lane) {
                if(now-pose_time>200ms || now-traffic_time>200ms) throw std::runtime_error("Current GT path/traffic observation is unavailable");
                const auto axles=kinematic_axles(axle_wheels,grounded);
                const auto rear_x=ego_x+axles.rear_x*std::cos(ego_yaw),rear_y=ego_y+axles.rear_x*std::sin(ego_yaw);
                const auto goal=LateralController::update(*lane,rear_x,rear_y,ego_yaw,vehicle->speed_mps,axles.wheelbase(),lateral_settings(*settings));
                path_error=goal.distance;path_offset=goal.offset;remaining=goal.remaining;
                if(lcc && path_error>lane->width/2) {
                    if(!acc) throw std::runtime_error("LCC lane reference lost; releasing assistance");
                    // E.g. the driver steered into another lane: steering goes back, ACC stays.
                    taken|=1;lcc=false;enabled_axes&=~1;eps.reset();
                    std::cout<<"LCC lane reference lost; steering back to the driver, ACC continues"<<std::endl;
                }
                if(lcc) front_offset-=axles.rear_x; // lane distances are from the rear point
                double wheel_angle=goal.wheel_angle,wheel_rate=goal.wheel_rate;
                if(lateral_mpc) {
                    LccMpc::Input in;
                    in.enable=lcc && !driver_steering;in.speed_mps=vehicle->speed_mps;
                    in.lateral_error_m=goal.offset;in.heading_error_rad=goal.heading_error;
                    for(int i=0;i<LccMpc::preview_points;++i) in.curvature_preview[i]=lane->curvature(goal.s+i*LccMpc::preview_step_m,settings->curvature_window_m);
                    in.wheel_angle_rad=settings->steering_angle_per_unit_rad*(std::isfinite(drive->steering_applied)?drive->steering_applied:0);
                    in.wheelbase_m=axles.wheelbase();
                    in.trailer_connected=now-trailer_time<=200ms;
                    in.hitch_angle_rad=std::remainder(ego_yaw-trailer_yaw,2*std::numbers::pi);
                    const auto& out=lcc_mpc.update(in);
                    // Status 4 (inputs out of range): the baseline steers this frame.
                    if(in.enable && out.status!=LccMpc::BadInput) {wheel_angle=out.wheel_angle_rad;wheel_rate=out.wheel_rate_rad_s;}
                }
                if(lcc) {
                    steering_target=std::clamp(wheel_angle/settings->steering_angle_per_unit_rad,-1.0,1.0);
                    steering_rate=wheel_rate/settings->steering_angle_per_unit_rad;
                    if(acc) {
                        // Baseline curve speed limit, on the pure pursuit curvature as before.
                        if(std::abs(goal.pursuit_curvature)>0) speed_target=std::min(speed_target,std::sqrt(settings->lateral_acceleration_mps2/std::abs(goal.pursuit_curvature)));
                        cruise_target=speed_target;
                        // The PID stops at the lane end on a speed ramp; the MPC sees a stopped car
                        // there, placed so its 5 m standstill gap leaves the rear point 1 m short.
                        if(mpc) {lead_gap=remaining+4;lead_id=lane_end_id;lead_speed=0;}
                        else speed_target=std::min(speed_target,std::sqrt(2*settings->stopping_deceleration_mps2*std::max(0.0,remaining-1)));
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
                    if(!mpc) {
                        const auto desired=front_axle+settings->standstill_gap_m+std::max(0.0,vehicle->speed_mps)*settings->following_time_s;
                        speed_target=std::min(speed_target,std::max(0.0,actor.speed+settings->following_gain*(gap-desired)));
                    }
                    const auto bumper_gap=mpc?gap-front_offset:gap;
                    if(bumper_gap<lead_gap) {lead_gap=bumper_gap;lead_id=actor.id;lead_speed=actor.speed;}
                }
                const bool lane_end_hold=mpc && acc && lead_id==lane_end_id && acc_mpc.output().mode==AccMpc::Hold;
                if(lcc && (lane_end_hold || (remaining<=1 && (!acc || (!mpc && std::abs(vehicle->speed_mps)<0.1))))) {std::cout<<"Lane reference ended; releasing control"<<std::endl;break;}
            }
            const auto lead=leads.update(lead_id,lead_gap,lead_speed,std::chrono::duration<double>(now-started).count(),{});
            const auto error=speed_target-vehicle->speed_mps;
            ets2_msgs::msg::DriveCommand command;
            command.owner=owner;command.epoch=epoch;command.sequence=++sequence;
            command.sent_steady_ns=steady_ns();
            command.axes=enabled_axes;
            command.steering=lcc && !driver_steering?steering_target:0;
            if(lcc && !driver_steering && settings->relative_steering) {
                if(!std::isfinite(drive->steering_applied)) throw std::runtime_error("Applied steering feedback is unavailable");
                command.steering=eps.command(steering_target,steering_rate,drive->steering_applied,vehicle->speed_mps,dt,eps_settings(*settings));
            }
            const auto speed_kp=(error>=0?settings->throttle_gain:settings->brake_gain);
            if(speed_target==0) speed_integral=0;
            const auto candidate=speed_integral+error*settings->speed_integral_gain*dt;
            const auto derivative=dt>0?-(vehicle->speed_mps-previous_speed)/dt*settings->speed_derivative_gain:0;
            if(speed_target>0 && (std::abs(speed_kp*error+candidate+derivative)<=1 || error*(speed_kp*error+candidate+derivative)<0)) speed_integral=candidate;
            const auto effort=std::clamp(speed_kp*error+speed_integral+derivative,-1.0,1.0);
            previous=now;previous_speed=vehicle->speed_mps;
            command.throttle=acc?std::max(0.0,effort):0;command.brake=acc?std::max(0.0,-effort):0;
            if(mpc) {
                AccMpc::Input in;
                in.enable=acc;in.set_speed_mps=cruise_target;in.time_gap_s=settings->following_time_s;
                in.ego_speed_mps=vehicle->speed_mps;in.ego_accel_mps2=vehicle->acceleration_base_mps2[0];
                in.lead_valid=lead.valid;in.lead_gap_m=lead.gap_m;in.lead_speed_mps=lead.speed_mps;in.lead_accel_mps2=lead.accel_mps2;
                // Resume from HOLD: the driver taps the accelerator.
                in.resume=std::isfinite(drive->throttle_manual) && drive->throttle_manual>.2;
                in.engine_rpm=vehicle->engine_rpm;
                in.gear=actuation && now-actuation_time<=200ms && actuation->engine_gear_available?actuation->engine_gear:0;
                const auto& out=acc_mpc.update(in,std::chrono::duration<double>(now-started).count());
                command.throttle=acc?out.throttle:0;
                if(enabled_axes&4) {command.brake=acc?out.brake:0;command.retarder_step=acc?out.retarder_step:0;command.engine_brake=acc && out.engine_brake;}
                else command.brake=acc?AccMpc::folded_brake(out):0;
            }
            if(enabled_axes&2) pedal_sequence=command.sequence;
            publisher->publish(command);
            if(now>=report) {
                std::cout<<"speed="<<vehicle->speed_mps<<" target="<<speed_target<<" path_error="<<path_error<<" path_offset="<<path_offset<<" remaining="<<remaining<<" lead_gap="<<lead_gap<<" lead_accel="<<lead.accel_mps2
                    <<" steering_target="<<steering_target<<" steering_applied="<<drive->steering_applied<<" steering_input="<<command.steering<<" throttle="<<command.throttle<<" brake="<<command.brake
                    <<" axes="<<unsigned(drive->axes)<<" accepted_sequence="<<drive->sequence
                    <<" driver_throttle="<<drive->throttle_manual<<" driver_steering="<<drive->steering_manual<<(driver_steering?" (LCC yields)":"");
                if(lane && lateral_mpc) std::cout<<" lcc_status="<<unsigned(lcc_mpc.output().status)<<" trailer="<<(now-trailer_time<=200ms)
                    <<" hitch="<<std::remainder(ego_yaw-trailer_yaw,2*std::numbers::pi);
                if(mpc) std::cout<<" acc_mode="<<unsigned(acc_mpc.output().mode)<<" a_des="<<acc_mpc.output().a_des_mps2
                    <<" retarder="<<int(command.retarder_step)<<" engine_brake="<<command.engine_brake<<" lead="<<(!lead.valid?"none":lead.id==lane_end_id?"lane_end":std::to_string(lead.id));
                std::cout<<std::endl;
                report=now+1s;
            }
            next+=20ms;
            if(next<Clock::now()) next=Clock::now(); // overrun: restart the schedule
            std::this_thread::sleep_until(next);
        }
    } catch(const std::exception& error) {std::cerr<<error.what()<<std::endl;result=1;}
    if(epoch) try {control(false);} catch(const std::exception& error) {std::cerr<<"Release: "<<error.what()<<std::endl;}
    rclcpp::shutdown();return result;
}
