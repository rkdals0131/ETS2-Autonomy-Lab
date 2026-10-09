std_msgs/Header header
uint64 sdk_frame
bool paused
uint64 render_time_us
uint64 simulation_time_us
uint64 paused_simulation_time_us
# Missing SDK channels are NaN. Inputs are dimensionless [-1,1] or [0,1].
float64 speed_mps
float64 engine_rpm
float64 steering
float64 throttle
float64 brake
float64[3] velocity_base_mps
float64[3] angular_velocity_base_radps
float64[3] acceleration_base_mps2
