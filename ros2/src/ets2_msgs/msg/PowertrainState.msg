std_msgs/Header header
uint64 sdk_frame
# Powertrain and brake signals a truck's CAN bus carries (J1939 EEC1, ETC1/2, ERC1,
# EBC1/2, ET1, EFL/P1, VEP1, LFE-like). Missing values are NaN; flags are false.

# From SDK channels.
float64 engine_rpm
float64 clutch_applied              # effective clutch, 0 engaged .. 1 open
float64 cruise_control_mps          # 0 when off
float64 oil_temperature_c
float64 oil_pressure_psi
float64 water_temperature_c
float64 battery_voltage_v
float64 fuel_l
float64 fuel_consumption_average_lpkm
float64 adblue_l
float64 air_pressure_psi
float64 brake_temperature_c
uint32 retarder_step                # 0 off .. truck_config retarder.steps
bool engine_brake_active
bool parking_brake
bool lift_axle_raised

# From the vehicle's physics (ot Tier 1); NaN and an empty array without it.
bool drivetrain_available
float64 engine_speed_radps
float64 engine_torque_nm            # actual; negative is friction or engine brake
float64 output_shaft_speed_radps
float64 output_torque_nm            # transmission output; includes the retarder
float64 gear_ratio                  # current gear, 0 in neutral
float64 retarder_level              # 0 .. 1 of the retarder's full torque
float64 engine_brake_level          # 0 .. 1
float64[] wheel_brake_torque_nm     # per SDK wheel index; lifted wheels may report torque
