# drive_speed's assistance state and controller internals, one per control cycle (50 Hz)
# while a session is armed, on /ets2/drive/assist.
std_msgs/Header header
# LCC: 0 off (not enabled), 1 active, 2 yielding to the driver's steering, 3 searching for a
# lane (none, or the truck is off it, e.g. after the driver's lane change)
uint8 lcc_state
bool acc_active
# Lane (map provider): path id, the reference point's lane distance and what is left ahead
int64 lane_path_id
float64 lane_s_m
float64 lane_remaining_m
# Rear reference point against the smoothed lane, left positive, and the soft (re)engagement
# reference (lane_merge.hpp) the controller steers to instead of the centre; 0 when merged
float64 lateral_offset_m
float64 heading_error_rad
float64 merge_offset_m
float64 merge_heading_rad
float64 lane_curvature
# Lane in use: 0 the provider's followed lane, -1/1 its left/right side lane. Turn signal
# lane change: the side requested (waiting for a gap) and the side in progress, 0 none
int8 lane_source
int8 lane_change_request
int8 lane_change_active
# Lateral controller: MPC status (0 off, 1 MPC, 2 QP fallback, 3 low speed, 4 bad input),
# planned front wheel angle and rate, steering target and relative input sent, applied steering
uint8 lcc_status
float64 wheel_angle_target_rad
float64 wheel_rate_target_rad_s
float64 steering_target
float64 steering_input
float64 steering_applied
# ACC: set and target speed, MPC mode (acc_mpc.hpp) and acceleration demand, lead, estimates
float64 set_speed_mps
float64 target_speed_mps
uint8 acc_mode
float64 accel_target_mps2
bool lead_valid
float64 lead_gap_m
float64 lead_speed_mps
float64 mass_kg
float64 grade
# HUD inputs (hud_node): the ACC mark, 0 none, 1 the gap ACC keeps without a lead, 2 the
# lead's rear bumper; its ROS world position and the lane heading there. The lane width, and
# the driver's throttle overriding ACC (ACC waits). LCC's path is /ets2/drive/plan.
uint8 acc_mark
float64 acc_mark_x
float64 acc_mark_y
float64 acc_mark_heading
float64 lane_width_m
bool driver_throttle
# Foxglove scene (hud_node): the lead's and the vehicle holding a turn signal lane change
# back's GT traffic ids (0 none), ACC's target bumper gap (5 m + time gap x speed), the
# lateral acceleration the curve speed is planned with, and the truck's front bumper and
# rear end along base_link x (m).
uint64 lead_id
uint64 lane_change_blocker_id
float64 target_gap_m
float64 curve_lateral_acceleration_mps2
float64 truck_front_m
float64 truck_rear_m
