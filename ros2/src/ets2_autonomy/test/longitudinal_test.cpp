#include "control/lead_tracker.hpp"
#include <gtest/gtest.h>

TEST(LeadTracker,EstimatesAccelerationOfOneLead) {
    LeadTracker tracker;const LeadTracker::Settings s;
    LeadTracker::Lead lead;
    for(int i=0;i<=150;++i) {const double t=i*.02;lead=tracker.update(7,40-t,25-2*t,t,s);}
    EXPECT_TRUE(lead.valid);EXPECT_EQ(lead.id,7u);
    EXPECT_NEAR(lead.accel_mps2,-2,.05);EXPECT_NEAR(lead.speed_mps,19,1e-9);
}

TEST(LeadTracker,NewLeadRestartsAtZero) {
    LeadTracker tracker;const LeadTracker::Settings s;
    for(int i=0;i<=100;++i) tracker.update(1,50,20-i*.02,i*.02,s);
    // A slower car cuts in: its speed jump is not an acceleration.
    auto lead=tracker.update(2,20,15,2.02,s);
    EXPECT_EQ(lead.id,2u);EXPECT_EQ(lead.accel_mps2,0);EXPECT_EQ(lead.gap_m,20);
    lead=tracker.update(2,20,15,2.04,s);EXPECT_NEAR(lead.accel_mps2,0,1e-9);
}

TEST(LeadTracker,NoLeadClears) {
    LeadTracker tracker;const LeadTracker::Settings s;
    tracker.update(3,30,10,0,s);tracker.update(3,30,11,.1,s);
    EXPECT_FALSE(tracker.update(std::nullopt,0,0,.2,s).valid);
    EXPECT_EQ(tracker.update(3,30,11,.3,s).accel_mps2,0);
}

#include "control/acc_mpc.hpp"

// The generated MPC behind a lead braking from 80 to 40 km/h at -2 m/s^2, on a point mass
// whose acceleration follows a_des with a 0.4 s lag (the acc_smoke_main plant), called
// at the node's 50 Hz: it never closes inside 20 m and settles near the 2 s time gap.
TEST(AccMpc,FollowsABrakingLeadAtTheNodeRate) {
    AccMpc acc;
    double v=80/3.6,a=0,xe=0,vl=v,xl=5+2*v,min_gap=1e9;uint8_t last_mode=0;bool followed=false;
    const double dt=.02;
    for(int k=0;k<=60*50;++k) {
        const double t=k*dt,al=(t>=10 && vl>40/3.6)?-2:0;
        AccMpc::Input in;
        in.enable=true;in.set_speed_mps=90/3.6;in.time_gap_s=2;in.ego_speed_mps=v;in.ego_accel_mps2=a;
        in.lead_valid=true;in.lead_gap_m=xl-xe;in.lead_speed_mps=vl;in.lead_accel_mps2=al;
        in.engine_rpm=1200;in.gear=12;
        const auto& out=acc.update(in,t);
        a+=(out.a_des_mps2-a)*dt/.4;v=std::max(0.0,v+a*dt);xe+=v*dt;vl+=al*dt;xl+=vl*dt;
        min_gap=std::min(min_gap,xl-xe);last_mode=out.mode;followed|=out.mode==AccMpc::Follow;
        EXPECT_GE(out.throttle,0);EXPECT_LE(out.brake,1);EXPECT_GE(out.retarder_step,0);EXPECT_LE(out.retarder_step,5);
    }
    EXPECT_TRUE(followed);EXPECT_EQ(last_mode,AccMpc::Follow);
    EXPECT_GT(min_gap,20);
    EXPECT_NEAR(v,40/3.6,.5);
    EXPECT_NEAR(xl-xe,5+2*vl,6);
}

// The controller reports OFF on its first step after enable and decides on the next; a
// frame inside the 50 ms period holds the previous output.
TEST(AccMpc,StepsOnItsOwn50msSchedule) {
    AccMpc acc;AccMpc::Input in;in.enable=true;in.set_speed_mps=20;in.ego_speed_mps=20;in.gear=12;in.engine_rpm=1200;
    EXPECT_EQ(acc.update(in,0).mode,AccMpc::Off);
    EXPECT_EQ(acc.update(in,.02).mode,AccMpc::Off);
    EXPECT_EQ(acc.update(in,.04).mode,AccMpc::Off);
    EXPECT_EQ(acc.update(in,.06).mode,AccMpc::Cruise);
}

TEST(AccMpc,FoldsRetarderIntoBrake) {
    AccMpc::Output out;out.brake=.05;out.retarder_step=0;
    EXPECT_DOUBLE_EQ(AccMpc::folded_brake(out),.05);
    out.retarder_step=5;EXPECT_NEAR(AccMpc::folded_brake(out),.149,1e-12);
    out.brake=.95;EXPECT_EQ(AccMpc::folded_brake(out),1);
}

// Set speed 0 without a lead (the game test that crept at 1.08 m/s): the controller stops and
// holds. Plant from the controller's own outputs at low speed: the automatic gearbox's idle
// creep (14 kN on 20.3 t while rolling without throttle, the MATLAB plant's IdleCreep) is
// balanced by brake 0.085 as in the game, so the brake decelerates 8.1 m/s^2 per unit; a
// 0.4 s lag; the retarder is left out (little effect below 2 m/s).
TEST(AccMpc,StopsAndHoldsWithoutLead) {
    AccMpc acc;double v=5,a=0;uint8_t mode=0;
    for(int k=0;k<=30*50;++k) {
        AccMpc::Input in;in.enable=true;in.set_speed_mps=0;in.ego_speed_mps=v;in.ego_accel_mps2=a;in.engine_rpm=700;in.gear=1;
        const auto& out=acc.update(in,k*.02);
        const auto creep=v>.05 && out.throttle==0?14000/20300.0:0;
        const auto target=out.throttle*1.2-out.brake*8.1+creep;
        a+=(target-a)*.02/.4;
        v=std::max(0.0,v+a*.02);if(v==0) a=std::max(0.0,a);
        mode=out.mode;
    }
    EXPECT_EQ(mode,AccMpc::Hold);EXPECT_LT(v,.05);
}

#include "control/road_grade.hpp"

TEST(RoadGrade,RiseOverTheLast20m) {
    RoadGrade grade;
    EXPECT_TRUE(std::isnan(grade.update(0,0,0)));
    for(double s=1;s<=60;s+=1) grade.update(s*std::cos(.3),s*std::sin(.3),.02*s);
    EXPECT_NEAR(grade.grade,std::atan(.02),1e-9);
    const auto held=grade.grade;grade.update(60*std::cos(.3),60*std::sin(.3)+.1,5);   // < 0.5 m: standstill
    EXPECT_EQ(grade.grade,held);
}
