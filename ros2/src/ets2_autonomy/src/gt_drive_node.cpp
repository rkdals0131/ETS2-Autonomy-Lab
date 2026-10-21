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
#include "control/hud_path.hpp"
#include "control/road_grade.hpp"
#include "control/lane_merge.hpp"
#include "control/curve_speed.hpp"
#include <ets2_msgs/msg/assist_state.hpp>
#include <ets2_msgs/msg/actuation_state.hpp>
#include <ets2_msgs/msg/powertrain_state.hpp>
#include <nav_msgs/msg/path.hpp>
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
    const bool cruise_engage=initial.engage=="cruise";
    // start: the path file is read once now. cruise: it is the provider's follow output, read at
    // each engagement (it may not exist yet).
    std::unique_ptr<LanePath> lane;
    int lane_side=0; // cruise mode: the file the lane came from, 0 followed, -1/1 left/right side lane
    if(!cruise_engage && !initial.path_file.empty()) try {lane=std::make_unique<LanePath>(initial.path_file);} catch(const std::exception& e) {
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
    ets2_msgs::msg::PowertrainState::SharedPtr powertrain;
    Clock::time_point drive_time{},vehicle_time{},actuation_time{},powertrain_time{};
    auto ds=node->create_subscription<ets2_msgs::msg::DriveState>("/ets2/drive/state",rclcpp::QoS(1),
        [&](ets2_msgs::msg::DriveState::SharedPtr value){drive=std::move(value);drive_time=Clock::now();});
    auto vs=node->create_subscription<ets2_msgs::msg::VehicleState>("/ets2/vehicle/state",rclcpp::QoS(1),
        [&](ets2_msgs::msg::VehicleState::SharedPtr value){vehicle=std::move(value);vehicle_time=Clock::now();});
    // The MPC's allocator uses the engine gear (throttle map, shift freeze).
    auto as=node->create_subscription<ets2_msgs::msg::ActuationState>("/ets2/vehicle/actuation",rclcpp::QoS(1),
        [&](ets2_msgs::msg::ActuationState::SharedPtr value){actuation=std::move(value);actuation_time=Clock::now();});
    // The game's cruise control speed (0 when off) is the engage button in cruise mode; the
    // retarder step applied feeds the ACC's mass estimator.
    auto pts=node->create_subscription<ets2_msgs::msg::PowertrainState>("/ets2/vehicle/powertrain",rclcpp::QoS(1),
        [&](ets2_msgs::msg::PowertrainState::SharedPtr value){powertrain=std::move(value);powertrain_time=Clock::now();});
    const bool mpc=initial.longitudinal=="mpc",lateral_mpc=initial.lateral=="mpc";
    // ego pose is base_link (drive axle); the controller's rear point and wheelbase follow
    // the grounded axles (kinematic_axles.hpp), so a lowered lift axle moves them.
    double ego_x=0,ego_y=0,ego_z=0,ego_yaw=0,front_axle=0;bool pose_received=false,traffic_received=false,wheels_received=false,liftable=false;
    std::map<uint32_t,AxleWheel> axle_wheels;std::map<uint32_t,bool> grounded;
    Clock::time_point pose_time{},traffic_time{};
    builtin_interfaces::msg::Time pose_stamp; // simulation time: the HUD marks' TTL runs on it
    struct Actor {uint64_t id;double x,y,yaw,speed,length,width;};std::vector<Actor> actors;
    rclcpp::Subscription<geometry_msgs::msg::PoseStamped>::SharedPtr ps;
    rclcpp::Subscription<std_msgs::msg::String>::SharedPtr config;
    rclcpp::Subscription<ets2_msgs::msg::WheelState>::SharedPtr ws;
    rclcpp::Subscription<ets2_msgs::msg::TrafficState>::SharedPtr ts;
    // Connected trailer (relay: SDK placement, only while connected) for the MPC's hitch angle.
    rclcpp::Subscription<geometry_msgs::msg::PoseStamped>::SharedPtr trs;
    double trailer_yaw=0;Clock::time_point trailer_time{};
    // GT pose, geometry and traffic: for the lane, and in cruise mode for leads without one.
    const bool ground_truth=lane || cruise_engage;
    if(ground_truth) {
        ps=node->create_subscription<geometry_msgs::msg::PoseStamped>("/ets2/ground_truth/ego/pose",rclcpp::QoS(1),[&](geometry_msgs::msg::PoseStamped::SharedPtr msg) {
            const auto& p=msg->pose.position;const auto& q=msg->pose.orientation;
            if(msg->header.frame_id!="world" || !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z) || !std::isfinite(q.w))
                throw std::runtime_error("Invalid GT ego pose");
            ego_x=p.x;ego_y=p.y;ego_z=p.z;ego_yaw=std::atan2(2*(q.w*q.z+q.x*q.y),1-2*(q.y*q.y+q.z*q.z));pose_received=true;pose_time=Clock::now();pose_stamp=msg->header.stamp;
        });
        // The relay's latched configuration. A copy that does not parse (2026-10-21 20:16: one
        // arrived as Foxglove connected and ended the session) is dropped: the geometry already
        // known stays, and without one the session waits for a good copy.
        config=node->create_subscription<std_msgs::msg::String>("/ets2/sensors/config",rclcpp::QoS(1).transient_local(),[&](std_msgs::msg::String::SharedPtr msg) {
            try {
                const auto data=nlohmann::json::parse(msg->data);double front=0;
                std::map<uint32_t,AxleWheel> wheels;
                for(const auto& w:data.at("vehicle").at("wheels")) {
                    const AxleWheel wheel{w.at("position_base_m").at(0).template get<double>(),w.value("steerable",false),w.value("liftable",false)};
                    if(!std::isfinite(wheel.x)) throw std::runtime_error("nonfinite wheel position");
                    wheels[w.at("index").template get<uint32_t>()]=wheel;
                    if(wheel.steerable) front=std::max(front,wheel.x);
                }
                if(!std::isfinite(front) || front<=0) throw std::runtime_error("no steering axle");
                front_axle=front;axle_wheels=std::move(wheels);
                liftable=std::any_of(axle_wheels.begin(),axle_wheels.end(),[](const auto& w){return w.second.liftable;});
            } catch(const std::exception& e) {
                std::cerr<<"Ignored /ets2/sensors/config ("<<e.what()<<"; "<<msg->data.size()<<" bytes, starts '"<<msg->data.substr(0,40)<<"'); "
                    <<(front_axle>0?"keeping the known geometry":"waiting for a valid one")<<std::endl;
            }
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
    // HUD inputs: LCC's path while it steers (empty otherwise, and at release) at 20 Hz; the
    // ACC mark rides on AssistState. hud_node turns them into the decal and Foxglove markers.
    auto plan_path=node->create_publisher<nav_msgs::msg::Path>("/ets2/drive/plan",rclcpp::QoS(1).best_effort());
    // The lateral MPC's own plan for the rear point over 1.5 s (debug, 20 Hz; empty unless it
    // steers on the MPC): what it intends, against what the truck then does.
    auto mpc_plan=node->create_publisher<nav_msgs::msg::Path>("/ets2/drive/mpc_plan",rclcpp::QoS(1).best_effort());
    const auto plan_clear=[&]() {
        nav_msgs::msg::Path path;path.header.frame_id="world";path.header.stamp=pose_stamp;plan_path->publish(path);mpc_plan->publish(path);
    };
    // Controller internals for recordings (50 Hz while armed) and the lane in use (1 Hz).
    auto assist_state=node->create_publisher<ets2_msgs::msg::AssistState>("/ets2/drive/assist",rclcpp::QoS(10));
    auto lane_path=node->create_publisher<nav_msgs::msg::Path>("/ets2/drive/lane",rclcpp::QoS(1));
    uint64_t epoch=0,sequence=0;int result=0;
    // With loader 0.42+ the MPC also owns the retarder and engine brake (axes bit 4, only
    // together with the pedals); otherwise its retarder steps are folded into the brake.
    bool auxiliary=false;
    // Axes the driver took back in this session: the brake ends ACC (loader 0.44+ keeps LCC);
    // it does not re-engage by itself. LCC keeps its axis: without a lane it searches.
    uint8_t taken=0;
    const auto axes=[&]() {const auto a=configuration.load()->axes()&~taken;return uint8_t(a|(auxiliary && (a&2)?4:0));};
    const auto control=[&](bool enabled) {epoch=request_drive_control(node,service,owner,enabled,epoch,enabled?axes():0);};
    // The game's cruise control speed, 0 when off; NaN while unknown (paused, or no state for 1 s),
    // which is neither an "off" nor an "on" edge.
    const auto cruise_speed=[&]() {
        return powertrain && Clock::now()-powertrain_time<=1s?powertrain->cruise_control_mps:std::numeric_limits<double>::quiet_NaN();
    };
    const auto rear_point=[&]() {
        const auto axles=kinematic_axles(axle_wheels,grounded);
        return std::array<double,2>{ego_x+axles.rear_x*std::cos(ego_yaw),ego_y+axles.rear_x*std::sin(ego_yaw)};
    };
    // Cruise mode: the provider's follow file and its side lanes (<file>.left/.right.json, only
    // where such a lane exists).
    const auto side_file=[&](int side) {
        if(!side) return initial.path_file;
        const auto dot=initial.path_file.rfind('.');
        return initial.path_file.substr(0,dot)+(side<0?".left":".right")+(dot==std::string::npos?"":initial.path_file.substr(dot));
    };
    const auto read_lane=[&](int side)->std::unique_ptr<LanePath> {
        try {return std::make_unique<LanePath>(side_file(side));} catch(const std::exception&) {return nullptr;}
    };
    // The lane the truck is in (nearest, within half its width) from the three files, or none
    // and why not.
    std::string lane_problem;
    const auto find_lane=[&]()->std::pair<std::unique_ptr<LanePath>,int> {
        if(initial.path_file.empty()) {lane_problem="no path_file";return {nullptr,0};}
        const auto [x,y]=rear_point();
        std::pair<std::unique_ptr<LanePath>,int> best{nullptr,0};double best_d=0;
        lane_problem.clear();
        for(const int side:{0,-1,1}) {
            std::unique_ptr<LanePath> fresh;
            try {fresh=std::make_unique<LanePath>(side_file(side));}
            catch(const std::exception& e) {if(!side) lane_problem=std::string("lane file unusable: ")+e.what();continue;}
            const auto p=fresh->track(x,y);
            if(p.d<=fresh->width/2 && fresh->remaining()>50) {if(!best.first || p.d<best_d) {best_d=p.d;best={std::move(fresh),side};}}
            else if(!side) lane_problem="lane file is "+std::to_string(p.d)+" m from the truck, "+std::to_string(fresh->remaining())+" m ahead";
        }
        if(best.first) lane_problem.clear();
        return best;
    };
    constexpr uint64_t lane_end_id=std::numeric_limits<uint64_t>::max();
    // One armed epoch. set_speed is the cruise speed in cruise mode (else the parameter).
    // Paused: the game paused (or a hitch read as one); the standby engages again at the same
    // set speed if the game runs again within 3 s.
    enum class End {Stopped,Released,Toggle,Reengage,Paused};
    double resume_speed=0; // the set speed of the last session, for a resume after a pause
    const auto session=[&](double set_speed)->End {
        taken=0;
        if(!axes()) throw std::runtime_error("Enable ACC or LCC explicitly before starting");
        // Cruise mode: the loader also refuses to arm while the driver's LB+RT is still read as
        // throttle activity (LB freezes the throttle value at 0, so the node cannot see it);
        // the game's cruise keeps the speed meanwhile, so retry for up to 10 s.
        for(const auto give_up=Clock::now()+10s;;) {
            try {control(true);break;} catch(const std::exception&) {
                if(!cruise_engage || !(cruise_speed()>0) || Clock::now()>=give_up || stopped()) throw;
            }
            for(const auto until=Clock::now()+100ms;Clock::now()<until;) {rclcpp::spin_some(node);std::this_thread::sleep_for(5ms);}
        }
        // Wait for the state that shows this epoch armed: an older state (e.g. "disconnected"
        // from the previous relay) would otherwise read as a release in the first frame.
        const auto confirmed=[&]{return drive->armed && drive->owner==owner && drive->epoch==epoch;};
        const auto confirm_until=Clock::now()+1s;
        while(!stopped() && Clock::now()<confirm_until && !confirmed()) {rclcpp::spin_some(node);std::this_thread::sleep_for(2ms);}
        if(!stopped() && !confirmed()) throw std::runtime_error("The armed state did not arrive within 1 s");
        std::cout<<"Driving armed; owner="<<owner<<", epoch="<<epoch<<", axes="<<unsigned(axes());
        if(cruise_engage) std::cout<<", set "<<set_speed*3.6<<" km/h. The cruise control button disengages.";
        else std::cout<<". Ctrl+C releases control.";
        std::cout<<std::endl;
        auto report=Clock::now(),previous=report;double speed_integral=0,previous_speed=vehicle->speed_mps;
        VirtualEps eps;
        LeadTracker leads;AccMpc acc_mpc;LccMpc lcc_mpc;RoadGrade grade;const auto started=Clock::now();
        if(lane) std::cout<<"Lateral: "<<(lateral_mpc?"MPC":"pure pursuit")<<std::endl;
        uint64_t pedal_sequence=0;Clock::time_point driver_steering_until{},next_lane_read=started+500ms,next_hud=started,next_lane_publish=started;
        // LCC steers (merging softly onto the lane at every (re)engagement), yields to the
        // driver's steering, or searches (no lane, or the truck is off it or not yet settled in
        // it, e.g. during the driver's lane change). The steering axis stays owned throughout;
        // the input is 0 unless steering, the same as the stick let go, and the driver's stick
        // always passes alone.
        enum class Lcc:uint8_t {Off,Active,Yield,Search};
        Lcc lcc_state=Lcc::Off;std::optional<LaneMerge> merge;bool lane_replaced=false,was_on_lane=false;
        // Turn signal lane change: the switch's rising edge requests one to that side (both on,
        // the hazards, is none); it starts while LCC steers, once the gap on the side lane is
        // clear, and keeps going until merged (changing: its side).
        int change_request=0,previous_signal=2,changing=0;std::string change_waiting;uint64_t change_blocker=0;
        // The driver's steering: since when without a pause over 0.3 s (the stick crossing centre).
        Clock::time_point steering_since{},steering_seen{};
        // A lane change waits for a gap on the target lane: nobody beside the truck, the vehicle
        // ahead at least 5 m + 0.5 s + 3 s of closing ahead of the front bumper, the one behind
        // at least 5 m + 3 s of its closing behind the rear (trailer included). front and rear
        // are from the lane's reference point at lane distance s.
        const auto gap_problem=[&](const LanePath& target,double s,double front,double rear,double v)->std::pair<std::string,uint64_t> {
            for(const auto& a:actors) {
                const auto p=target.project(a.x,a.y,std::max(0.0,s-200),s+200);
                if(!(p.d<=(target.width+a.width)/2)) continue;
                const auto& A=target.points[p.segment];const auto& B=target.points[p.segment+1];
                if(std::cos(a.yaw-std::atan2(B.y-A.y,B.x-A.x))<=0) continue;
                const auto ahead=p.s-a.length/2-(s+front),behind=(s-rear)-(p.s+a.length/2);
                if(ahead>=0) {if(ahead<5+.5*v+3*std::max(0.0,v-a.speed)) return {"vehicle ahead at "+std::to_string(int(ahead))+" m",a.id};}
                else if(behind>=0) {if(behind<5+3*std::max(0.0,a.speed-v)) return {"vehicle behind at "+std::to_string(int(behind))+" m",a.id};}
                else return {"vehicle beside",a.id};
            }
            return {"",0};
        };
        // Cruise mode: the game's cruise control is still on at arm. A light brake cancels it
        // (as the driver's brake does); a later press of the button turns it on again, which
        // reads as the driver's switch: off while ACC runs, re-engage after a brake cancel.
        bool cancel_pending=cruise_engage,toggle_off=false,cruise_seen_off=false;auto cancel_until=started+3s;
        std::optional<ets2_msgs::msg::DriveCommand> last_command;std::optional<Clock::time_point> stale_since;
        hud::Points mpc_points;
        resume_speed=set_speed;
        // Fixed 50 Hz schedule: sleep_until the next 20 ms deadline keeps the mean
        // period at 20 ms even with Windows' coarse sleep granularity; after an
        // overrun the schedule restarts from now instead of bursting to catch up.
        for(auto next=Clock::now();!stopped() && rclcpp::ok();) {
            rclcpp::spin_some(node);const auto now=Clock::now();
            if(!drive->armed || drive->owner!=owner || drive->epoch!=epoch) {
                if(drive->reason=="paused") {std::cout<<"Released by a game pause"<<std::endl;epoch=0;return End::Paused;}
                if(drive->reason!="manual") throw std::runtime_error("Driving released: "+drive->reason);
                std::cout<<"The driver took over; control released"<<std::endl;epoch=0;return End::Released;
            }
            // Loader 0.44+: the driver's brake revokes the pedals after a command that held them.
            // It ends LCC too; the cruise control button engages both again.
            if(pedal_sequence && drive->sequence>=pedal_sequence && !(drive->axes&2) && !(taken&2)) {
                taken|=2;std::cout<<"The driver's brake: ACC and LCC off"<<std::endl;return End::Released;
            }
            const auto settings=configuration.load();
            auto enabled_axes=axes();
            if(!enabled_axes) {std::cout<<"ACC and LCC disabled; releasing control"<<std::endl;return End::Released;}
            const bool acc=enabled_axes&2;bool lcc=enabled_axes&1;
            if(const auto cruise=cruise_speed();cruise_engage && !std::isnan(cruise)) {
                if(cruise<=0) {
                    cruise_seen_off=true;
                    if(cancel_pending) {cancel_pending=false;if(toggle_off) {std::cout<<"Cruise control button: off"<<std::endl;return End::Toggle;}}
                } else if(!cancel_pending && cruise_seen_off) {
                    if(!acc) {std::cout<<"Cruise control button: ACC again at "<<cruise*3.6<<" km/h"<<std::endl;return End::Reengage;}
                    cancel_pending=toggle_off=true;cancel_until=now+3s;cruise_seen_off=false;
                }
                if(cancel_pending && cruise>0) {
                    // The driver's throttle overrides our brake (LB+RT reads as activity at value 0, hence 3 s).
                    if(drive->throttle_manual>0) cancel_until=std::max(cancel_until,now+1s);
                    else if(now>cancel_until) throw std::runtime_error("The game's cruise control did not cancel on a brake pulse; it keeps the speed");
                }
            }
            // The driver's steering overrides LCC (the loader passes it alone); LCC resumes 0.5 s
            // after the stick is let go and ACC keeps running. Steering for steering_takeover_s
            // ends both.
            if(std::isfinite(drive->steering_manual) && drive->steering_manual!=0) {
                if(now-steering_seen>300ms) steering_since=now;
                steering_seen=now;
                if(lcc) driver_steering_until=now+500ms;
                if(now-steering_since>=std::chrono::duration<double>(settings->steering_takeover_s)) {
                    std::cout<<"The driver steered for "<<settings->steering_takeover_s<<" s: ACC and LCC off"<<std::endl;return End::Released;
                }
            }
            const bool driver_steering=lcc && now<driver_steering_until;
            if(!acc) speed_integral=0;
            // A game hitch stops the state for a moment: up to 1 s the last command is sent again
            // (the core's 200 ms command window stays open) and nothing is computed on old state;
            // longer is a lost game. A paused game ends the session as a pause.
            if(vehicle->paused) {std::cout<<"The game paused"<<std::endl;return End::Paused;}
            if(now-drive_time>1s || now-vehicle_time>1s || !std::isfinite(vehicle->speed_mps))
                throw std::runtime_error("Current game state is unavailable for over 1 s");
            if(now-drive_time>200ms || now-vehicle_time>200ms) {
                if(!stale_since) {stale_since=now;std::cout<<"Game state late; holding the last command"<<std::endl;}
                if(last_command) {
                    auto again=*last_command;again.sequence=++sequence;again.sent_steady_ns=steady_ns();publisher->publish(again);
                }
                next+=20ms;if(next<Clock::now()) next=Clock::now();
                std::this_thread::sleep_until(next);
                continue;
            }
            if(stale_since) {
                std::cout<<"Game state back after "<<std::chrono::duration<double>(now-*stale_since).count()<<" s"<<std::endl;stale_since.reset();
                previous=now; // the controllers' dt skips the hitch
            }
            // Cruise mode, every 0.25 s: follow the provider's revisions of the lane in use (from
            // its file). A truck off its lane (the driver changed lanes) takes the lane it is in
            // from any of the three files at once, without waiting for the provider to reset.
            // When the lane's file moved on to another lane (the provider reset after a lane
            // change), the same lane is taken from the file that has it now, keeping the merge
            // in progress; until then the loaded lane stays.
            if(cruise_engage && lcc && now>=next_lane_read) {
                next_lane_read=now+250ms;
                bool moved=!lane;
                if(lane) try {lane->extend(side_file(lane_side));} catch(const std::exception&) {moved=true;}
                if(!lane || !was_on_lane) {
                    if(auto [fresh,side]=find_lane();fresh) {lane=std::move(fresh);lane_side=side;lane_replaced=true;}
                } else if(moved) {
                    const auto [x,y]=rear_point();
                    const auto offset=lane->offset(x,y,lane->progress),heading=lane->heading(lane->progress);
                    for(const int side:{0,-1,1}) {
                        auto fresh=read_lane(side);
                        if(!fresh) continue;
                        const auto p=fresh->track(x,y);
                        if(std::abs(fresh->offset(x,y,p.s)-offset)>.3 || std::abs(std::remainder(fresh->heading(p.s)-heading,2*std::numbers::pi))>.05) continue;
                        if(merge) merge->s0+=p.s-lane->progress;
                        lane=std::move(fresh);lane_side=side;
                        break;
                    }
                }
            }
            // Turn signal (the driver's switch, from the relay's powertrain state).
            const int signal=powertrain && now-powertrain_time<=1s && powertrain->turn_signal_left!=powertrain->turn_signal_right?(powertrain->turn_signal_left?-1:1):0;
            if(previous_signal==2) previous_signal=signal;
            if(signal!=previous_signal) {
                if(change_request && !signal) std::cout<<"Lane change request withdrawn"<<std::endl;
                change_request=signal;previous_signal=signal;change_waiting.clear();change_blocker=0;
            }
            const char* side_name=change_request<0?"left":"right";
            if(change_request && !(cruise_engage && lcc)) change_request=0;
            else if(change_request && lcc_state==Lcc::Active && !changing && lane && lane_side==0 && was_on_lane) {
                auto target=read_lane(change_request);
                const auto [x,y]=rear_point();
                const auto axles=kinematic_axles(axle_wheels,grounded);
                if(!target) {std::cout<<"Lane change: no lane on the "<<side_name<<std::endl;change_request=0;}
                else {
                    const auto to=LateralController::update(*target,x,y,ego_yaw,vehicle->speed_mps,axles.wheelbase(),lateral_settings(*settings));
                    // The side lane is one lane over: the truck is right of a left lane (offset < 0).
                    if(!(to.offset*change_request>.5*target->width && to.offset*change_request<1.6*target->width)) {
                        std::cout<<"Lane change: the "<<side_name<<" lane is not beside the truck (offset "<<to.offset<<" m)"<<std::endl;change_request=0;
                    } else if(const auto [problem,blocker]=gap_problem(*target,to.s,front_axle+settings->front_overhang_m-axles.rear_x,
                                                             (now-trailer_time<=200ms?15.0:3.0),std::max(0.0,vehicle->speed_mps));!problem.empty()) {
                        if(problem!=change_waiting) std::cout<<"Lane change to the "<<side_name<<" waits: "<<problem<<std::endl;
                        change_waiting=problem;change_blocker=blocker;
                    } else {
                        merge=LaneMerge::start(to.s,to.offset,to.heading_error,vehicle->speed_mps,settings->lane_change_lateral_acceleration_mps2,settings->lane_change_min_s);
                        lane=std::move(target);lane_side=change_request;changing=change_request;change_request=0;change_waiting.clear();change_blocker=0;
                        std::cout<<"Lane change to the "<<side_name<<" over "<<merge->length<<" m"<<std::endl;
                    }
                }
            }
            const auto dt=std::chrono::duration<double>(now-previous).count();
            if(ground_truth) grade.update(ego_x,ego_y,ego_z);
            auto speed_target=cruise_engage?set_speed:settings->target_speed_mps;
            auto steering_target=settings->steering_target;double steering_rate=0;
            double path_error=0,path_offset=0,remaining=0,lead_gap=std::numeric_limits<double>::infinity(),lead_ahead=lead_gap;
            std::optional<uint64_t> lead_id;double lead_speed=0;
            // MPC lead: bumper-to-bumper gap; front_offset is the gap reference point to the front bumper.
            double front_offset=front_axle+settings->front_overhang_m,cruise_target=speed_target;
            if(ground_truth && (now-pose_time>200ms || now-traffic_time>200ms)) throw std::runtime_error("Current GT pose/traffic observation is unavailable");
            // Lane errors (against the smoothed lane, at the rear point) and the LCC state. LCC
            // takes the lane once the truck is settled in it (a quarter lane width, 0.1 rad) and
            // the driver has let go; it keeps it while within half a lane width.
            const auto axles=kinematic_axles(axle_wheels,grounded);
            LateralController::Output goal{};bool on_lane=false,settled=false;
            if(lane) {
                const auto [rear_x,rear_y]=rear_point();
                goal=LateralController::update(*lane,rear_x,rear_y,ego_yaw,vehicle->speed_mps,axles.wheelbase(),lateral_settings(*settings));
                path_error=goal.distance;path_offset=goal.offset;remaining=goal.remaining;
                // On the lane: within half its width of the path LCC steers on (the merge's, during
                // a merge or lane change).
                on_lane=std::abs(goal.offset-(merge?merge->at(goal.s).offset:0))<=lane->width/2;
                // Taken once the merge back stays inside the lane: heading away from the centre,
                // turning back takes v^2 h^2 / 2 of lateral travel at 1 m/s^2.
                const auto v=std::max(0.0,vehicle->speed_mps),h=goal.heading_error;
                settled=std::abs(goal.offset)+(h*goal.offset>0?v*v*h*h/2:0)<=lane->width/2-.4 && std::abs(h)<=.1;
            }
            const auto previous_lcc=lcc_state;
            if(!lcc) lcc_state=Lcc::Off;
            else if(!lane && !cruise_engage) lcc_state=driver_steering?Lcc::Yield:Lcc::Active; // start mode, no lane: steering_target
            else if(driver_steering) lcc_state=on_lane?Lcc::Yield:Lcc::Search;
            else if(lcc_state==Lcc::Active?on_lane:settled) lcc_state=Lcc::Active;
            else lcc_state=Lcc::Search;
            if(lcc_state!=Lcc::Active) merge.reset();
            else if(lane && (previous_lcc!=Lcc::Active || lane_replaced)) merge=LaneMerge::start(goal.s,goal.offset,goal.heading_error,vehicle->speed_mps);
            if(merge && merge->done(goal.s)) merge.reset();
            if(changing && (!merge || lcc_state!=Lcc::Active)) {
                std::cout<<"Lane change to the "<<(changing<0?"left":"right")<<(merge?" interrupted":" done")<<std::endl;changing=0;
            }
            was_on_lane=lane && on_lane;
            if(lcc_state!=previous_lcc) {
                std::cout<<"LCC: "<<(lcc_state==Lcc::Active?"steering":lcc_state==Lcc::Yield?"yields to the driver's steering":lcc_state==Lcc::Off?"off":"searching for a lane");
                if(lcc_state==Lcc::Search) {
                    if(!lane) std::cout<<" ("<<lane_problem<<")";
                    else std::cout<<" (offset "<<goal.offset<<" m, heading error "<<goal.heading_error<<" rad, lane width "<<lane->width<<" m)";
                }
                if(merge) std::cout<<"; merging over "<<merge->length<<" m from "<<goal.offset<<" m";
                std::cout<<std::endl;
            } else if(lane_replaced && merge) std::cout<<"LCC: new lane (path "<<lane->path_id<<"); merging over "<<merge->length<<" m from "<<goal.offset<<" m"<<std::endl;
            lane_replaced=false;
            // Start mode: a fixed lane that the truck left is not coming back.
            if(lcc_state==Lcc::Search && lane && !cruise_engage && !acc) throw std::runtime_error("LCC lane reference lost; releasing assistance");
            const bool steering=lcc_state==Lcc::Active,lane_frame=lane && on_lane;
            if(!steering) eps.reset();
            LaneMerge::Reference reference{0,0,0};
            double wheel_angle=0,wheel_rate=0;
            if(lane) {
                if(merge) reference=merge->at(goal.s);
                if(lane_frame) front_offset-=axles.rear_x; // lane distances are from the rear point
                wheel_angle=goal.wheel_angle;wheel_rate=goal.wheel_rate;
                if(lateral_mpc) {
                    LccMpc::Input in;
                    in.enable=steering;in.speed_mps=vehicle->speed_mps;
                    in.lateral_error_m=goal.offset-reference.offset;in.heading_error_rad=goal.heading_error-reference.heading;
                    for(int i=0;i<LccMpc::preview_points;++i) {
                        const auto s=goal.s+i*LccMpc::preview_step_m;
                        in.curvature_preview[i]=lane->curvature(s,settings->curvature_window_m)+(merge?merge->at(s).curvature:0);
                    }
                    in.wheel_angle_rad=settings->steering_angle_per_unit_rad*(std::isfinite(drive->steering_applied)?drive->steering_applied:0);
                    in.wheelbase_m=axles.wheelbase();
                    in.trailer_connected=now-trailer_time<=200ms;
                    in.hitch_angle_rad=std::remainder(ego_yaw-trailer_yaw,2*std::numbers::pi);
                    const auto& out=lcc_mpc.update(in);
                    // Status 4 (inputs out of range): the baseline steers this frame.
                    if(in.enable && out.status!=LccMpc::BadInput) {wheel_angle=out.wheel_angle_rad;wheel_rate=out.wheel_rate_rad_s;}
                    // Its plan in the world: sample i is i*dt ahead at the current speed, moved left
                    // of the lane by the reference (the merge) plus the planned error.
                    mpc_points.clear();
                    if(in.enable && out.status==LccMpc::Mpc)
                        for(int i=0;i<LccMpc::predicted_points;++i) {
                            const auto s=goal.s+std::max(0.0,vehicle->speed_mps)*i*out.predicted_dt_s;
                            const auto c=lane->at(s);const auto h=lane->heading(s);
                            const auto r=(merge?merge->at(s).offset:0)+out.predicted_offset_m[i];
                            mpc_points.push_back({c.x-std::sin(h)*r,c.y+std::cos(h)*r});
                        }
                }
                if(steering) {
                    steering_target=std::clamp(wheel_angle/settings->steering_angle_per_unit_rad,-1.0,1.0);
                    steering_rate=wheel_rate/settings->steering_angle_per_unit_rad;
                    if(acc) {
                        // Curve speed from the lane ahead (curve_speed.hpp).
                        speed_target=std::min(speed_target,curve_speed(*lane,goal.s,settings->lateral_acceleration_mps2,settings->curve_deceleration_mps2));
                        cruise_target=speed_target;
                        // The PID stops at the lane end on a speed ramp; the MPC sees a stopped car
                        // there, placed so its 5 m standstill gap leaves the rear point 1 m short.
                        if(mpc) {lead_gap=remaining+4;lead_id=lane_end_id;lead_speed=0;lead_ahead=remaining;}
                        else speed_target=std::min(speed_target,std::sqrt(2*settings->stopping_deceleration_mps2*std::max(0.0,remaining-1)));
                    }
                }
            }
            if(ground_truth && acc) for(const auto& actor:actors) {
                double ahead,lateral,heading;
                if(lane_frame) {
                    const auto p=lane->project(actor.x,actor.y,lane->progress);
                    const auto a=lane->points[p.segment],b=lane->points[p.segment+1];
                    // Beside the lane centre or the truck (off the centre while merging).
                    ahead=p.s-lane->progress;lateral=std::min(p.d,std::abs(p.offset-goal.offset));heading=std::atan2(b.y-a.y,b.x-a.x);
                } else {
                    // Human steering owns the current direction. Do not brake
                    // because the original LCC path ends or the driver changes lane.
                    const auto dx=actor.x-ego_x,dy=actor.y-ego_y;
                    ahead=dx*std::cos(ego_yaw)+dy*std::sin(ego_yaw);
                    lateral=std::abs(-dx*std::sin(ego_yaw)+dy*std::cos(ego_yaw));heading=ego_yaw;
                }
                const auto lane_width=lane?lane->width:3.5;
                if(ahead<=0 || lateral>(lane_width+actor.width)/2 || std::cos(actor.yaw-heading)<=0) continue;
                const auto gap=ahead-actor.length/2;
                if(!mpc) {
                    const auto desired=front_axle+settings->standstill_gap_m+std::max(0.0,vehicle->speed_mps)*settings->following_time_s;
                    speed_target=std::min(speed_target,std::max(0.0,actor.speed+settings->following_gain*(gap-desired)));
                }
                const auto bumper_gap=mpc?gap-front_offset:gap;
                if(bumper_gap<lead_gap) {lead_gap=bumper_gap;lead_id=actor.id;lead_speed=actor.speed;lead_ahead=gap;}
            }
            if(lane) {
                const bool lane_end_hold=mpc && acc && lead_id==lane_end_id && acc_mpc.output().mode==AccMpc::Hold;
                if(steering && (lane_end_hold || (remaining<=1 && (!acc || (!mpc && std::abs(vehicle->speed_mps)<0.1))))) {std::cout<<"Lane reference ended; releasing control"<<std::endl;return End::Released;}
            }
            const auto lead=leads.update(lead_id,lead_gap,lead_speed,std::chrono::duration<double>(now-started).count(),{});
            const auto error=speed_target-vehicle->speed_mps;
            ets2_msgs::msg::DriveCommand command;
            command.owner=owner;command.epoch=epoch;command.sequence=++sequence;
            command.sent_steady_ns=steady_ns();
            command.axes=enabled_axes;
            command.steering=steering?steering_target:0;
            if(steering && settings->relative_steering) {
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
                // Mass estimator: grade (NaN unknown), no prior yet (learns from 20.3 t), and the
                // applied pedals and retarder so the driver's own throttle teaches it too.
                in.pitch_rad=grade.grade;in.mass_prior_kg=0;
                in.actuation_valid=actuation && now-actuation_time<=200ms;
                if(in.actuation_valid) {in.throttle_applied=actuation->throttle_applied;in.brake_applied=actuation->brake_applied;}
                in.retarder_applied=powertrain && now-powertrain_time<=200ms?int32_t(powertrain->retarder_step):-1;
                const auto& out=acc_mpc.update(in,std::chrono::duration<double>(now-started).count());
                command.throttle=acc?out.throttle:0;
                if(enabled_axes&4) {command.brake=acc?out.brake:0;command.retarder_step=acc?out.retarder_step:0;command.engine_brake=acc && out.engine_brake;}
                else command.brake=acc?AccMpc::folded_brake(out):0;
            }
            if(cancel_pending && acc && cruise_speed()>0) {command.throttle=0;command.brake=std::max(command.brake,.05f);}
            if(enabled_axes&2) pedal_sequence=command.sequence;
            publisher->publish(command);last_command=command;
            // HUD inputs. The ACC mark: the lead's rear bumper (150 m at most), or without a lead
            // the gap ACC keeps (front bumper + 5 m + time gap); along the lane when the truck is
            // on it, else on the arc of its yaw rate.
            const auto v=std::max(0.0,vehicle->speed_mps);
            const bool lead_marked=acc && lead.valid && lead.id!=lane_end_id && std::isfinite(lead_ahead) && lead_ahead<=150;
            std::optional<double> mark;
            if(lead_marked) mark=std::max(2.0,lead_ahead);
            else if(acc) mark=front_offset+5+settings->following_time_s*v;
            std::array<double,3> mark_pose{};
            if(mark && lane_frame) {
                const auto s=lane->progress+*mark;const auto p=lane->at(s);
                mark_pose={p.x,p.y,lane->heading(s)};
            } else if(mark && ground_truth) {
                const auto yaw_rate=vehicle->angular_velocity_base_radps[2];
                const auto arc=hud::arc(ego_x,ego_y,ego_yaw,v>1 && std::isfinite(yaw_rate)?yaw_rate/v:0,*mark);
                const auto [px,py]=arc[arc.size()-2];const auto [x,y]=arc.back();
                mark_pose={x,y,std::atan2(y-py,x-px)};
            }
            // LCC's path at 20 Hz: from 1 m past the front bumper (front_offset is from the rear
            // point here; under the cab it only overlapped the truck) to the ACC mark, or 3 s ahead
            // (40-120 m); through the merge during a (re)engagement or lane change.
            if(ground_truth && now>=next_hud) {
                next_hud=now+50ms;
                nav_msgs::msg::Path path;path.header.frame_id="world";path.header.stamp=pose_stamp;
                const auto ribbon_start=front_offset+1,ribbon_end=mark?*mark:ribbon_start+std::clamp(3*v,40.0,120.0);
                if(steering && lane && ribbon_end-ribbon_start>=hud::step_m)
                    for(const auto& [x,y]:hud::lane(*lane,lane->progress+ribbon_start,ribbon_end-ribbon_start,merge?&*merge:nullptr)) {
                        geometry_msgs::msg::PoseStamped pose;pose.pose.position.x=x;pose.pose.position.y=y;pose.pose.orientation.w=1;path.poses.push_back(pose);
                    }
                plan_path->publish(path);
                nav_msgs::msg::Path plan;plan.header=path.header;
                if(steering) for(const auto& [x,y]:mpc_points) {
                    geometry_msgs::msg::PoseStamped pose;pose.pose.position.x=x;pose.pose.position.y=y;pose.pose.orientation.w=1;plan.poses.push_back(pose);
                }
                mpc_plan->publish(plan);
            }
            {
                ets2_msgs::msg::AssistState state;
                state.header.stamp=pose_stamp;state.header.frame_id="world";
                state.lcc_state=uint8_t(lcc_state);state.acc_active=acc;
                if(lane) {
                    state.lane_path_id=lane->path_id;state.lane_s_m=goal.s;state.lane_remaining_m=remaining;
                    state.lateral_offset_m=goal.offset;state.heading_error_rad=goal.heading_error;state.lane_curvature=goal.path_curvature;
                }
                state.merge_offset_m=reference.offset;state.merge_heading_rad=reference.heading;
                state.lane_source=int8_t(lane_side);state.lane_change_request=int8_t(change_request);state.lane_change_active=int8_t(changing);
                state.lcc_status=lane && lateral_mpc?lcc_mpc.output().status:0;
                state.wheel_angle_target_rad=wheel_angle;state.wheel_rate_target_rad_s=wheel_rate;
                state.steering_target=steering_target;state.steering_input=command.steering;state.steering_applied=drive->steering_applied;
                state.set_speed_mps=cruise_engage?set_speed:settings->target_speed_mps;state.target_speed_mps=mpc?cruise_target:speed_target;
                if(mpc) {state.acc_mode=acc_mpc.output().mode;state.accel_target_mps2=acc_mpc.output().a_des_mps2;state.mass_kg=acc_mpc.output().mass_kg;}
                state.lead_valid=lead.valid;state.lead_gap_m=lead.gap_m;state.lead_speed_mps=lead.speed_mps;state.grade=grade.grade;
                state.acc_mark=mark && ground_truth?(lead_marked?2:1):0;
                state.acc_mark_x=mark_pose[0];state.acc_mark_y=mark_pose[1];state.acc_mark_heading=mark_pose[2];
                state.lane_width_m=lane?lane->width:0;state.driver_throttle=acc && drive->throttle_manual>0;
                state.lead_id=lead_marked?lead.id:0;state.lane_change_blocker_id=change_request?change_blocker:0;
                state.target_gap_m=5+settings->following_time_s*v;state.curve_lateral_acceleration_mps2=settings->lateral_acceleration_mps2;
                // Truck outline: front bumper (estimate, front_overhang_m) and 1 m behind the last axle.
                double last_axle=0;for(const auto& [index,w]:axle_wheels) last_axle=std::min(last_axle,w.x);
                state.truck_front_m=front_axle+settings->front_overhang_m;state.truck_rear_m=last_axle-1;
                assist_state->publish(state);
            }
            // The lane in use, 50 m behind to 400 m ahead, once a second.
            if(lane && now>=next_lane_publish) {
                next_lane_publish=now+1s;
                nav_msgs::msg::Path path;path.header.frame_id="world";path.header.stamp=pose_stamp;
                for(const auto& p:lane->points) if(p.s>=lane->progress-50 && p.s<=lane->progress+400) {
                    geometry_msgs::msg::PoseStamped pose;pose.pose.position.x=p.x;pose.pose.position.y=p.y;pose.pose.orientation.w=1;path.poses.push_back(pose);
                }
                lane_path->publish(path);
            }
            if(now>=report) {
                std::cout<<"speed="<<vehicle->speed_mps<<" target="<<speed_target<<" path_error="<<path_error<<" path_offset="<<path_offset<<" remaining="<<remaining<<" lead_gap="<<lead_gap<<" lead_accel="<<lead.accel_mps2
                    <<" steering_target="<<steering_target<<" steering_applied="<<drive->steering_applied<<" steering_input="<<command.steering<<" throttle="<<command.throttle<<" brake="<<command.brake
                    <<" axes="<<unsigned(drive->axes)<<" accepted_sequence="<<drive->sequence
                    <<" driver_throttle="<<drive->throttle_manual<<" driver_steering="<<drive->steering_manual<<(driver_steering?" (LCC yields)":"");
                if(cruise_engage) std::cout<<" game_cruise="<<cruise_speed();
                std::cout<<" lcc="<<(lcc_state==Lcc::Active?"steering":lcc_state==Lcc::Yield?"yield":lcc_state==Lcc::Off?"off":"search")<<" merge_offset="<<reference.offset<<" lane_source="<<lane_side<<" lane_change="<<changing;
                if(lane && lateral_mpc) std::cout<<" lcc_status="<<unsigned(lcc_mpc.output().status)<<" trailer="<<(now-trailer_time<=200ms)
                    <<" hitch="<<std::remainder(ego_yaw-trailer_yaw,2*std::numbers::pi);
                if(mpc) std::cout<<" acc_mode="<<unsigned(acc_mpc.output().mode)<<" a_des="<<acc_mpc.output().a_des_mps2
                    <<" retarder="<<int(command.retarder_step)<<" engine_brake="<<command.engine_brake
                    <<" mass_t="<<acc_mpc.output().mass_kg/1000<<(acc_mpc.output().mass_valid?"":"(learning)")<<" grade="<<grade.grade<<" lead="<<(!lead.valid?"none":lead.id==lane_end_id?"lane_end":std::to_string(lead.id));
                std::cout<<std::endl;
                report=now+1s;
            }
            next+=20ms;
            if(next<Clock::now()) next=Clock::now(); // overrun: restart the schedule
            std::this_thread::sleep_until(next);
        }
        return End::Stopped;
    };
    // Outside a session the assistance is off: AssistState says so at once and every 0.5 s
    // after (Foxglove's indicators and the hud node read the latest message; without it the
    // last armed state stayed on screen after a brake, 2026-10-21).
    const auto publish_off=[&]() {
        ets2_msgs::msg::AssistState state;state.header.stamp=pose_stamp;state.header.frame_id="world";
        assist_state->publish(state);
    };
    const auto release=[&]() {
        if(epoch) try {control(false);} catch(const std::exception& error) {std::cerr<<"Release: "<<error.what()<<std::endl;}
        epoch=0;plan_clear();publish_off();
    };
    const auto driver_input=[&]() {
        return ((axes()&1) && drive->steering_manual!=0) || ((axes()&2) && (drive->throttle_manual!=0 || drive->brake_manual!=0));
    };
    try {
        // Cruise mode is resident: wait for the game (it may be paused or loading) without a deadline.
        const auto until=cruise_engage?Clock::time_point::max():Clock::now()+10s;
        if(cruise_engage) std::cout<<"Waiting for the bridge and the game"<<std::endl;
        const auto discovered=[&]() {
            return drive && vehicle && (!ground_truth || (pose_received && traffic_received && front_axle>0 && (wheels_received || !liftable)))
                && (!cruise_engage || powertrain) && service->service_is_ready() && publisher->get_subscription_count();
        };
        while(!stopped() && Clock::now()<until && !discovered()) {rclcpp::spin_some(node);std::this_thread::sleep_for(10ms);}
        if(stopped()) throw std::runtime_error("Stopped before arm");
        if(!discovered()) throw std::runtime_error("Bridge discovery timed out");
        if(!drive->available || !drive->permitted || !drive->profile_supported || (!cruise_engage && vehicle->paused)) throw std::runtime_error("Game input is unavailable or paused");
        if(!axes()) throw std::runtime_error("Enable ACC or LCC explicitly before starting");
        if(axes()!=3 && !drive->independent_axes) throw std::runtime_error("Independent ACC/LCC requires the updated loader; restart the game");
        auxiliary=mpc && drive->auxiliary_brakes;
        if(mpc) std::cout<<"Longitudinal: MPC, retarder "<<(auxiliary?"owned":"folded into the brake (loader < 0.42)")<<std::endl;
        if(!cruise_engage) {
            // The loader refuses to arm an axis the driver is using (its reason then reads only "disabled").
            if(driver_input()) throw std::runtime_error("Driver input on a requested axis; let go of the pedals and stick, then start again");
            session(0);
        } else {
            if(!(axes()&2)) throw std::runtime_error("Cruise engage needs ACC enabled");
            std::cout<<"Standby: the game's cruise control button engages"<<std::endl;
            bool wait_off=!(cruise_speed()<=0),reengage=false;
            std::optional<Clock::time_point> resume_until; // after a pause: engage again by itself until then
            auto next_off=Clock::now();
            while(!stopped() && rclcpp::ok()) {
                rclcpp::spin_some(node);
                if(Clock::now()>=next_off) {next_off=Clock::now()+500ms;publish_off();}
                if(resume_until) {
                    const auto now=Clock::now();
                    const bool running=vehicle && drive && now-vehicle_time<=200ms && now-drive_time<=200ms && !vehicle->paused && drive->available;
                    if(now>resume_until) {resume_until.reset();std::cout<<"The pause lasted over 3 s; standby: the game's cruise control button engages"<<std::endl;continue;}
                    if(!running || driver_input()) {std::this_thread::sleep_for(20ms);continue;}
                    resume_until.reset();
                    std::cout<<"The game runs again: engaging at "<<resume_speed*3.6<<" km/h"<<std::endl;
                    taken=0;std::tie(lane,lane_side)=find_lane();
                    End end=End::Released;
                    try {end=session(resume_speed);} catch(const std::exception& error) {std::cerr<<error.what()<<std::endl;}
                    release();taken=0;
                    if(end==End::Stopped) break;
                    if(end==End::Paused) {resume_until=Clock::now()+3s;continue;}
                    reengage=end==End::Reengage;wait_off=!reengage;
                    std::cout<<(reengage?"Re-engaging":"Standby: the game's cruise control button engages")<<std::endl;
                    continue;
                }
                if(!reengage) {
                    const auto cruise=cruise_speed();
                    if(cruise<=0) wait_off=false;
                    if(wait_off || !(cruise>0)) {std::this_thread::sleep_for(20ms);continue;}
                }
                reengage=false;
                // Pressing LB+RT pulls the throttle, and the loader refuses to arm a used
                // axis: the game's cruise control keeps the speed until the driver lets go.
                const auto give_up=Clock::now()+10s;
                while(!stopped() && Clock::now()<give_up && cruise_speed()>0 && driver_input()) {rclcpp::spin_some(node);std::this_thread::sleep_for(20ms);}
                if(stopped()) break;
                if(!(cruise_speed()>0)) continue;
                if(driver_input()) {std::cout<<"Driver input did not stop within 10 s; press the cruise control button again"<<std::endl;wait_off=true;continue;}
                if(vehicle->paused) {wait_off=true;continue;}
                taken=0;std::tie(lane,lane_side)=find_lane();
                End end=End::Released;
                try {end=session(cruise_speed());} catch(const std::exception& error) {std::cerr<<error.what()<<std::endl;}
                release();taken=0;
                if(end==End::Stopped) break;
                if(end==End::Paused) {resume_until=Clock::now()+3s;continue;}
                reengage=end==End::Reengage;wait_off=!reengage;
                std::cout<<(reengage?"Re-engaging":"Standby: the game's cruise control button engages")<<std::endl;
            }
        }
    } catch(const std::exception& error) {std::cerr<<error.what()<<std::endl;result=1;}
    release();
    rclcpp::shutdown();return result;
}
