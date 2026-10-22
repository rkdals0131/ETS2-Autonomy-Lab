#pragma once
// Why the assistance turned off, without a recording (drive_speed): the control loop's last
// keep_s seconds at one sample per period_s in a ring, written with the reason as one JSON line
// per session end to the event file (the launcher: <session log dir>/disengage.jsonl), and a
// summary line for drive.log. No ROS here.
// Reasons: driver_takeover (the loader released on the driver's input), driver_brake,
// driver_steering, cruise_button_off, cruise_button_reengage, axes_disabled, lane_end,
// game_paused, relay_release (another release reason from the relay), node_stopped, error
// (an exception: lost state, lost lane, a failed arm, ...).
#include <nlohmann/json.hpp>
#include <chrono>
#include <cmath>
#include <ctime>
#include <deque>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

struct DisengageSample {
    double t=0;                                   // s since the session armed
    double speed=0,set_speed=0,target_speed=0;    // m/s
    double accel_target=NAN;int acc_mode=-1;      // MPC (NaN/-1 with the PID)
    double throttle=0,brake=0,steering=0,steering_applied=NAN;    // command, applied steering
    double driver_throttle=0,driver_brake=0,driver_steering=0;    // the driver's inputs
    int lcc=0;                                    // 0 off, 1 steering, 2 yields, 3 searching
    double lane_offset=NAN,heading_error=NAN,lane_remaining=NAN;  // m, rad, m
    int lane_change=0;                            // -1/1 while changing lanes
    double lead_gap=NAN,lead_speed=NAN;           // m, m/s (NaN without a lead)
    double cruise=NAN;                            // the game's cruise speed (0 off, NaN unknown)
    int axes=0;                                   // axes the relay reports armed
    double state_age_ms=0;                        // the older of drive and vehicle state
};

class DisengageLog {
public:
    explicit DisengageLog(std::string file,double keep_s=10,double period_s=.1):file_(std::move(file)),keep_s_(keep_s),period_s_(period_s) {}
    void start(double set_speed) {history_.clear();set_speed_=set_speed;started_=std::chrono::system_clock::now();}
    void sample(const DisengageSample& s) {
        if(!history_.empty() && s.t-history_.back().t<period_s_-1e-9) return;
        history_.push_back(s);
        while(history_.size()>1 && history_.back().t-history_.front().t>keep_s_) history_.pop_front();
    }
    const std::deque<DisengageSample>& history() const {return history_;}
    // The line for the event file (also written there when the file is set).
    nlohmann::json event(const std::string& reason,const std::string& detail,const nlohmann::json& at_end={}) const {
        nlohmann::json samples=nlohmann::json::array();
        for(const auto& s:history_) samples.push_back({
            {"t",round(s.t,2)},{"speed",round(s.speed,2)},{"set_speed",round(s.set_speed,2)},{"target_speed",round(s.target_speed,2)},
            {"accel_target",round(s.accel_target,3)},{"acc_mode",s.acc_mode},{"throttle",round(s.throttle,3)},{"brake",round(s.brake,3)},
            {"steering",round(s.steering,4)},{"steering_applied",round(s.steering_applied,4)},{"driver_throttle",round(s.driver_throttle,3)},
            {"driver_brake",round(s.driver_brake,3)},{"driver_steering",round(s.driver_steering,4)},{"lcc",s.lcc},{"lane_offset",round(s.lane_offset,3)},
            {"heading_error",round(s.heading_error,4)},{"lane_remaining",round(s.lane_remaining,1)},{"lane_change",s.lane_change},
            {"lead_gap",round(s.lead_gap,1)},{"lead_speed",round(s.lead_speed,2)},{"cruise",round(s.cruise,2)},{"axes",s.axes},
            {"state_age_ms",round(s.state_age_ms,0)}});
        return {{"time",stamp(std::chrono::system_clock::now())},{"session_start",stamp(started_)},{"event","disengaged"},{"reason",reason},
                {"detail",detail},{"armed_s",history_.empty()?nlohmann::json(nullptr):nlohmann::json(round(history_.back().t,1))},
                {"set_speed_kmh",round(set_speed_*3.6,1)},{"at_end",at_end},{"history",samples}};
    }
    // Writes the event (when the file is set) and returns the summary line: the reason, how long
    // it was armed, and the last 2 s of the driver's inputs, LCC, the lead and the state age.
    std::string write(const std::string& reason,const std::string& detail,const nlohmann::json& at_end={}) {
        std::string problem;
        if(!file_.empty()) {
            std::ofstream out(file_,std::ios::app);
            if(out) out<<event(reason,detail,at_end).dump()<<'\n';
            if(!out) problem=" (could not write "+file_+")";
        }
        return summary(reason,detail)+problem;
    }
    std::string summary(const std::string& reason,const std::string& detail) const {
        std::ostringstream line;line<<std::fixed<<std::setprecision(1);
        line<<"Disengaged: "<<reason;
        if(!detail.empty()) line<<" ("<<detail<<")";
        if(history_.empty()) {line<<", before any control frame";return line.str();}
        const auto& last=history_.back();
        double throttle=0,brake=0,steering=0,age=0;int searching=0;
        for(const auto& s:history_) if(last.t-s.t<=2) {
            throttle=std::max(throttle,s.driver_throttle);brake=std::max(brake,s.driver_brake);steering=std::max(steering,std::abs(s.driver_steering));
            age=std::max(age,s.state_age_ms);searching+=s.lcc==3;
        }
        line<<" after "<<last.t<<" s at "<<last.speed*3.6<<" km/h; last 2 s: driver throttle "<<std::setprecision(2)<<throttle<<" brake "<<brake
            <<" steering "<<steering<<std::setprecision(1)<<", LCC "<<(last.lcc==1?"steering":last.lcc==2?"yields":last.lcc==3?"searching":"off");
        if(searching) line<<" (searching "<<searching<<" samples)";
        if(std::isfinite(last.lead_gap)) line<<", lead "<<last.lead_gap<<" m";
        line<<", state age max "<<std::setprecision(0)<<age<<" ms";
        if(!file_.empty()) line<<"; "<<history_.size()<<" samples in "<<file_;
        return line.str();
    }
private:
    static nlohmann::json round(double v,int digits) {
        if(!std::isfinite(v)) return nullptr;
        const double f=std::pow(10.0,digits);return std::round(v*f)/f;
    }
    static std::string stamp(std::chrono::system_clock::time_point t) {
        const auto c=std::chrono::system_clock::to_time_t(t);std::tm local{};
#ifdef _WIN32
        localtime_s(&local,&c);
#else
        localtime_r(&c,&local);
#endif
        const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count()%1000;
        std::ostringstream out;out<<std::put_time(&local,"%Y-%m-%dT%H:%M:%S")<<'.'<<std::setw(3)<<std::setfill('0')<<ms;
        return out.str();
    }
    std::string file_;double keep_s_,period_s_;
    std::deque<DisengageSample> history_;double set_speed_=0;
    std::chrono::system_clock::time_point started_{};
};
