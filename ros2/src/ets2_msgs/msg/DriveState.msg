std_msgs/Header header
uint64 sdk_frame
bool available
bool permitted
bool profile_supported
bool armed
bool active
string reason
string owner
uint64 epoch
uint64 sequence
uint64 deadline_ms
uint64 command_window_ms
float32 steering_command
float32 throttle_command
float32 brake_command
# Independently polled physical controls, before SDK semantic input is added.
float32 steering_manual
float32 throttle_manual
float32 brake_manual
# SDK simulation-applied values from this frame. Missing values are NaN.
float64 steering_applied
float64 throttle_applied
float64 brake_applied
string error
# Actual owner axes; 0 when released. This is not the requested UI mode.
uint8 axes
bool independent_axes
# Retarder and engine brake (axes bit 4) need loader 0.42.0+; false/-1 otherwise.
bool auxiliary_brakes
# Commanded retarder step 0..5, -1 when the auxiliary brakes are not owned.
int8 retarder_command
bool engine_brake_command
