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
