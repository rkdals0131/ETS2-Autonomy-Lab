#pragma once
// Model-predictive lane centering generated from apps/control/matlab/lcc (LCC_Controller.slx,
// Adaptive MPC, Embedded Coder). States lateral and heading error, wheel angle, hitch angle and
// the inputs still on their way to the game (measured 3-frame delay); the move is the game's
// relative steering input itself, the lane curvature ahead is a measured disturbance (61
// points, 1 m apart), with curvature-bias and rack input-bias observers and a lateral jerk
// cost. While it steers (status 1) the output angle is the measured wheel angle and the rate
// makes VirtualEps (same table, rate limit off) send exactly the planned input. Step 20 ms:
// update() once per 50 Hz node frame. QP failure and low speed fall back to the built-in
// pure pursuit (status 2/3); bad inputs give status 4 and the caller uses the baseline.
// The plan (status 1 only): predicted lateral and heading error against the reference the
// inputs are taken from (the lane, or the node's merge reference) every predicted_dt_s (0.1 s)
// from the state's time (sample 0 = the inputs) to the end of the 1.5 s horizon: rows of the
// MPC's optimal state sequence (lcc_mpc_pred.m). Zero in every other status.
#include "LCC_Controller.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>

class LccMpc {
public:
    static constexpr int preview_points=61;
    static constexpr double preview_step_m=1;
    static constexpr int predicted_points=16; // t = 0, 0.1, ... 1.5 s (predicted_dt_s)
    enum Status : uint8_t {Off,Mpc,QpFallback,LowSpeed,BadInput};
    struct Input {
        bool enable=false;double speed_mps=0;
        double lateral_error_m=0,heading_error_rad=0; // rear reference point, left positive
        std::array<double,preview_points> curvature_preview{}; // 1/m at s, s+1, ... s+60 m
        double wheel_angle_rad=0,wheelbase_m=0;        // measured front wheel angle
        bool trailer_connected=false;double hitch_angle_rad=0; // truck yaw - trailer yaw
    };
    struct Output {
        double wheel_angle_rad=0,wheel_rate_rad_s=0;uint8_t status=Off;
        // Plan, status Mpc only (arrays zero otherwise): sample i at i*predicted_dt_s after the inputs'
        // time; lateral error of the reference point (left +, m) and heading error (yaw minus
        // reference heading, rad), relative to the reference the inputs were taken against.
        std::array<double,predicted_points> predicted_offset_m{},predicted_heading_rad{};
        double predicted_dt_s=0;
    };
    LccMpc():controller_(std::make_unique<ets2::LccController>()) {controller_->initialize();}
    const Output& update(const Input& in) {
        ets2::LccController::ExtU_LCC_Controller_T u{};
        u.enable=in.enable;u.speed_mps=in.speed_mps;u.lateral_error_m=in.lateral_error_m;u.heading_error_rad=in.heading_error_rad;
        std::copy(in.curvature_preview.begin(),in.curvature_preview.end(),u.curvature_preview);
        u.wheel_angle_rad=in.wheel_angle_rad;u.wheelbase_m=in.wheelbase_m;
        u.trailer_connected=in.trailer_connected;u.hitch_angle_rad=in.trailer_connected?in.hitch_angle_rad:0;
        controller_->setExternalInputs(&u);controller_->step();
        const auto& y=controller_->getExternalOutputs();
        static_assert(sizeof y.predicted_offset_m==sizeof(double)*predicted_points && sizeof y.predicted_heading_rad==sizeof(double)*predicted_points);
        output_={y.wheel_angle_cmd_rad,y.wheel_rate_cmd_rad_s,y.status};
        if(!std::isfinite(output_.wheel_angle_rad) || !std::isfinite(output_.wheel_rate_rad_s)) output_={0,0,BadInput};
        output_.predicted_dt_s=y.predicted_dt_s; // constant 0.1 s, every status
        if(output_.status==Mpc) {
            std::copy_n(y.predicted_offset_m,predicted_points,output_.predicted_offset_m.begin());
            std::copy_n(y.predicted_heading_rad,predicted_points,output_.predicted_heading_rad.begin());
        }
        return output_;
    }
    const Output& output() const {return output_;}
private:
    std::unique_ptr<ets2::LccController> controller_; // ~1.4 MB of QP work arrays: on the heap
    Output output_;
};
