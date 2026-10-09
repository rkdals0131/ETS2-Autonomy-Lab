std_msgs/Header header
uint64 sdk_frame
# Driver inputs and simulation-applied inputs from the same SDK frame.
# Steering is [-1,1], positive left. Pedals are [0,1]. Missing values are NaN.
float64 steering_input
float64 throttle_input
float64 brake_input
float64 steering_applied
float64 throttle_applied
float64 brake_applied
# Positive forward gears, zero neutral, negative reverse gears.
bool engine_gear_available
int32 engine_gear
bool displayed_gear_available
int32 displayed_gear
