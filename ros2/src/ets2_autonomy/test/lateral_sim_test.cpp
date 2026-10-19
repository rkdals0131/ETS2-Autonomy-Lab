// Offline closed loop: kinematic bicycle + measured relative-steering plant
// (integrator 0.65/s, one game frame delay, 60 Hz game) and the 50 Hz
// controller. The baseline (pure pursuit + applied-steering PI) runs in the same
// loop for comparison only; its metrics are printed, not asserted.
#include "control/lateral_controller.hpp"
#include "control/virtual_eps.hpp"
#include <gtest/gtest.h>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <numbers>
#include <string>

namespace {
constexpr double wheelbase=3.824,angle_per_unit=.7087,game_dt=1.0/60,control_dt=.02;

using Points=std::vector<std::array<double,2>>;
// Polyline of straights (curvature 0) and arcs, 1.5 m spacing like the map lanes.
struct PathBuilder {
    Points points{{0,0}};double heading=0;
    PathBuilder& add(double length,double curvature) {
        constexpr double step=1.5;
        for(double s=step;s<=length+1e-9;s+=step) {
            auto [x,y]=points.back();
            if(curvature==0) {x+=step*std::cos(heading);y+=step*std::sin(heading);}
            else {const auto next=heading+step*curvature;x+=(std::sin(next)-std::sin(heading))/curvature;y-=(std::cos(next)-std::cos(heading))/curvature;heading=next;}
            points.push_back({x,y});
        }
        return *this;
    }
};

struct Plant {double rate=.65,centering=0,max=.94;};
struct Scenario {std::string name;Points points;double speed,offset=0,duration=0,settle_s=0;Plant plant{};};
struct Metrics {double max_offset=0,settled_offset=0,rms=0,overshoot=0,max_applied=0;int reversals=0;double input_variation=0;};

// A controller maps (lane, pose, speed, applied steering) to the relative input u.
using Controller=std::function<double(LanePath&,double,double,double,double,double)>;

Controller baseline() {
    return [integral=0.0](LanePath& lane,double x,double y,double yaw,double v,double applied) mutable {
        const auto goal=lane.follow(x,y,yaw,6+std::max(0.0,v)*.7);
        const auto target=std::clamp(std::atan(wheelbase*goal.curvature)/.7,-1.0,1.0);
        const auto error=target-applied,proportional=2*error,candidate=integral+error*1*control_dt;
        if(std::abs(proportional+candidate)<=1 || error*(proportional+candidate)<0) integral=candidate;
        return std::clamp(proportional+integral,-1.0,1.0);
    };
}
Controller current(VirtualEps::Settings eps_settings={},LateralController::Settings settings={}) {
    return [eps=VirtualEps{},eps_settings,settings](LanePath& lane,double x,double y,double yaw,double v,double applied) mutable {
        const auto out=LateralController::update(lane,x,y,yaw,v,wheelbase,settings);
        const auto target=std::clamp(out.wheel_angle/angle_per_unit,-1.0,1.0);
        return eps.command(target,out.wheel_rate/angle_per_unit,applied,control_dt,eps_settings);
    };
}

Metrics simulate(const Scenario& scenario,Controller controller) {
    LanePath lane(scenario.points,3.5),truth(scenario.points,3.5);
    const auto heading=lane.heading(0);const auto start=lane.at(0);
    double x=start.x-std::sin(heading)*scenario.offset,y=start.y+std::cos(heading)*scenario.offset,yaw=heading;
    double applied=std::clamp(std::atan(wheelbase*lane.curvature(0,5))/angle_per_unit,-1.0,1.0);
    double latched=0,active=0,command=0,next_control=.003; // controller phase vs game frames
    Metrics m;double sum2=0,last_u=0,extreme=applied;int direction=0,samples=0;
    const auto frames=int(scenario.duration/game_dt);
    for(int n=0;n<frames;++n) {
        const auto t=n*game_dt;
        // Controller ticks in (t_{n-1}, t_n] see the state after frame n-1.
        for(;next_control<=t;next_control+=control_dt) {
            command=controller(lane,x,y,yaw,scenario.speed,applied);
            m.input_variation+=std::abs(command-last_u);last_u=command;
            const auto p=truth.track(x,y);
            m.max_offset=std::max(m.max_offset,std::abs(p.offset));
            if(next_control>=scenario.settle_s) {m.settled_offset=std::max(m.settled_offset,std::abs(p.offset));sum2+=p.offset*p.offset;++samples;}
            if(scenario.offset>0) m.overshoot=std::max(m.overshoot,-p.offset);
        }
        // The command latched at the previous frame boundary acts over this frame.
        active=latched;latched=command;
        applied=std::clamp(applied+game_dt*(scenario.plant.rate*active-scenario.plant.centering*applied),-scenario.plant.max,scenario.plant.max);
        const auto angle=angle_per_unit*applied;
        // Midpoint heading: explicit Euler would bias heading against travel direction.
        const auto turn=scenario.speed*std::tan(angle)/wheelbase*game_dt;
        x+=scenario.speed*std::cos(yaw+turn/2)*game_dt;y+=scenario.speed*std::sin(yaw+turn/2)*game_dt;yaw+=turn;
        m.max_applied=std::max(m.max_applied,std::abs(applied));
        // Steering direction reversals larger than 0.002 units (hysteresis).
        if(direction>=0 && applied<extreme-.002) {if(direction>0) ++m.reversals;direction=-1;extreme=applied;}
        else if(direction<=0 && applied>extreme+.002) {if(direction<0) ++m.reversals;direction=1;extreme=applied;}
        else if(direction>0) extreme=std::max(extreme,applied);
        else if(direction<0) extreme=std::min(extreme,applied);
    }
    m.rms=samples?std::sqrt(sum2/samples):0;m.input_variation/=scenario.duration;
    return m;
}

void print(const char* who,const Scenario& s,const Metrics& m) {
    std::printf("| %-34s | %-4s | %.3f | %.3f | %.4f | %.3f | %3d | %.3f | %.3f |\n",
                s.name.c_str(),who,m.max_offset,m.settled_offset,m.rms,m.overshoot,m.reversals,m.input_variation,m.max_applied);
}
std::pair<Metrics,Metrics> compare(const Scenario& s,Controller controller=current()) {
    const auto old=simulate(s,baseline()),now=simulate(s,controller);
    print("old",s,old);print("new",s,now);
    return {old,now};
}
std::string speed_name(double speed) {return std::to_string(int(speed))+" m/s";}

Scenario arc(double radius,double speed) {
    const auto duration=60.0;
    return {"arc R"+std::to_string(int(radius))+" "+speed_name(speed),
            PathBuilder{}.add(100,0).add(speed*duration+200,1/radius).points,speed,0,duration,100/speed+20};
}
} // namespace

TEST(LanePath,WindowedTrackMatchesFullScan) {
    LanePath windowed(PathBuilder{}.add(50,0).add(800,1/300.0).add(300,-1/500.0).points,3.5);
    LanePath full=windowed;
    for(double s=0;s<1100;s+=.37) {
        const auto p=windowed.at(s);const auto h=windowed.heading(s);
        const auto x=p.x-std::sin(h)*.8,y=p.y+std::cos(h)*.8;
        const auto a=windowed.track(x,y),b=full.project(x,y,full.progress);full.progress=std::max(full.progress,b.s);
        ASSERT_NEAR(a.s,b.s,1e-9);ASSERT_NEAR(a.offset,.8,1e-2);
    }
}

TEST(LanePath,CurvatureOfArcs) {
    LanePath lane(PathBuilder{}.add(100,0).add(400,1/400.0).add(400,-1/1000.0).points,3.5);
    EXPECT_NEAR(lane.curvature(50,5),0,1e-9);
    EXPECT_NEAR(lane.curvature(300,5),1/400.0,1e-6);
    EXPECT_NEAR(lane.curvature(700,5),-1/1000.0,1e-6);
}

TEST(VirtualEps,TracksRampWithoutLagAndHolds) {
    VirtualEps eps;double applied=0;
    for(int k=0;k<100;++k) {const auto target=.2*k*control_dt;applied+=.65*control_dt*eps.command(target,.2,applied,control_dt,{});}
    EXPECT_NEAR(applied,.2*100*control_dt,1e-9); // exact for the undelayed ideal integrator
    VirtualEps limited;const VirtualEps::Settings slow{.65,10,.3};
    const auto first=limited.command(1,0,0,control_dt,slow);
    EXPECT_NEAR(first,.3/.65+10*.3*control_dt,1e-12); // target slews from applied, not jumps
    EXPECT_EQ(VirtualEps{}.command(.5,0,.5,control_dt,{}),0);
}

TEST(LateralSim,StraightWithOffset) {
    for(const double speed:{15.0,25.0}) {
        const Scenario s{"straight 1 m offset "+speed_name(speed),PathBuilder{}.add(speed*40+200,0).points,speed,1,40,20};
        const auto [old,now]=compare(s);
        EXPECT_LT(now.overshoot,.08);EXPECT_LT(now.settled_offset,.005);EXPECT_LE(now.reversals,3);
    }
}

TEST(LateralSim,ConstantRadius) {
    for(const double radius:{400.0,1000.0}) for(const double speed:{15.0,25.0}) {
        const auto [old,now]=compare(arc(radius,speed));
        EXPECT_LT(now.settled_offset,.005);EXPECT_LT(now.max_offset,.02);EXPECT_LE(now.reversals,2);
    }
}

TEST(LateralSim,SCurve) {
    for(const double speed:{15.0,25.0}) {
        const Scenario s{"S-curve R300 "+speed_name(speed),PathBuilder{}.add(100,0).add(250,1/300.0).add(250,-1/300.0).add(speed*60,0).points,speed,0,(600+speed*30)/speed,0};
        const auto [old,now]=compare(s);
        EXPECT_LT(now.max_offset,.03);EXPECT_LE(now.reversals,4);
    }
}

TEST(LateralSim,PlantUncertainty) {
    // The rate and self-centering at speed are not measured yet: the loop must stay
    // stable for a rate -30%/+40% off and a 0.5/s centering pull. Uncompensated
    // centering leaves a P-loop offset; the centering parameter removes it.
    for(const Plant plant:{Plant{.45,0},Plant{.9,0},Plant{.65,.5}}) {
        auto s=arc(400,25);s.plant=plant;
        char name[64];std::snprintf(name,sizeof name,"R400 25 m/s rate %.2f centering %.1f",plant.rate,plant.centering);s.name=name;
        const auto [old,now]=compare(s);
        EXPECT_LT(now.settled_offset,plant.centering>0?.08:.005);EXPECT_LT(now.max_offset,.08);EXPECT_LE(now.reversals,2);
    }
    auto s=arc(400,25);s.plant={.65,.5};s.name="R400 25 m/s centering 0.5 compensated";
    const auto [old,now]=compare(s,current({.65,10,0,.5}));
    EXPECT_LT(now.settled_offset,.005);
}

TEST(LateralSim,RecordedLane) {
    const std::filesystem::path file=ETS2_LANE_FILE;
    if(!std::filesystem::exists(file)) GTEST_SKIP()<<"No recorded lane at "<<file;
    LanePath lane(file.string());Points points;for(const auto& p:lane.points) points.push_back({p.x,p.y});
    for(const double speed:{15.0,25.0}) {
        const Scenario s{"recorded lane "+speed_name(speed),points,speed,0,(lane.points.back().s-30)/speed,0};
        const auto [old,now]=compare(s);
        EXPECT_LT(now.max_offset,.03);EXPECT_LE(now.reversals,3);
    }
}
