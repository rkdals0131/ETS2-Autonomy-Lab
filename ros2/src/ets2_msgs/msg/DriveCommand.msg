# Owner is a controller identity, not an authentication token.
string owner
uint64 epoch
# Strictly increasing within the epoch. Start at 1 after successful arm.
uint64 sequence
# Sender's std::chrono::steady_clock in ns, taken when the command is computed:
# CLOCK_MONOTONIC on Linux, QPC on Windows (Python: time.perf_counter_ns, never
# time.monotonic). The receiver on the same host drops commands older than
# max_command_age_ms; the relay then grants a 200 ms window from receipt, like a
# drive-by-wire heartbeat.
uint64 sent_steady_ns
# Positive-left normalized profile input [-1,1]; normalized pedals [0,1].
# In c_relatsteer=1 profiles steering requests position change, not wheel position.
float32 steering
float32 throttle
float32 brake
# Ownership: 1 steering, 2 throttle+brake, 3 both, plus 4 for the auxiliary brakes
# (only with 2: 6 ACC with retarder, 7 all). 0 retains legacy both-axis mode.
uint8 axes
# With axes bit 4: retarder step 0..5 (0 off) and the engine brake held. The loader
# pulses the step on change and every 0.5 s, and step 0 once when control ends.
int8 retarder_step
bool engine_brake
