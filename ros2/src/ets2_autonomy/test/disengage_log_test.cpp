// The disengagement event log (control/disengage_log.hpp): the ring keeps the last 10 s at
// 10 Hz, the event line carries the reason and the history, the summary names the driver's input.
#include "control/disengage_log.hpp"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>

TEST(DisengageLog,RingKeepsTheLastSecondsAtItsPeriod) {
    DisengageLog log("");
    log.start(80/3.6);
    for(int i=0;i<=1500;++i) {DisengageSample s;s.t=i*.02;s.speed=20;log.sample(s);}   // 30 s at 50 Hz
    const auto& h=log.history();
    EXPECT_NEAR(h.back().t,30,1e-9);
    EXPECT_LE(h.back().t-h.front().t,10+1e-9);
    EXPECT_NEAR(double(h.size()),101,1);
    EXPECT_NEAR(h[1].t-h[0].t,.1,1e-9);
}

TEST(DisengageLog,EventAndSummaryNameTheCause) {
    const auto file=std::filesystem::temp_directory_path()/"disengage_log_test.jsonl";
    std::filesystem::remove(file);
    DisengageLog log(file.string());
    log.start(25);
    for(int i=0;i<50;++i) {
        DisengageSample s;s.t=i*.1;s.speed=24;s.lcc=1;s.lead_gap=40;
        if(i>=45) s.driver_brake=.3;   // the last half second: the driver's brake
        log.sample(s);
    }
    const auto line=log.write("driver_brake","",{{"paused",false}});
    EXPECT_NE(line.find("Disengaged: driver_brake after 4.9 s at 86.4 km/h"),std::string::npos)<<line;
    EXPECT_NE(line.find("brake 0.30"),std::string::npos)<<line;
    EXPECT_NE(line.find("lead 40.0 m"),std::string::npos)<<line;
    log.start(25);
    EXPECT_NE(log.write("error","The armed state did not arrive within 1 s").find("before any control frame"),std::string::npos);
    std::ifstream in(file);std::string first,second;
    ASSERT_TRUE(std::getline(in,first) && std::getline(in,second));
    const auto event=nlohmann::json::parse(first);
    EXPECT_EQ(event["reason"],"driver_brake");
    EXPECT_EQ(event["history"].size(),50u);
    EXPECT_EQ(event["history"].back()["driver_brake"],.3);
    EXPECT_TRUE(event["history"].back()["lane_offset"].is_null());   // NaN: no lane value
    EXPECT_EQ(event["at_end"]["paused"],false);
    EXPECT_DOUBLE_EQ(event["set_speed_kmh"].get<double>(),90);
    EXPECT_EQ(nlohmann::json::parse(second)["detail"],"The armed state did not arrive within 1 s");
    in.close();std::filesystem::remove(file);
}
