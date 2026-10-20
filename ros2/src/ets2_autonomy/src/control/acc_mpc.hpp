#pragma once
// Model-predictive ACC generated from apps/control/matlab/acc (ACC_Controller.slx, Embedded
// Coder). Upper MPC asks for an acceleration; the allocator spends it on coasting, retarder
// steps 1-4, the service brake and, for sustained deceleration, retarder 5. The generated
// step is fixed at 50 ms, so the 50 Hz node calls update() every frame and the controller
// steps on its own 50 ms schedule, holding its outputs in between.
// Mass: an RLS estimator inside the controller learns 20.3 t / m from throttle windows (the
// throttle map the allocator inverts, road grade from pitch_rad) and scales the allocator
// maps and the MPC's acceleration limits; the brake efficiency is learnt from braking. With
// the applied pedals and retarder (actuation_valid) it also learns while the driver drives.
#include "ACC_Controller.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>

class AccMpc {
public:
    static constexpr double period_s=.05;
    enum Mode : uint8_t {Off,Cruise,Follow,Stop,Hold,Go,Emergency};
    struct Input {
        bool enable=false;double set_speed_mps=0,time_gap_s=2,ego_speed_mps=0,ego_accel_mps2=0;
        bool lead_valid=false;double lead_gap_m=0,lead_speed_mps=0,lead_accel_mps2=0; // gap bumper to bumper
        bool resume=false;double engine_rpm=0;int32_t gear=0; // SDK engine gear, 0 neutral
        double pitch_rad=0;       // road grade, uphill positive; 0/NaN unknown
        double mass_prior_kg=0;   // truck + trailer + cargo; 0/NaN unknown (starts at 20.3 t)
        bool actuation_valid=false;double throttle_applied=0,brake_applied=0;int32_t retarder_applied=-1; // NaN/-1: own command
    };
    struct Output {
        double throttle=0,brake=0;int32_t retarder_step=0;bool engine_brake=false;
        double a_des_mps2=0;uint8_t mode=Off;
        double mass_kg=0,mass_sigma_kg=0;bool mass_valid=false;
    };
    AccMpc():controller_(std::make_unique<ets2::AccController>()) {controller_->initialize();}
    // t: monotonic seconds. Steps once per 50 ms; after a stall it steps once and restarts
    // the schedule rather than catching up.
    const Output& update(const Input& in,double t) {
        if(stepped_ && t<next_) return output_;
        ets2::AccController::ExtU_ACC_Controller_T u{};
        u.enable=in.enable;u.set_speed_mps=finite(in.set_speed_mps);u.time_gap_s=finite(in.time_gap_s);
        u.ego_speed_mps=finite(in.ego_speed_mps);
        u.ego_accel_mps2=std::abs(in.ego_speed_mps)<.1?0:finite(in.ego_accel_mps2); // contract: 0 at standstill
        u.lead_valid=in.lead_valid && std::isfinite(in.lead_gap_m) && std::isfinite(in.lead_speed_mps);
        u.lead_gap_m=u.lead_valid?in.lead_gap_m:0;u.lead_speed_mps=u.lead_valid?in.lead_speed_mps:0;
        u.lead_accel_mps2=u.lead_valid?finite(in.lead_accel_mps2):0;
        u.resume=in.resume;u.engine_rpm=finite(in.engine_rpm);u.gear=in.gear;
        u.pitch_rad=in.pitch_rad;u.mass_prior_kg=in.mass_prior_kg; // NaN means unknown in the contract
        u.actuation_valid=in.actuation_valid;u.throttle_applied=in.throttle_applied;u.brake_applied=in.brake_applied;u.retarder_applied=in.retarder_applied;
        controller_->setExternalInputs(&u);controller_->step();
        const auto& y=controller_->getExternalOutputs();
        output_={std::clamp(y.throttle,0.0,1.0),std::clamp(y.brake,0.0,1.0),std::clamp(y.retarder_step,0,5),y.engine_brake,y.a_des_mps2,y.mode,
                 y.mass_est_kg,y.mass_sigma_kg,y.mass_valid};
        next_=stepped_ && t-next_<period_s?next_+period_s:t+period_s;stepped_=true;
        return output_;
    }
    const Output& output() const {return output_;}
    // Without the auxiliary-brake axis (loader < 0.42) the retarder step is folded into the
    // service brake: brake input with the same added deceleration at 70 km/h, measured on
    // the automatic gearbox with the trailer (docs/31, acc-20261020-192243): steps 1-4 add
    // 0.05/0.21/0.31/0.39 m/s^2 over coasting, step 5 2.25; brake adds ~22.7 m/s^2 per unit.
    static double folded_brake(const Output& out) {
        static constexpr std::array<double,6> equivalent{0,.002,.009,.014,.017,.099};
        return std::clamp(out.brake+equivalent[std::clamp(out.retarder_step,0,5)],0.0,1.0);
    }
private:
    static double finite(double x) {return std::isfinite(x)?x:0;}
    std::unique_ptr<ets2::AccController> controller_; // ~250 kB of MPC state: on the heap
    Output output_;
    double next_=0;bool stepped_=false;
};
