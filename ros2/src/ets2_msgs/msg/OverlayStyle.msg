# Style of the HUD decal for /ets2/overlay/path. The relay applies the latest style received
# within the last second to each path; without one it uses its overlay_width_m/overlay_alpha
# parameters and white.
std_msgs/Header header
float32 width_m              # ribbon width, (0, 16] m
std_msgs/ColorRGBA color     # components 0..1; a is the opacity
