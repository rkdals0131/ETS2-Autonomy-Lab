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
#include <gtest/gtest.h>
#include <array>
#include <cmath>
#include <cstdio>
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
struct Scenario {std::string name;Points points;double speed,offset=0,duration=0,settle_s=0;Plant plant{};};
struct Metrics {double max_offset=0,settled_offset=0,rms=0,overshoot=0,max_applied=0;int reversals=0;double input_variation=0;
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
// trailer: the plant here is the measured truck-alone steering.
Controller lateral_mpc() {
    return [mpc=std::make_shared<LccMpc>(),eps=VirtualEps{}](LanePath& lane,double x,double y,double yaw,double v,double applied) mutable {
        const auto goal=LateralController::update(lane,x,y,yaw,v,wheelbase,{});
        LccMpc::Input in;in.enable=true;in.speed_mps=v;in.lateral_error_m=goal.offset;in.heading_error_rad=goal.heading_error;
        for(int i=0;i<LccMpc::preview_points;++i) in.curvature_preview[i]=lane.curvature(goal.s+i*LccMpc::preview_step_m,6);
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
    const auto heading=lane.heading(0);const auto start=lane.at(0);
    double x=start.x-std::sin(heading)*scenario.offset,y=start.y+std::cos(heading)*scenario.offset,yaw=heading;
    double applied=std::clamp(std::atan(wheelbase*lane.curvature(0,5))/angle_per_unit,-1.0,1.0);
    double latched=0,active=0,command=0,next_control=.003; // controller phase vs game frames
    Metrics m;double sum2=0,last_u=0,extreme=applied;int direction=0,samples=0;
    const auto [rate,centering]=measured_plant(scenario.speed);
    const auto frames=int(scenario.duration/game_dt);
    for(int n=0;n<frames;++n) {
        const auto t=n*game_dt;
        // Controller ticks in (t_{n-1}, t_n] see the state after frame n-1.
        for(;next_control<=t;next_control+=control_dt) {
            if(rolling && lane.remaining()<600 && lane.extend(window(lane.points.back().s,lane.progress+1000))>0) ++m.extensions;
            command=controller(lane,x,y,yaw,scenario.speed,applied);
            m.input_variation+=std::abs(command-last_u);last_u=command;
            const auto p=truth.track(x,y);
            m.offsets.push_back(p.offset);
            m.max_offset=std::max(m.max_offset,std::abs(p.offset));
            if(next_control>=scenario.settle_s) {m.settled_offset=std::max(m.settled_offset,std::abs(p.offset));sum2+=p.offset*p.offset;++samples;}
            if(scenario.offset>0) m.overshoot=std::max(m.overshoot,-p.offset);
        }
        // The command latched at the previous frame boundary acts over this frame.
        active=latched;latched=command;
        applied=std::clamp(applied+game_dt*(scenario.plant.rate_scale*rate*active-scenario.plant.centering_scale*centering*applied),-scenario.plant.max,scenario.plant.max);
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
    EXPECT_LT(now.max_offset,.04);
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
