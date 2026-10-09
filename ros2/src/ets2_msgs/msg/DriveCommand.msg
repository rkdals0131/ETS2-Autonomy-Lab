# Owner is a controller identity, not an authentication token.
string owner
uint64 epoch
# Strictly increasing within the epoch. Start at 1 after successful arm.
uint64 sequence
# Copy from the latest DriveState. Windows monotonic clock, not ROS/sim time.
# Forwarded unchanged; queued or delayed commands do not get a new lifetime.
uint64 command_window_ms
# Positive-left normalized profile input [-1,1]; normalized pedals [0,1].
# In c_relatsteer=1 profiles steering requests position change, not wheel position.
float32 steering
float32 throttle
float32 brake
# Ownership: 1 steering, 2 throttle+brake, 3 both. 0 retains legacy both-axis mode.
uint8 axes
