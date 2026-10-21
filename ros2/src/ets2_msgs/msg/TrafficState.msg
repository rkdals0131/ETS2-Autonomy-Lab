# Internal traffic actor observation at SDK frame_end; independent of camera visibility.
# World ENU coordinates, dimensions in metres, speed in metres/second, yaw radians.
std_msgs/Header header
uint64 sdk_frame
bool available
string error
uint64[] id
float64[] x
float64[] y
float64[] yaw
float64[] speed_mps
float64[] length_m
float64[] width_m
# Box centre height and box height (m). NaN from a core before 0.47.1, which did not report
# them; consumers then treat the actor as on the truck's level.
float64[] z
float64[] height_m
