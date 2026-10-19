// C ABI over the ROS-free lateral controller (ros2/src/ets2_autonomy/src/control) so
// lane_test.py can drive the game with the exact production law over the ot pipe
// while the ROS bridge is unavailable. One handle per lane path.
#include "control/lateral_controller.hpp"
#include "control/virtual_eps.hpp"
#include <algorithm>
#include <exception>
#include <memory>
#include <string>

namespace {
struct Handle {
    LanePath lane;
    VirtualEps eps;
    std::string error;
    explicit Handle(const char* file):lane(file) {}
};
thread_local std::string last_error;
}

extern "C" {
// Returns nullptr on failure; lat_last_error() then describes it.
__declspec(dllexport) void* lat_create(const char* lane_json) {
    try {return new Handle(lane_json);}
    catch(const std::exception& e) {last_error=e.what();return nullptr;}
}
__declspec(dllexport) const char* lat_last_error() {return last_error.c_str();}
__declspec(dllexport) void lat_destroy(void* handle) {delete static_cast<Handle*>(handle);}
__declspec(dllexport) double lat_lane_width(void* handle) {return static_cast<Handle*>(handle)->lane.width;}

// Pose in ROS world (x east, y = -SCS z, yaw counterclockwise from +x) at the rear
// (bicycle) axle. applied = SDK effective steering. out[10]:
// 0 command u, 1 steering target, 2 target rate, 3 wheel angle, 4 offset (left +),
// 5 heading error, 6 distance to lane, 7 remaining, 8 path curvature, 9 commanded curvature.
__declspec(dllexport) int lat_step(void* handle,double x,double y,double yaw,double speed,double applied,double dt,
                                   double wheelbase,double lookahead_m,double lookahead_time_s,double angle_per_unit,
                                   double bandwidth,double* out) {
    auto& h=*static_cast<Handle*>(handle);
    try {
        LateralController::Settings lateral;
        lateral.lookahead_m=lookahead_m;lateral.lookahead_time_s=lookahead_time_s;
        const auto goal=LateralController::update(h.lane,x,y,yaw,speed,wheelbase,lateral);
        const auto target=std::clamp(goal.wheel_angle/angle_per_unit,-1.0,1.0);
        const auto rate=goal.wheel_rate/angle_per_unit;
        VirtualEps::Settings eps;eps.bandwidth=bandwidth;
        const double values[10]={h.eps.command(target,rate,applied,speed,dt,eps),target,rate,goal.wheel_angle,goal.offset,
                                 goal.heading_error,goal.distance,goal.remaining,goal.path_curvature,goal.curvature};
        std::copy(std::begin(values),std::end(values),out);
        return 0;
    } catch(const std::exception& e) {last_error=e.what();return 1;}
}
}
