#pragma once
#include "lateral_controller.hpp"
#include "virtual_eps.hpp"
#include <rclcpp/rclcpp.hpp>
#include <array>
#include <cmath>
#include <vector>

struct DrivingParameters {
    bool arm=false,acc_enabled=true,lcc_enabled=true,relative_steering=true;
    std::string path_file;
    // "mpc": generated model-predictive ACC (acc_mpc.hpp) with the retarder; "pid": the
    // earlier speed PID with a gap-to-speed lead rule.
    std::string longitudinal="mpc";
    // "mpc": generated model-predictive lane centering (lcc_mpc.hpp) with the trailer;
    // "pp": the linearized pure pursuit with curvature feedforward (lateral_controller.hpp).
    std::string lateral="mpc";
    // "start": arm at start and drive until released. "cruise": stand by; the game's cruise
    // control button engages at the speed it sets (the game's cruise is cancelled once the
    // node holds the pedals) and disengages on the next press; path_file is then the map
    // provider's --follow output, read at each engagement and extended while driving.
    std::string engage="start";
    double target_speed_mps=0,steering_target=0;
    // Relative-steering inner loop (virtual_eps.hpp) and lane feedforward (lateral_controller.hpp).
    // The steering rate and self-centering are a measured speed table inside VirtualEps.
    double steering_bandwidth=6.5,steering_rate_limit=0;
    double curvature_preview_s=0,curvature_window_m=6;
    double throttle_gain=.2,brake_gain=.2,speed_integral_gain=.08,speed_derivative_gain=0;
    // Mean front wheel angle per applied steering unit, measured 2026-10-20: 0.698-0.700 rad at
    // 20-80 km/h (0.7087 at standstill, linear, residual <0.004 rad).
    double lookahead_m=6,lookahead_time_s=.7,steering_angle_per_unit_rad=.70;
    // Curve speed while LCC steers (curve_speed.hpp): lateral acceleration on the lane ahead and
    // the deceleration it is planned with (under the ACC's 0.5 m/s^2 set-speed ramp).
    double lateral_acceleration_mps2=2,curve_deceleration_mps2=.45,stopping_deceleration_mps2=1.5;
    // following_time_s 2.0 is the time gap the MPC was tuned for; its standstill gap is 5 m
    // bumper to bumper, built into the controller (standstill_gap_m is the PID's).
    double following_time_s=2,standstill_gap_m=5,following_gain=.5;
    // Front axle to front bumper; not measured, Scania S cab estimate.
    double front_overhang_m=1.5;
    // Turn signal lane change: peak lateral acceleration of its path and its shortest
    // duration (one 4.5 m lane at 0.9 m/s^2: about 5.6 s at any speed).
    double lane_change_lateral_acceleration_mps2=.9,lane_change_min_s=4;
    // The driver's steering for this long ends ACC and LCC (shorter: LCC yields).
    double steering_takeover_s=3;
    uint8_t axes() const {return (lcc_enabled?1:0)|(acc_enabled?2:0);}
};

struct DrivingDoubleParameter {
    const char* name;
    double DrivingParameters::*value;
    enum Range {Nonnegative,Positive,Steering} range=Nonnegative;
    bool accepts(double x) const {
        return std::isfinite(x) && (range==Steering?std::abs(x)<=1:range==Positive?x>0:x>=0);
    }
};
inline constexpr std::array driving_double_parameters{
    DrivingDoubleParameter{"target_speed_mps",&DrivingParameters::target_speed_mps},
    DrivingDoubleParameter{"steering_target",&DrivingParameters::steering_target,DrivingDoubleParameter::Steering},
    DrivingDoubleParameter{"steering_bandwidth",&DrivingParameters::steering_bandwidth,DrivingDoubleParameter::Positive},
    DrivingDoubleParameter{"steering_rate_limit",&DrivingParameters::steering_rate_limit},
    DrivingDoubleParameter{"curvature_preview_s",&DrivingParameters::curvature_preview_s},
    DrivingDoubleParameter{"curvature_window_m",&DrivingParameters::curvature_window_m,DrivingDoubleParameter::Positive},
    DrivingDoubleParameter{"throttle_gain",&DrivingParameters::throttle_gain},
    DrivingDoubleParameter{"brake_gain",&DrivingParameters::brake_gain},
    DrivingDoubleParameter{"speed_integral_gain",&DrivingParameters::speed_integral_gain},
    DrivingDoubleParameter{"speed_derivative_gain",&DrivingParameters::speed_derivative_gain},
    DrivingDoubleParameter{"lookahead_m",&DrivingParameters::lookahead_m,DrivingDoubleParameter::Positive},
    DrivingDoubleParameter{"lookahead_time_s",&DrivingParameters::lookahead_time_s},
    DrivingDoubleParameter{"steering_angle_per_unit_rad",&DrivingParameters::steering_angle_per_unit_rad,DrivingDoubleParameter::Positive},
    DrivingDoubleParameter{"lateral_acceleration_mps2",&DrivingParameters::lateral_acceleration_mps2,DrivingDoubleParameter::Positive},
    DrivingDoubleParameter{"curve_deceleration_mps2",&DrivingParameters::curve_deceleration_mps2,DrivingDoubleParameter::Positive},
    DrivingDoubleParameter{"stopping_deceleration_mps2",&DrivingParameters::stopping_deceleration_mps2,DrivingDoubleParameter::Positive},
    DrivingDoubleParameter{"following_time_s",&DrivingParameters::following_time_s},
    DrivingDoubleParameter{"standstill_gap_m",&DrivingParameters::standstill_gap_m},
    DrivingDoubleParameter{"following_gain",&DrivingParameters::following_gain},
    DrivingDoubleParameter{"front_overhang_m",&DrivingParameters::front_overhang_m},
    DrivingDoubleParameter{"lane_change_lateral_acceleration_mps2",&DrivingParameters::lane_change_lateral_acceleration_mps2,DrivingDoubleParameter::Positive},
    DrivingDoubleParameter{"lane_change_min_s",&DrivingParameters::lane_change_min_s},
    DrivingDoubleParameter{"steering_takeover_s",&DrivingParameters::steering_takeover_s,DrivingDoubleParameter::Positive},
};
inline DrivingParameters declare_driving_parameters(rclcpp::Node& node) {
    DrivingParameters result;
    result.arm=node.declare_parameter("arm",result.arm);
    result.acc_enabled=node.declare_parameter("acc_enabled",result.acc_enabled);
    result.lcc_enabled=node.declare_parameter("lcc_enabled",result.lcc_enabled);
    result.relative_steering=node.declare_parameter("relative_steering",result.relative_steering);
    result.path_file=node.declare_parameter("path_file",result.path_file);
    result.longitudinal=node.declare_parameter("longitudinal",result.longitudinal);
    result.lateral=node.declare_parameter("lateral",result.lateral);
    result.engage=node.declare_parameter("engage",result.engage);
    for(const auto& p:driving_double_parameters) result.*p.value=node.declare_parameter(p.name,result.*p.value);
    return result;
}
inline bool valid_driving_parameters(const DrivingParameters& values) {
    if(values.longitudinal!="mpc" && values.longitudinal!="pid") return false;
    if(values.lateral!="mpc" && values.lateral!="pp") return false;
    if(values.engage!="start" && values.engage!="cruise") return false;
    for(const auto& p:driving_double_parameters) if(!p.accepts(values.*p.value)) return false;
    return true;
}
inline rcl_interfaces::msg::SetParametersResult check_driving_parameter_changes(const std::vector<rclcpp::Parameter>& changes) {
    rcl_interfaces::msg::SetParametersResult result;result.successful=true;
    for(const auto& p:changes) {
        const auto& name=p.get_name();
        if(name=="arm" || name=="relative_steering" || name=="path_file" || name=="longitudinal" || name=="lateral" || name=="engage") {
            result.successful=false;result.reason="Input mode and arm are startup options; restart explicitly";break;
        }
        if(name=="acc_enabled" || name=="lcc_enabled") {
            if(p.get_type()!=rclcpp::ParameterType::PARAMETER_BOOL) {
                result.successful=false;result.reason="ACC/LCC modes must be bool";break;
            }
        }
        for(const auto& field:driving_double_parameters) if(name==field.name) {
            if(p.get_type()!=rclcpp::ParameterType::PARAMETER_DOUBLE || !field.accepts(p.as_double())) {
                result.successful=false;result.reason="Speed/gains must be finite and nonnegative (positive for steering bandwidth, wheel angle per unit, lookahead, window, lateral acceleration, curve and stopping deceleration); steering must be in [-1,1]";break;
            }
        }
        if(!result.successful) break;
    }
    return result;
}
inline void apply_driving_parameter_changes(DrivingParameters& values,const std::vector<rclcpp::Parameter>& changes) {
    for(const auto& p:changes) {
        const auto& name=p.get_name();
        if(name=="acc_enabled") values.acc_enabled=p.as_bool();
        else if(name=="lcc_enabled") values.lcc_enabled=p.as_bool();
        else for(const auto& field:driving_double_parameters) if(name==field.name) values.*field.value=p.as_double();
    }
}
inline VirtualEps::Settings eps_settings(const DrivingParameters& values) {
    return {values.steering_bandwidth,values.steering_rate_limit};
}
inline LateralController::Settings lateral_settings(const DrivingParameters& values) {
    return {values.lookahead_m,values.lookahead_time_s,values.curvature_preview_s,values.curvature_window_m};
}
