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
    enum Status : uint8_t {Off,Mpc,QpFallback,LowSpeed,BadInput};
    struct Input {
        bool enable=false;double speed_mps=0;
        double lateral_error_m=0,heading_error_rad=0; // rear reference point, left positive
        std::array<double,preview_points> curvature_preview{}; // 1/m at s, s+1, ... s+60 m
        double wheel_angle_rad=0,wheelbase_m=0;        // measured front wheel angle
        bool trailer_connected=false;double hitch_angle_rad=0; // truck yaw - trailer yaw
    };
    struct Output {double wheel_angle_rad=0,wheel_rate_rad_s=0;uint8_t status=Off;};
    LccMpc():controller_(std::make_unique<ets2::LccController>()) {controller_->initialize();}
    const Output& update(const Input& in) {
        ets2::LccController::ExtU_LCC_Controller_T u{};
        u.enable=in.enable;u.speed_mps=in.speed_mps;u.lateral_error_m=in.lateral_error_m;u.heading_error_rad=in.heading_error_rad;
        std::copy(in.curvature_preview.begin(),in.curvature_preview.end(),u.curvature_preview);
        u.wheel_angle_rad=in.wheel_angle_rad;u.wheelbase_m=in.wheelbase_m;
        u.trailer_connected=in.trailer_connected;u.hitch_angle_rad=in.trailer_connected?in.hitch_angle_rad:0;
        controller_->setExternalInputs(&u);controller_->step();
        const auto& y=controller_->getExternalOutputs();
        output_={y.wheel_angle_cmd_rad,y.wheel_rate_cmd_rad_s,y.status};
        if(!std::isfinite(output_.wheel_angle_rad) || !std::isfinite(output_.wheel_rate_rad_s)) output_={0,0,BadInput};
        return output_;
    }
    const Output& output() const {return output_;}
private:
    std::unique_ptr<ets2::LccController> controller_; // ~1.4 MB of QP work arrays: on the heap
    Output output_;
};
