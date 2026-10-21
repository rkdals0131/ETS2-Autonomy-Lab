// Offline closed loop: kinematic bicycle + measured relative-steering plant
// (2026-10-20, Scania S without trailer: d(applied)/dt = a(v)*u - c(v)*applied,
// one game frame delay, 60 Hz game) and the 50 Hz controller. The plant table is
// an independent copy of the measurement, not VirtualEps::plant, so the test
// checks the controller against the plant. The baseline (pure pursuit +
// applied-steering PI) runs in the same loop for comparison only; its metrics are
// printed, not asserted.
#include "control/lateral_controller.hpp"
#include "control/virtual_eps.hpp"
#include "control/kinematic_axles.hpp"
#include "control/lcc_mpc.hpp"
#include "control/hud_path.hpp"
#include "control/hud_marks.hpp"
#include "control/lane_merge.hpp"
#include "control/curve_speed.hpp"
#include <gtest/gtest.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <numbers>
#include <string>
#include <vector>

namespace {
// Wheelbase from the SDK wheel positions; wheel angle per unit at 20-80 km/h.
constexpr double wheelbase=3.734,angle_per_unit=.70,game_dt=1.0/60,control_dt=.02;

// Measured steering plant: rate a (/s) and self-centering c (/s) at 0..96.8 km/h, held beyond.
std::array<double,2> measured_plant(double speed) {
    constexpr double kmh[]{0,20,40,60,80,88.2,96.8},a[]{.650,.570,.489,.401,.320,.284,.261},c[]{0,.939,1.624,1.995,2.108,2.084,2.093};
    const auto x=std::min(std::abs(speed)*3.6,96.8);
    int i=0;while(i<5 && x>kmh[i+1]) ++i;
    const auto f=(x-kmh[i])/(kmh[i+1]-kmh[i]);
    return {a[i]+f*(a[i+1]-a[i]),c[i]+f*(c[i+1]-c[i])};
}

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

struct Plant {double rate_scale=1,centering_scale=1,max=.94;}; // scales of the measured a(v), c(v)
// delay_frames: game frames from the controller tick to the frame the command acts in (1: the
// next frame); measure_age_frames: frames the applied steering the controller reads is old.
struct Scenario {std::string name;Points points;double speed,offset=0,duration=0,settle_s=0;Plant plant{};int delay_frames=1,measure_age_frames=0;double lane_fit_half_m=8;};
struct Metrics {double max_offset=0,settled_offset=0,rms=0,overshoot=0,max_applied=0;int reversals=0;double input_variation=0;
                double yaw_rate_jitter=0; // settled RMS of yaw rate minus its 1 s moving average, rad/s
                double max_lateral_acceleration=0; // |yaw rate| * speed, m/s^2
                int extensions=0;std::vector<double> offsets;}; // offsets: lane offset per controller tick

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
// compensate_centering=false removes the c(v)*applied/a(v) term again (exact while
// the command is not saturated), to show what the centering compensation buys.
Controller current(VirtualEps::Settings eps_settings={},LateralController::Settings settings={},bool compensate_centering=true) {
    return [eps=VirtualEps{},eps_settings,settings,compensate_centering](LanePath& lane,double x,double y,double yaw,double v,double applied) mutable {
        const auto out=LateralController::update(lane,x,y,yaw,v,wheelbase,settings);
        const auto target=std::clamp(out.wheel_angle/angle_per_unit,-1.0,1.0);
        const auto u=eps.command(target,out.wheel_rate/angle_per_unit,applied,v,control_dt,eps_settings);
        if(compensate_centering) return u;
        const auto [a,c]=VirtualEps::plant(v);
        return std::clamp(u-c*applied/a,-1.0,1.0);
    };
}

// The generated MPC as drive_speed wires it: lane errors and curvature preview from the
// LanePath, measured wheel angle from the applied steering, VirtualEps for the input. No
// trailer: the plant here is the measured truck-alone steering. soft_engage: the node's
// LaneMerge from the first pose (its errors and curvature against the merge reference).
Controller lateral_mpc(bool soft_engage=false) {
    return [mpc=std::make_shared<LccMpc>(),eps=VirtualEps{},soft_engage,merge=std::optional<LaneMerge>{},first=true]
           (LanePath& lane,double x,double y,double yaw,double v,double applied) mutable {
        const auto goal=LateralController::update(lane,x,y,yaw,v,wheelbase,{});
        if(first && soft_engage) merge=LaneMerge::start(goal.s,goal.offset,goal.heading_error,v);
        first=false;
        if(merge && merge->done(goal.s)) merge.reset();
        const auto ref=merge?merge->at(goal.s):LaneMerge::Reference{0,0,0};
        LccMpc::Input in;in.enable=true;in.speed_mps=v;in.lateral_error_m=goal.offset-ref.offset;in.heading_error_rad=goal.heading_error-ref.heading;
        for(int i=0;i<LccMpc::preview_points;++i) {
            const auto s=goal.s+i*LccMpc::preview_step_m;
            in.curvature_preview[i]=lane.curvature(s,6)+(merge?merge->at(s).curvature:0);
        }
        in.wheel_angle_rad=angle_per_unit*applied;in.wheelbase_m=wheelbase;
        const auto& out=mpc->update(in);
        EXPECT_NE(out.status,LccMpc::BadInput);
        return eps.command(std::clamp(out.wheel_angle_rad/angle_per_unit,-1.0,1.0),out.wheel_rate_rad_s/angle_per_unit,applied,v,control_dt,{});
    };
}
// rolling: like map_lane_provider --follow, the controller's lane starts with the first
// 1000 m and is extended to 1000 m ahead of the vehicle whenever less than 600 m remain.
Metrics simulate(const Scenario& scenario,Controller controller,bool rolling=false) {
    LanePath truth(scenario.points,3.5);
    const auto window=[&](double from,double to) {
        Points xy;for(const auto& p:truth.points) if(p.s>from && p.s<=to) xy.push_back({p.x,p.y});
        return xy;
    };
    LanePath lane=rolling?LanePath(window(-1,1000),3.5):truth;
    lane.fit_half_m=scenario.lane_fit_half_m;
    const auto heading=lane.heading(0);const auto start=lane.at(0);
    double x=start.x-std::sin(heading)*scenario.offset,y=start.y+std::cos(heading)*scenario.offset,yaw=heading;
    double applied=std::clamp(std::atan(wheelbase*lane.curvature(0,5))/angle_per_unit,-1.0,1.0);
    double command=0,next_control=.003; // controller phase vs game frames
    std::vector<double> pipeline(std::max(1,scenario.delay_frames),0.0),history(scenario.measure_age_frames+1,applied),yaw_rates;
    Metrics m;double sum2=0,last_u=0,extreme=applied;int direction=0,samples=0;
    const auto [rate,centering]=measured_plant(scenario.speed);
    const auto frames=int(scenario.duration/game_dt);
    for(int n=0;n<frames;++n) {
        const auto t=n*game_dt;
        // Controller ticks in (t_{n-1}, t_n] see the state after frame n-1.
        for(;next_control<=t;next_control+=control_dt) {
            if(rolling && lane.remaining()<600 && lane.extend(window(lane.points.back().s,lane.progress+1000))>0) ++m.extensions;
            command=controller(lane,x,y,yaw,scenario.speed,history.front());
            m.input_variation+=std::abs(command-last_u);last_u=command;
            const auto p=truth.track(x,y);
            m.offsets.push_back(p.offset);
            m.max_offset=std::max(m.max_offset,std::abs(p.offset));
            if(next_control>=scenario.settle_s) {m.settled_offset=std::max(m.settled_offset,std::abs(p.offset));sum2+=p.offset*p.offset;++samples;}
            if(scenario.offset>0) m.overshoot=std::max(m.overshoot,-p.offset);
        }
        // The command latched delay_frames frame boundaries ago acts over this frame.
        const auto active=pipeline.front();pipeline.erase(pipeline.begin());pipeline.push_back(command);
        applied=std::clamp(applied+game_dt*(scenario.plant.rate_scale*rate*active-scenario.plant.centering_scale*centering*applied),-scenario.plant.max,scenario.plant.max);
        history.erase(history.begin());history.push_back(applied);
        const auto angle=angle_per_unit*applied;
        // Midpoint heading: explicit Euler would bias heading against travel direction.
        const auto turn=scenario.speed*std::tan(angle)/wheelbase*game_dt;
        x+=scenario.speed*std::cos(yaw+turn/2)*game_dt;y+=scenario.speed*std::sin(yaw+turn/2)*game_dt;yaw+=turn;
        if(t>=scenario.settle_s) yaw_rates.push_back(turn/game_dt);
        m.max_lateral_acceleration=std::max(m.max_lateral_acceleration,std::abs(turn/game_dt*scenario.speed));
        m.max_applied=std::max(m.max_applied,std::abs(applied));
        // Steering direction reversals larger than 0.002 units (hysteresis).
        if(direction>=0 && applied<extreme-.002) {if(direction>0) ++m.reversals;direction=-1;extreme=applied;}
        else if(direction<=0 && applied>extreme+.002) {if(direction<0) ++m.reversals;direction=1;extreme=applied;}
        else if(direction>0) extreme=std::max(extreme,applied);
        else if(direction<0) extreme=std::min(extreme,applied);
    }
    m.rms=samples?std::sqrt(sum2/samples):0;m.input_variation/=scenario.duration;
    double jitter2=0;int count=0;
    for(size_t i=30;i+30<yaw_rates.size();++i) {
        double mean=0;for(size_t j=i-30;j<i+30;++j) mean+=yaw_rates[j];
        jitter2+=std::pow(yaw_rates[i]-mean/60,2);++count;
    }
    m.yaw_rate_jitter=count?std::sqrt(jitter2/count):0;
    return m;
}

void print(const char* who,const Scenario& s,const Metrics& m) {
    std::printf("| %-34s | %-4s | %.3f | %.3f | %.4f | %.3f | %3d | %.3f | %.3f | %.4f |\n",
                s.name.c_str(),who,m.max_offset,m.settled_offset,m.rms,m.overshoot,m.reversals,m.input_variation,m.max_applied,m.yaw_rate_jitter);
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

TEST(LanePath,ExtendMatchesWholePath) {
    const auto all=PathBuilder{}.add(50,0).add(600,1/300.0).add(400,-1/700.0).add(200,0).points;
    const LanePath whole(all,3.5);
    LanePath rolling(Points(all.begin(),all.begin()+300),3.5);
    for(double s=0;s<400;s+=.5) {const auto p=rolling.at(s);rolling.track(p.x,p.y);}
    const auto progress=rolling.progress;
    // The joint point repeats, as a provider revision restarts at a published point.
    EXPECT_EQ(rolling.extend(Points(all.begin()+299,all.begin()+600)),300u);
    EXPECT_EQ(rolling.extend(Points(all.begin()+600,all.end())),all.size()-600);
    ASSERT_EQ(rolling.points.size(),whole.points.size());
    EXPECT_EQ(rolling.progress,progress);
    for(size_t i=0;i<whole.points.size();++i) {EXPECT_EQ(rolling.points[i].s,whole.points[i].s);EXPECT_EQ(rolling.points[i].x,whole.points[i].x);}
    for(size_t i=0;i<whole.headings.size();++i) EXPECT_EQ(rolling.headings[i],whole.headings[i]);
    for(double s=0;s<whole.points.back().s;s+=3.7) EXPECT_EQ(rolling.curvature(s,6),whole.curvature(s,6));
}

TEST(LanePath,ExtendFromProviderRevision) {
    // Two revisions of a rolling provider file: the second drops points behind the
    // vehicle (start_s > 0) and appends ahead.
    const auto all=PathBuilder{}.add(100,0).add(900,1/400.0).add(500,0).points;
    const LanePath whole(all,3.5);
    ASSERT_EQ(whole.points.size(),1000u);
    const auto dir=std::filesystem::temp_directory_path()/"ets2_lane_extend_test";
    std::filesystem::create_directories(dir);
    const auto write=[&](const std::string& name,size_t from,size_t to,long long revision,long long path_id,double shift=0) {
        nlohmann::json data{{"frame_id","world"},{"lane_width_m",4.5},{"end_reason","distance_limit"},{"revision",revision},{"path_id",path_id},{"start_s",whole.points[from].s}};
        for(auto i=from;i<to;++i) data["points"].push_back({whole.points[i].x,whole.points[i].y+shift});
        std::ofstream((dir/name).string())<<data.dump();
        return (dir/name).string();
    };
    LanePath lane(write("r1.json",0,700,1,3));
    EXPECT_EQ(lane.revision,1);EXPECT_EQ(lane.path_id,3);EXPECT_EQ(lane.start_s,0);
    for(double s=0;s<300;s+=.5) {const auto p=lane.at(s);lane.track(p.x,p.y);}
    EXPECT_EQ(lane.extend(write("r1.json",0,700,1,3)),0u);   // same revision: nothing to do
    EXPECT_EQ(lane.extend(write("r2.json",150,900,2,3)),200u);
    EXPECT_EQ(lane.revision,2);
    // A newer file that is not this path's continuation: another path, or no point
    // coinciding with this path's end (a reset that kept the path_id).
    EXPECT_THROW(lane.extend(write("other.json",600,1000,3,4)),std::runtime_error);
    EXPECT_THROW(lane.extend(write("moved.json",600,1000,3,3,1)),std::runtime_error);
    EXPECT_EQ(lane.revision,2);
    EXPECT_EQ(lane.extend(write("r3.json",800,1000,3,3)),100u);
    ASSERT_EQ(lane.points.size(),whole.points.size());
    for(size_t i=0;i<lane.points.size();++i) {EXPECT_NEAR(lane.points[i].s,whole.points[i].s,1e-9);EXPECT_EQ(lane.points[i].y,whole.points[i].y);}
    EXPECT_NEAR(lane.progress,299.5,1e-6);
    std::filesystem::remove_all(dir);
}

TEST(VirtualEps,TracksRampWithoutLagAndHolds) {
    VirtualEps eps;double applied=0;
    for(int k=0;k<100;++k) {const auto target=.2*k*control_dt;applied+=.65*control_dt*eps.command(target,.2,applied,0,control_dt,{});}
    EXPECT_NEAR(applied,.2*100*control_dt,1e-9); // exact for the undelayed ideal integrator
    VirtualEps limited;const VirtualEps::Settings slow{6.5,.3};
    const auto first=limited.command(1,0,0,0,control_dt,slow);
    EXPECT_NEAR(first,.3/.65+10*.3*control_dt,1e-12); // target slews from applied, not jumps
    EXPECT_EQ(VirtualEps{}.command(.5,0,.5,0,control_dt,{}),0);
}

TEST(VirtualEps,PlantTableMatchesMeasurement) {
    for(const double v:{0.0,3.0,20/3.6,8.0,40/3.6,15.0,60/3.6,20.0,80/3.6,23.5,88.2/3.6,25.0,96.8/3.6,30.0,-10.0}) {
        const auto p=VirtualEps::plant(v);const auto [a,c]=measured_plant(v);
        EXPECT_NEAR(p.rate,a,1e-12);EXPECT_NEAR(p.centering,c,1e-12);
    }
}

TEST(VirtualEps,BandwidthIndependentOfSpeed) {
    // On the undelayed measured plant the error decays by (1 - bandwidth*dt) per cycle at any speed.
    for(const double v:{0.0,40/3.6,80/3.6}) {
        VirtualEps eps;double applied=.05;const auto [a,c]=measured_plant(v);
        for(int k=0;k<10;++k) applied+=control_dt*(a*eps.command(.06,0,applied,v,control_dt,{})-c*applied);
        EXPECT_NEAR(.06-applied,.01*std::pow(1-6.5*control_dt,10),1e-12);
    }
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

TEST(LateralSim,RollingLaneSplicesWithoutTransient) {
    // The controller lane grows by provider-style revisions while the vehicle drives
    // through curves; the trajectory must equal the one on the whole lane.
    for(const double speed:{15.0,25.0}) {
        const Scenario s{"rolling lane S-curves "+speed_name(speed),PathBuilder{}.add(100,0).add(500,1/400.0).add(400,-1/300.0).add(300,0).add(600,1/500.0).add(speed*60,0).points,
                         speed,.5,(1900+speed*40)/speed,20};
        const auto whole=simulate(s,current()),rolling=simulate(s,current(),true);
        print("new",s,rolling);
        ASSERT_EQ(whole.offsets.size(),rolling.offsets.size());
        double difference=0;
        for(size_t i=0;i<whole.offsets.size();++i) difference=std::max(difference,std::abs(whole.offsets[i]-rolling.offsets[i]));
        EXPECT_GE(rolling.extensions,2);EXPECT_LT(difference,1e-9);
        EXPECT_LT(rolling.settled_offset,.03);EXPECT_LE(rolling.reversals,8); // 6: initial offset and four curve changes
    }
}

TEST(LateralSim,CenteringCompensation) {
    // At 80 km/h the centering is near its measured maximum (c 2.1/s): the inverse removes the P-loop
    // steering deficit that the self-centering otherwise leaves in a steady turn.
    auto s=arc(400,80/3.6);s.name="arc R400 80 km/h";
    const auto [old,now]=compare(s);
    s.name="arc R400 80 km/h no centering comp";
    const auto uncompensated=simulate(s,current({},{},false));print("new",s,uncompensated);
    EXPECT_LT(now.settled_offset,.005);EXPECT_LT(now.max_offset,.02);EXPECT_LE(now.reversals,2);
    EXPECT_GT(uncompensated.settled_offset,.1); // about 0.19 m, printed above
}

TEST(LateralSim,PlantUncertainty) {
    // The table is one truck without trailer: the loop must stay stable for a rate
    // -30%/+40% off and a centering 0x/1.5x the measured value. Without an integral
    // term any a or c mismatch leaves a steady steering error
    // (c_true*a_model/a_true - c_model)*applied/bandwidth, a 0.06-0.22 m offset here.
    for(const Plant plant:{Plant{.7,1},Plant{1.4,1},Plant{1,0},Plant{1,1.5}}) {
        auto s=arc(400,25);s.plant=plant;
        char name[64];std::snprintf(name,sizeof name,"R400 25 m/s rate x%.1f centering x%.1f",plant.rate_scale,plant.centering_scale);s.name=name;
        const auto [old,now]=compare(s);
        EXPECT_LT(now.max_offset,.25);EXPECT_LE(now.reversals,2);
    }
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

// Map lanes are float32 points (2-4 mm steps 30 km from the origin). With the game's
// steering delay (command to effect 2-3 frames, the applied value one frame old, measured
// 2026-10-21) the MPC on raw segment headings shook the truck at 3 Hz (game: yaw rate
// jitter 0.021 rad/s, 1016 reversals in 150 s here); on the smoothed lane it does not.
Points float32_far(Points points) {
    for(auto& [x,y]:points) {x=double(float(x-30915.24));y=double(float(y-28469.24));}
    return points;
}
TEST(LateralSim,Float32MapLane) {
    const auto lane=float32_far(PathBuilder{}.add(150,0).add(600,1/400.0).add(500,-1/300.0).add(900,0).add(700,1/700.0).add(400,0).points);
    for(const double speed:{60/3.6,87/3.6}) {
        Scenario s{"float32 lane, game delay "+speed_name(speed),lane,speed,0,std::min(140.0,3000/speed),10};
        s.delay_frames=3;s.measure_age_frames=1;
        const auto smoothed=simulate(s,lateral_mpc());print("new",s,smoothed);
        s.name="  same, unsmoothed lane";s.lane_fit_half_m=.5; // fewer than 3 points: segment headings
        const auto raw=simulate(s,lateral_mpc());print("raw",s,raw);
        // The game-delay MPC (2026-10-21 evening) trades a few cm on curvature steps for less
        // sway, and no longer shakes even on the unsmoothed lane.
        EXPECT_LT(smoothed.yaw_rate_jitter,.006);EXPECT_LT(smoothed.settled_offset,.08);
        EXPECT_LT(raw.yaw_rate_jitter,.006);
    }
}

// Engaging 1 m off the centre at 80 km/h: the merge reference keeps the lateral acceleration
// near its 0.5 m/s^2 design instead of the MPC's own correction.
TEST(LateralSim,SoftEngagement) {
    const auto speed=80/3.6;
    for(const auto [offset,name]:{std::pair{1.0,"1 m"},std::pair{2.0,"2 m (after a lane change)"}}) {
        Scenario s{std::string("engage ")+name+" off, 80 km/h",PathBuilder{}.add(speed*30+200,0).points,speed,offset,30,25};
        const auto hard=simulate(s,lateral_mpc()),soft=simulate(s,lateral_mpc(true));
        print("mpc",s,hard);print("soft",s,soft);
        std::printf("  peak lateral acceleration %.2f -> %.2f m/s^2\n",hard.max_lateral_acceleration,soft.max_lateral_acceleration);
        EXPECT_LT(soft.max_lateral_acceleration,.8);EXPECT_LT(soft.max_lateral_acceleration,hard.max_lateral_acceleration);
        EXPECT_LT(soft.overshoot,.05);EXPECT_LT(soft.settled_offset,.02);
    }
}

TEST(LaneMerge,ReferenceShape) {
    const auto merge=LaneMerge::start(100,2.0,.03,25);
    const auto a=merge.at(100);EXPECT_NEAR(a.offset,2.0,1e-12);EXPECT_NEAR(a.heading,.03,1e-12);
    const auto b=merge.at(100+merge.length);EXPECT_EQ(b.offset,0);EXPECT_EQ(b.heading,0);
    const auto peak=[](const LaneMerge& m,double v) {
        double result=0;for(double s=m.s0;s<m.s0+m.length;s+=.25) result=std::max(result,std::abs(m.at(s).curvature)*v*v);
        return result;
    };
    // Quintic: no step of curvature at either end.
    EXPECT_NEAR(merge.at(100).curvature,0,1e-9);EXPECT_NEAR(merge.at(100+merge.length-1e-6).curvature,0,1e-6);
    // The offset term reaches its 0.5 m/s^2; the heading term may add up to as much again.
    EXPECT_NEAR(peak(LaneMerge::start(0,2,0,25),25),.5,.01);
    EXPECT_LE(peak(merge,25),1.0);
    EXPECT_GE(LaneMerge::start(0,0,0,25).length,50.0); // at least 2 s of driving
    // A turn signal lane change one 4.5 m lane over at 90 km/h: about 5.4 s at 0.9 m/s^2.
    const auto change=LaneMerge::start(0,-4.5,0,25,.9,4);
    EXPECT_NEAR(change.length/25,5.4,.1);EXPECT_NEAR(peak(change,25),.9,.02);
}

TEST(CurveSpeed,PlansTheDecelerationAhead) {
    // R130 from 300 m ahead: sqrt(a*R + 2*d*distance) = sqrt(2*130 + 2*0.45*300) = 23.0 m/s.
    LanePath lane(PathBuilder{}.add(300,0).add(300,1/130.0).points,3.5);
    EXPECT_NEAR(curve_speed(lane,0,2,.45),std::sqrt(2*130+2*.45*295),.3);   // 10 m window: the curve shows 5 m early
    EXPECT_NEAR(curve_speed(lane,400,2,.45),std::sqrt(2*130.0),.2);         // in the curve
    LanePath straight(PathBuilder{}.add(800,0).points,3.5);
    EXPECT_TRUE(std::isinf(curve_speed(straight,0,2,.45)));
}

// Exploration on a lane rebuilt from a game drive (map_lane_provider over the recorded poses):
// ETS2_REPLAY_LANE=<lane json> [ETS2_LANE_FIT=<fit half width m>]. Skipped otherwise.
TEST(LateralSim,GameReplayExploration) {
    const auto* file=std::getenv("ETS2_REPLAY_LANE");
    if(!file) GTEST_SKIP()<<"ETS2_REPLAY_LANE not set";
    LanePath lane(file);Points points;for(const auto& p:lane.points) points.push_back({p.x,p.y});
    for(const auto [delay,age]:{std::pair{1,0},std::pair{3,1}}) for(const double speed:{60/3.6,87/3.6}) {
        Scenario s{"replay d"+std::to_string(delay)+"a"+std::to_string(age)+" "+speed_name(speed),points,speed,0,std::min(150.0,(lane.points.back().s-80)/speed),10};
        s.delay_frames=delay;s.measure_age_frames=age;
        if(const auto* fit=std::getenv("ETS2_LANE_FIT")) s.lane_fit_half_m=std::atof(fit);
        compare(s,lateral_mpc());
    }
}

// Scania S 6x2 tag lift, SDK wheel positions in base_link (drive axle): front axle
// 3.158 m ahead, tag axle 1.295 m behind (docs/29).
// The MPC on the same measured steering plant: offsets, a constant radius and an S-curve.
TEST(LateralSim,ModelPredictive) {
    for(const double speed:{60/3.6,80/3.6,100/3.6}) {
        const auto [old,now]=compare({"MPC straight 1 m offset "+speed_name(speed),PathBuilder{}.add(speed*40+200,0).points,speed,1,40,20},lateral_mpc());
        EXPECT_LT(now.overshoot,.08);EXPECT_LT(now.settled_offset,.01);
    }
    for(const double speed:{60/3.6,80/3.6}) {
        const auto [old,now]=compare(arc(400,speed),lateral_mpc());
        EXPECT_LT(now.settled_offset,.01);EXPECT_LT(now.max_offset,.03);
    }
    const auto speed=80/3.6;
    const auto [old,now]=compare({"MPC S-curve R300 "+speed_name(speed),PathBuilder{}.add(100,0).add(250,1/300.0).add(250,-1/300.0).add(speed*60,0).points,speed,0,(600+speed*30)/speed,0},lateral_mpc());
    EXPECT_LT(now.max_offset,.08); // curvature step reversals: cut on purpose by the jerk weight
}

TEST(Hud,ArcLaneAndLeadBar) {
    // Left arc R100 from the origin heading +x: every point 100 m from the centre (0, 100).
    auto arc=hud::arc(0,0,0,1/100.0,50);
    ASSERT_EQ(arc.size(),26u);
    for(const auto& [x,y]:arc) EXPECT_NEAR(std::hypot(x,y-100),100,1e-9);
    EXPECT_NEAR(std::atan2(arc.back()[0],100-arc.back()[1]),.5,1e-9);   // 50 m of arc
    const auto end=arc.back();hud::bar(arc);
    ASSERT_EQ(arc.size(),29u);EXPECT_EQ(arc.back(),end);
    EXPECT_NEAR(std::hypot(arc[26][0]-end[0],arc[26][1]-end[1]),hud::bar_half_m,1e-9);
    // The lane ribbon stops at the path end.
    LanePath lane(PathBuilder{}.add(30,0).points,3.5);
    const auto ribbon=hud::lane(lane,10,100);
    EXPECT_NEAR(ribbon.front()[0],10,1e-9);EXPECT_NEAR(ribbon.back()[0],lane.points.back().s,1e-9);
}

TEST(Hud,MarksForEachState) {
    hud::Composer composer;
    hud::Inputs in;in.ego_yaw=0;in.lane_width=3.5;
    EXPECT_TRUE(composer.update(in,.033).empty());                          // off: nothing
    in.lcc_steering=true;in.plan={{6,0},{30,0},{60,0}};
    auto marks=composer.update(in,.033);
    ASSERT_EQ(marks.size(),1u);EXPECT_DOUBLE_EQ(marks[0].width_m,.2);       // LCC: the thin ribbon
    // ACC without a lead: two brackets at the gap, apart in the middle.
    in.acc=true;in.mark=1;in.mark_x=40;in.mark_heading=0;
    marks=composer.update(in,.033);
    ASSERT_EQ(marks.size(),3u);
    const auto& right=marks[1].points;const auto& left=marks[2].points;          // ROS y: left positive
    EXPECT_NEAR(right.front()[1],-1.5,1e-9);EXPECT_NEAR(left.back()[1],1.5,1e-9);    // lane edges less 0.25 m
    EXPECT_GT(left.front()[1]-right.back()[1],1.5);                                  // an opening in the middle
    // A lead: the brackets slide to it and close into one bar within a quarter second.
    in.mark=2;in.mark_x=30;
    marks=composer.update(in,.1);
    EXPECT_EQ(marks.size(),3u);EXPECT_GT(composer.join(),0);EXPECT_LT(composer.join(),1);
    for(int i=0;i<10;++i) marks=composer.update(in,.033);
    ASSERT_EQ(marks.size(),2u);EXPECT_NEAR(marks[1].points[0][0],30,1e-3);EXPECT_NEAR(marks[1].points[0][1],-1.5,1e-9);
    // Braking: amber, then red; the driver's throttle fades the marks.
    in.accel_target=-1.5;EXPECT_FLOAT_EQ(composer.update(in,.033)[1].rgba[2],hud::amber[2]);
    in.accel_target=-3;EXPECT_FLOAT_EQ(composer.update(in,.033)[1].rgba[1],hud::red[1]);
    in.accel_target=0;in.driver_throttle=true;EXPECT_LT(composer.update(in,.033)[1].rgba[3],.4f);
    in.driver_throttle=false;
    // A lane change to the left waiting for a gap: dashes one lane width left, 8-26 m along the plan.
    in.change_request=-1;
    marks=composer.update(in,.033);
    ASSERT_EQ(marks.size(),5u);
    for(size_t i=2;i<5;++i) {EXPECT_NEAR(marks[i].points.front()[1],3.5,1e-9);EXPECT_NEAR(marks[i].points.back()[0]-marks[i].points.front()[0],2.5,.6);}
    EXPECT_NEAR(marks[2].points.front()[0],14,1e-9);   // 8 m along a plan that starts at x = 6
    // ACC off: its marks go and the animation restarts.
    in.acc=false;in.change_request=0;
    EXPECT_EQ(composer.update(in,.033).size(),1u);EXPECT_EQ(composer.join(),0);
}

TEST(KinematicAxles,TagAxleMovesTheRearPoint) {
    const std::map<uint32_t,AxleWheel> wheels{{0,{3.158,true,false}},{1,{3.158,true,false}},{2,{0,false,false}},
                                              {3,{0,false,false}},{4,{-1.295,false,true}},{5,{-1.295,false,true}}};
    const auto up=kinematic_axles(wheels,{{4,false},{5,false}});
    EXPECT_NEAR(up.rear_x,0,1e-9);EXPECT_NEAR(up.wheelbase(),3.158,1e-9);
    const auto down=kinematic_axles(wheels,{{4,true},{5,true}});
    EXPECT_NEAR(down.wheelbase(),3.68,.005);   // measured effective wheelbase
    EXPECT_NEAR(kinematic_axles(wheels,{}).wheelbase(),3.158,1e-9);   // no state yet: lift axle raised
    const std::map<uint32_t,AxleWheel> four{{0,{3.734,true,false}},{1,{3.734,true,false}},{2,{0,false,false}},{3,{0,false,false}}};
    EXPECT_NEAR(kinematic_axles(four,{}).wheelbase(),3.734,1e-9);
}

// Cornering with the game's steering delay on clothoid curves, curvature steps and the lane
// wiggles of map navigation curves, truck alone and with the box trailer. Delay (bag
// 20261021-163908): a command acts 3 game frames after the frame whose state it used (2-4 seen),
// the applied steering read is from that frame (age 0; an older bag suggested 1). Plant: the
// reference truck of apps/control/matlab/lcc (6x2 tag raised, wheelbase 3.158 m to the MPC),
// yaw from the measured effective wheelbase L(v) and the drive axle sideslip, kinematic trailer
// with hitch offset (lcc_plant_params.m), steering from the measured table above.
namespace {
constexpr std::array<double,7> yaw_kmh{0,25,50,60,80,90,100};
struct YawTable {std::array<double,7> dL,slip;}; // effective wheelbase shift, reference-point sideslip (m)
constexpr YawTable truck_yaw{{0,.008,.026,.029,.038,.040,.046},{0,-.027,-.053,-.065,-.084,-.092,-.098}};
constexpr YawTable trailer_yaw{{0,-.050,-.082,-.097,-.119,-.132,-.141},{0,.042,.065,.070,.084,.086,.090}};
double yaw_table(const std::array<double,7>& y,double kmh) {
    if(kmh>=yaw_kmh.back()) return y.back();
    int i=0;while(kmh>yaw_kmh[i+1]) ++i;
    return y[i]+(kmh-yaw_kmh[i])/(yaw_kmh[i+1]-yaw_kmh[i])*(y[i+1]-y[i]);
}
// Segments {length, curvature at the start, at the end}: curvature linear in s (a clothoid when
// they differ, a step between segments). Points 1.5 m apart like the map lanes; curve_from/to: s
// of the curved part.
struct CurveLane {Points points;double length=0,curve_from=-1,curve_to=0;};
CurveLane curve_lane(std::initializer_list<std::array<double,3>> segments) {
    CurveLane lane;lane.points.push_back({0,0});
    double x=0,y=0,h=0,s=0,next=1.5;constexpr double ds=.01;
    for(const auto& [length,k0,k1]:segments) {
        if(k0!=0 || k1!=0) {if(lane.curve_from<0) lane.curve_from=s;lane.curve_to=s+length;}
        const auto n=int(std::lround(length/ds));
        for(int i=0;i<n;++i) {
            const auto k=k0+(k1-k0)*(i+.5)/n,mid=h+k*ds/2;
            x+=ds*std::cos(mid);y+=ds*std::sin(mid);h+=k*ds;s+=ds;
            if(s>=next-1e-9) {lane.points.push_back({x,y});next+=1.5;}
        }
    }
    lane.length=s;
    return lane;
}
struct CornerRun {std::string name;CurveLane lane;double speed;bool trailer=false;int delay_frames=3,measure_age_frames=0;Plant plant{};};
struct CornerMetrics {
    double peak=0,rms=0,exit_peak=0;  // |offset| in the curve, after it (150 m), m
    double max_rate=0,rms_accel=0;    // wheel angle rate rad/s, its change rad/s^2 (60 Hz)
    double yaw_jitter=0;              // RMS of yaw rate minus its 1 s mean, rad/s
    double sway=0;                    // max |lateral acceleration minus its 2 s mean|, curve +50 m, m/s^2
    double trailer_peak=0;
    int crossings=0; // sign changes of the offset beyond +-5 mm from the curve start to 150 m after its end
};
CornerMetrics corner_sim(const CornerRun& r) {
    LanePath truth(r.lane.points,3.5),lane=truth;
    const auto v=r.speed,kmh=v*3.6,wheelbase_mpc=3.158;
    const auto& yt=r.trailer?trailer_yaw:truck_yaw;
    const auto L=wheelbase_mpc+yaw_table(yt.dL,kmh),slip=yaw_table(yt.slip,kmh);
    constexpr double hitch_offset=.478,trailer_length=7.851;
    const auto [rate,centering]=measured_plant(v);
    LccMpc mpc;VirtualEps eps;
    // Start on the lane at its start, steering its curvature (a replay lane may start in a curve).
    const auto start=truth.at(0);
    double x=start.x,y=start.y,yaw=truth.heading(0),hitch=0,last_rate=0,sum2=0,accel2=0,s=0,next_control=.003;
    double applied=std::clamp(std::atan(L*truth.curvature(3,6))/angle_per_unit,-.9,.9),command=centering*applied/rate;
    std::vector<double> pipeline(std::max(1,r.delay_frames),command),history(r.measure_age_frames+1,applied),yaw_rates,lateral,lane_s;
    CornerMetrics m;int samples=0,frames_counted=0,side=0;
    const auto frames=int((r.lane.length-60)/v/game_dt);
    // ETS2_CORNER_CSV=<folder>: per controller tick t, s, offset, heading error, applied, MPC angle and rate, input.
    std::ofstream csv;
    if(const auto* dir=std::getenv("ETS2_CORNER_CSV")) {
        auto name=r.name+(r.trailer?"_trailer":"_truck")+"_d"+std::to_string(r.delay_frames)+"a"+std::to_string(r.measure_age_frames)+".csv";
        for(auto& c:name) if(!std::isalnum(static_cast<unsigned char>(c)) && c!='_' && c!='.') c='_';
        csv.open((std::filesystem::path(dir)/name).string());csv<<"t,s,offset,heading_error,applied,mpc_angle,mpc_rate,command,hitch\n";
    }
    for(int n=0;n<frames;++n) {
        const auto t=n*game_dt;
        for(;next_control<=t;next_control+=control_dt) {
            const auto goal=LateralController::update(lane,x,y,yaw,v,wheelbase_mpc,{});
            LccMpc::Input in;in.enable=true;in.speed_mps=v;in.lateral_error_m=goal.offset;in.heading_error_rad=goal.heading_error;
            for(int i=0;i<LccMpc::preview_points;++i) in.curvature_preview[i]=lane.curvature(goal.s+i*LccMpc::preview_step_m,6);
            in.wheel_angle_rad=angle_per_unit*history.front();in.wheelbase_m=wheelbase_mpc;
            in.trailer_connected=r.trailer;in.hitch_angle_rad=hitch;
            const auto& out=mpc.update(in);
            EXPECT_EQ(out.status,LccMpc::Mpc);
            command=eps.command(std::clamp(out.wheel_angle_rad/angle_per_unit,-1.0,1.0),out.wheel_rate_rad_s/angle_per_unit,history.front(),v,control_dt,{});
            const auto p=truth.track(x,y);s=p.s;
            if(csv) csv<<next_control<<','<<p.s<<','<<p.offset<<','<<goal.heading_error<<','<<applied<<','<<out.wheel_angle_rad<<','<<out.wheel_rate_rad_s<<','<<command<<','<<hitch<<'\n';
            if(p.s>=r.lane.curve_from && p.s<=r.lane.curve_to) {m.peak=std::max(m.peak,std::abs(p.offset));sum2+=p.offset*p.offset;++samples;}
            if(p.s>r.lane.curve_to && p.s<=r.lane.curve_to+150) m.exit_peak=std::max(m.exit_peak,std::abs(p.offset));
            if(p.s>=r.lane.curve_from && p.s<=r.lane.curve_to+150) {
                if((p.offset>.005 && side<0) || (p.offset<-.005 && side>0)) ++m.crossings;
                if(std::abs(p.offset)>.005) side=p.offset>0?1:-1;
            }
            if(r.trailer && p.s>30) { // effective trailer axle against the lane
                const auto hx=x-hitch_offset*std::cos(yaw),hy=y-hitch_offset*std::sin(yaw),psi_t=yaw-hitch;
                const auto q=truth.project(hx-trailer_length*std::cos(psi_t),hy-trailer_length*std::sin(psi_t),p.s-20,p.s);
                m.trailer_peak=std::max(m.trailer_peak,std::abs(q.offset));
            }
        }
        const auto active=pipeline.front();pipeline.erase(pipeline.begin());pipeline.push_back(command);
        const auto before=applied;
        applied=std::clamp(applied+game_dt*(r.plant.rate_scale*rate*active-r.plant.centering_scale*centering*applied),-.94,.94);
        history.erase(history.begin());history.push_back(applied);
        const auto delta=angle_per_unit*applied,wheel_rate=angle_per_unit*(applied-before)/game_dt;
        const auto yaw_rate=v*std::tan(delta)/L,side_speed=-slip*yaw_rate,mid=yaw+yaw_rate*game_dt/2;
        x+=(v*std::cos(mid)-side_speed*std::sin(mid))*game_dt;y+=(v*std::sin(mid)+side_speed*std::cos(mid))*game_dt;yaw+=yaw_rate*game_dt;
        hitch+=(yaw_rate-(v*std::sin(hitch)+(side_speed-hitch_offset*yaw_rate)*std::cos(hitch))/trailer_length)*game_dt;
        if(t>5) {
            m.max_rate=std::max(m.max_rate,std::abs(wheel_rate));accel2+=std::pow((wheel_rate-last_rate)/game_dt,2);++frames_counted;
            yaw_rates.push_back(yaw_rate);lateral.push_back(yaw_rate*v);lane_s.push_back(s);
        }
        last_rate=wheel_rate;
    }
    m.rms=samples?std::sqrt(sum2/samples):0;m.rms_accel=frames_counted?std::sqrt(accel2/frames_counted):0;
    double jitter2=0;int count=0;
    for(size_t i=30;i+30<yaw_rates.size();++i) {
        double mean=0;for(size_t j=i-30;j<i+30;++j) mean+=yaw_rates[j];
        jitter2+=std::pow(yaw_rates[i]-mean/60,2);++count;
    }
    m.yaw_jitter=count?std::sqrt(jitter2/count):0;
    for(size_t i=60;i+60<lateral.size();++i) if(lane_s[i]>=r.lane.curve_from && lane_s[i]<=r.lane.curve_to+50) {
        double mean=0;for(size_t j=i-60;j<i+60;++j) mean+=lateral[j];
        m.sway=std::max(m.sway,std::abs(lateral[i]-mean/120));
    }
    return m;
}
void print_corner(const CornerRun& r,const CornerMetrics& m) {
    std::printf("| %-28s | %-7s | d%da%d | %6.1f | %5.1f | %6.1f | %2d | %.3f | %6.3f | %.4f | %.2f | %4.0f |\n",r.name.c_str(),r.trailer?"trailer":"truck",
                r.delay_frames,r.measure_age_frames,1000*m.peak,1000*m.rms,1000*m.exit_peak,m.crossings,m.max_rate,m.rms_accel,m.yaw_jitter,m.sway,1000*m.trailer_peak);
}
constexpr double kmh(double k) {return k/3.6;}
CurveLane arc_lane(double radius,double clothoid,double arc_length) {
    const auto k=1/radius;
    return curve_lane({{150,0,0},{clothoid,0,k},{arc_length,k,k},{clothoid,k,0},{300,0,0}});
}
// Curves at about their design speed (lateral acceleration 1.6-2 m/s^2): clothoids as on
// motorways, steps (the worst case), the game's road joins (curvature steps of 2-4 /km) and the
// S-wiggle of a navigation curve through a lane-drop prefab (bag 20261021-163908, mod_ger_35:
// curvature ramps +2.7 -> -4.0 /km over 45 m, steps to +4.1, ramps to -3.6, steps to 0).
std::vector<CornerRun> corner_runs() {
    return {
        {"R1000 clothoid 60 m, 90",arc_lane(1000,60,300),kmh(90)},
        {"R400 clothoid 80 m, 90",arc_lane(400,80,300),kmh(90)},
        {"R400 step, 90",arc_lane(400,0,300),kmh(90)},
        {"R250 clothoid 60 m, 80",arc_lane(250,60,250),kmh(80)},
        {"R130 clothoid 40 m, 58",arc_lane(130,40,150),kmh(58)},
        {"R130 step, 58",arc_lane(130,0,150),kmh(58)},
        {"S R400 clothoids, 90",curve_lane({{150,0,0},{80,0,1/400.0},{150,1/400.0,1/400.0},{160,1/400.0,-1/400.0},{150,-1/400.0,-1/400.0},{80,-1/400.0,0},{300,0,0}}),kmh(90)},
        {"road joins 3 /km steps, 86",curve_lane({{150,0,0},{100,.003,.003},{100,.0033,.0033},{100,.003,.003},{300,0,0}}),kmh(86)},
        {"nav-curve wiggle, 82",curve_lane({{150,0,0},{45,.0027,-.004},{45,.0041,-.0036},{300,0,0}}),kmh(82)},
    };
}
} // namespace

TEST(LateralSim,CorneringWithGameDelay) {
    std::printf("| lane | vehicle | delay | peak mm | rms mm | exit mm | x | max rate | rms accel | yaw jitter | sway m/s2 | trailer axle mm |\n");
    for(const bool trailer:{false,true}) for(const auto [delay,age]:{std::pair{3,0},std::pair{3,1},std::pair{2,0},std::pair{4,0}}) for(auto r:corner_runs()) {
        r.trailer=trailer;r.delay_frames=delay;r.measure_age_frames=age;
        const auto m=corner_sim(r);
        print_corner(r,m);
        EXPECT_LT(m.yaw_jitter,.006)<<r.name;   // no limit cycle (the 1-frame design shook at 0.03-0.05 rad/s at 90 km/h)
        EXPECT_LT(m.max_rate,.1)<<r.name;
        if(r.name.starts_with("R130")) {EXPECT_LT(m.peak,.1)<<r.name;EXPECT_LT(m.exit_peak,.1)<<r.name;}
        else if(!r.name.starts_with("nav")) {EXPECT_LT(m.peak,.06)<<r.name;EXPECT_LT(m.exit_peak,.06)<<r.name;}
    }
}

// The replay lane of a game drive (ETS2_REPLAY_LANE, e.g. the mod_ger_35 wiggle), truck and trailer.
TEST(LateralSim,CorneringReplayLane) {
    const auto* file=std::getenv("ETS2_REPLAY_LANE");
    if(!file) GTEST_SKIP()<<"ETS2_REPLAY_LANE not set";
    LanePath lane(file);CurveLane replay;
    for(const auto& p:lane.points) replay.points.push_back({p.x,p.y});
    replay.length=lane.points.back().s;replay.curve_from=0;replay.curve_to=replay.length;
    for(const bool trailer:{false,true}) for(const double speed:{kmh(82),kmh(90)}) {
        CornerRun r{"replay "+std::to_string(int(speed*3.6+.5))+" km/h",replay,speed,trailer};
        print_corner(r,corner_sim(r));
    }
}

// The MPC sends the steering input through the VirtualEps table: the rack off that table (rate
// -30 %/+40 %, self-centering 0.5x/1.5x, as PlantUncertainty) must stay stable and in the lane.
TEST(LateralSim,CorneringRackMismatch) {
    for(const Plant plant:{Plant{.7,1},Plant{1.4,1},Plant{1,.5},Plant{1,1.5}}) for(auto r:{CornerRun{"R400 clothoid 80 m, 90",arc_lane(400,80,300),kmh(90),true},
                                                                                      CornerRun{"R130 clothoid 40 m, 58",arc_lane(130,40,150),kmh(58),true}}) {
        r.plant=plant;char name[80];std::snprintf(name,sizeof name,"%s rate x%.1f centering x%.1f",r.name.c_str(),plant.rate_scale,plant.centering_scale);r.name=name;
        const auto m=corner_sim(r);print_corner(r,m);
        EXPECT_LT(m.peak,.1)<<r.name;EXPECT_LT(m.yaw_jitter,.006)<<r.name;
    }
}
