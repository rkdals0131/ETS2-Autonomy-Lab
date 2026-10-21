# The planner's decision state, one per tick (10 Hz) on /ets2/planning/behavior: its behavior
# tree, what each node returned in this tick, and the maneuver state machine. The tree is sent
# whole every time (a few dozen nodes), so a viewer that joins late or replays a bag needs
# nothing else.
std_msgs/Header header
# Tree nodes in depth-first order: name, kind (the registered node type, e.g. Sequence,
# DriverSignal) and the parent's index (-1 for the root).
string[] node_name
string[] node_kind
int32[] node_parent
# What each node returned in this tick: 0 not ticked, 1 running, 2 success, 3 failure.
uint8[] node_status
# The lane change state machine: its state names, the active one (index) and why it is there.
string[] fsm_state
int32 fsm_active
string fsm_note
# The decision in words, and whether the planner only shows it (shadow: drive_speed still
# decides and executes) or commands it.
string decision
bool shadow
