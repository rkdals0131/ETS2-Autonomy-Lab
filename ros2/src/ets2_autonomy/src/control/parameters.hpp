#pragma once
#include <rclcpp/rclcpp.hpp>
#include <array>
#include <cmath>
#include <vector>

struct DrivingParameters {
    bool arm=false,acc_enabled=true,lcc_enabled=true,relative_steering=true;
    std::string path_file;
    double target_speed_mps=0,steering_target=0,steering_gain=2,steering_integral_gain=1;
    double throttle_gain=.2,brake_gain=.2,speed_integral_gain=.08,speed_derivative_gain=0;
    double lookahead_m=6,lookahead_time_s=.7,steering_angle_per_unit_rad=.7;
    double lateral_acceleration_mps2=1.5,stopping_deceleration_mps2=1.5;
    double following_time_s=1.5,standstill_gap_m=5,following_gain=.5;
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
    DrivingDoubleParameter{"steering_gain",&DrivingParameters::steering_gain},
    DrivingDoubleParameter{"steering_integral_gain",&DrivingParameters::steering_integral_gain},
    DrivingDoubleParameter{"throttle_gain",&DrivingParameters::throttle_gain},
    DrivingDoubleParameter{"brake_gain",&DrivingParameters::brake_gain},
    DrivingDoubleParameter{"speed_integral_gain",&DrivingParameters::speed_integral_gain},
    DrivingDoubleParameter{"speed_derivative_gain",&DrivingParameters::speed_derivative_gain},
    DrivingDoubleParameter{"lookahead_m",&DrivingParameters::lookahead_m,DrivingDoubleParameter::Positive},
    DrivingDoubleParameter{"lookahead_time_s",&DrivingParameters::lookahead_time_s},
    DrivingDoubleParameter{"steering_angle_per_unit_rad",&DrivingParameters::steering_angle_per_unit_rad,DrivingDoubleParameter::Positive},
    DrivingDoubleParameter{"lateral_acceleration_mps2",&DrivingParameters::lateral_acceleration_mps2},
    DrivingDoubleParameter{"stopping_deceleration_mps2",&DrivingParameters::stopping_deceleration_mps2,DrivingDoubleParameter::Positive},
    DrivingDoubleParameter{"following_time_s",&DrivingParameters::following_time_s},
    DrivingDoubleParameter{"standstill_gap_m",&DrivingParameters::standstill_gap_m},
    DrivingDoubleParameter{"following_gain",&DrivingParameters::following_gain},
};
inline DrivingParameters declare_driving_parameters(rclcpp::Node& node) {
    DrivingParameters result;
    result.arm=node.declare_parameter("arm",result.arm);
    result.acc_enabled=node.declare_parameter("acc_enabled",result.acc_enabled);
    result.lcc_enabled=node.declare_parameter("lcc_enabled",result.lcc_enabled);
    result.relative_steering=node.declare_parameter("relative_steering",result.relative_steering);
    result.path_file=node.declare_parameter("path_file",result.path_file);
    for(const auto& p:driving_double_parameters) result.*p.value=node.declare_parameter(p.name,result.*p.value);
    return result;
}
inline bool valid_driving_parameters(const DrivingParameters& values) {
    for(const auto& p:driving_double_parameters) if(!p.accepts(values.*p.value)) return false;
    return true;
}
inline rcl_interfaces::msg::SetParametersResult check_driving_parameter_changes(const std::vector<rclcpp::Parameter>& changes) {
    rcl_interfaces::msg::SetParametersResult result;result.successful=true;
    for(const auto& p:changes) {
        const auto& name=p.get_name();
        if(name=="arm" || name=="relative_steering" || name=="path_file") {
            result.successful=false;result.reason="Input mode and arm are startup options; restart explicitly";break;
        }
        if(name=="acc_enabled" || name=="lcc_enabled") {
            if(p.get_type()!=rclcpp::ParameterType::PARAMETER_BOOL) {
                result.successful=false;result.reason="ACC/LCC modes must be bool";break;
            }
        }
        for(const auto& field:driving_double_parameters) if(name==field.name) {
            if(p.get_type()!=rclcpp::ParameterType::PARAMETER_DOUBLE || !field.accepts(p.as_double())) {
                result.successful=false;result.reason="Speed/gains must be finite and nonnegative; steering must be in [-1,1]";break;
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
