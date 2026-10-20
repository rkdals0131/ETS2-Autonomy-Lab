function model = build_longitudinal_model(opts)
%BUILD_LONGITUDINAL_MODEL Create longitudinal_plant.slx from basic Simulink blocks.
%   build_longitudinal_model() rebuilds the model next to this file. Every block
%   parameter refers to the struct P from longitudinal_params.m (put it in the base
%   workspace or pass it with Simulink.SimulationInput/setVariable).
%
%   Root inputs (recorded in frames.jsonl, one column each in the external input):
%     1 thr        applied throttle (eff_thr, 0..1)
%     2 brk        applied service brake (eff_brk, 0..1)
%     3 ratio      gear ratio (dt.gear_ratio, 0 in neutral)
%     4 clutch     clutch (dt.clutch, 0 engaged .. 1 open)
%     5 ret_level  retarder level (dt.retarder_level, 0..1; 1.0 = step 5 = + engine brake)
%     6 eb_flag    engine brake switch (dt.engine_brake, 0/1)
%     7 brake_temp brake temperature deg C (x truck.brake.temperature)
%     8 grade      road grade sin(theta) (0 on the test pad)
%   Root outputs: 1 v (m/s), 2 a (m/s^2), 3 Te (Nm), 4 To (gearbox output torque
%   incl. retarder, Nm), 5 rpm, 6 F_brake (N), 7 T_ret (Nm, output shaft),
%   8 Tb_truck (sum of truck wheel brake torque, Nm).
arguments
    opts.Name (1,1) string = "longitudinal_plant"
    opts.Save (1,1) logical = true
end
here = fileparts(mfilename("fullpath"));
model = char(opts.Name);
if bdIsLoaded(model), close_system(model, 0); end
file = fullfile(here, model + ".slx");
if isfile(file), delete(file); end
new_system(model);
load_system(model);
set_param(model, "Solver", "ode3", "FixedStep", "P.sim.step", "StopTime", "10", ...
    "SaveOutput", "on", "OutputSaveName", "yout", "SaveFormat", "Array", ...
    "SaveTime", "on", "SaveState", "off", "LoadExternalInput", "on", ...
    "ExternalInput", "u_ext", "BooleanDataType", "off", ...
    "AlgebraicLoopMsg", "error", "ReturnWorkspaceOutputs", "on");
set_param(model, "Description", ...
    "ETS2 Scania S 6x2 longitudinal plant. Built by build_longitudinal_model.m; parameters in longitudinal_params.m.");

% ---------------- root ports ------------------------------------------------------
inNames = ["thr", "brk", "ratio", "clutch", "ret_level", "eb_flag", "brake_temp", "grade"];
for i = 1:numel(inNames)
    add_block("simulink/Sources/In1", model + "/" + inNames(i), "Port", num2str(i));
end
outNames = ["v", "a", "Te", "To", "rpm", "F_brake", "T_ret", "Tb_truck"];
for i = 1:numel(outNames)
    add_block("simulink/Sinks/Out1", model + "/" + outNames(i) + "_out", "Port", num2str(i));
end

eng = subsystem(model, "Engine", ["thr", "w_e", "eb_flag", "ret_level", "locked", "brk"], "Te");
build_engine(eng);
gb = subsystem(model, "Gearbox_Clutch", ["Te", "ratio", "clutch", "v", "a_body"], ...
    ["F_eng", "m_rot", "To_eng", "w_e", "locked"]);
build_gearbox(gb);
ret = subsystem(model, "Retarder", ["ret_level", "v"], ["T_ret", "F_ret"]);
build_retarder(ret);
brk = subsystem(model, "Service_Brake", ["brk", "brake_temp", "v"], ["F_brake", "Tb_truck"]);
build_brake(brk);
veh = subsystem(model, "Vehicle", ["F_eng", "F_ret", "F_brake", "m_rot", "grade"], ["v", "a", "F_res"]);
build_vehicle(veh);

m = model;
blk(m, "Math Operations/Sum", "To_sum", "Inputs", "++");
blk(m, "Math Operations/Gain", "to_rpm", "Gain", "60/(2*pi)");
ln(m, "thr/1", "Engine/1"); ln(m, "Gearbox_Clutch/4", "Engine/2"); ln(m, "eb_flag/1", "Engine/3");
ln(m, "ret_level/1", "Engine/4"); ln(m, "Gearbox_Clutch/5", "Engine/5"); ln(m, "brk/1", "Engine/6");
ln(m, "Engine/1", "Gearbox_Clutch/1"); ln(m, "ratio/1", "Gearbox_Clutch/2"); ln(m, "clutch/1", "Gearbox_Clutch/3");
ln(m, "Vehicle/1", "Gearbox_Clutch/4"); ln(m, "Vehicle/2", "Gearbox_Clutch/5");
ln(m, "ret_level/1", "Retarder/1"); ln(m, "Vehicle/1", "Retarder/2");
ln(m, "brk/1", "Service_Brake/1"); ln(m, "brake_temp/1", "Service_Brake/2"); ln(m, "Vehicle/1", "Service_Brake/3");
ln(m, "Gearbox_Clutch/1", "Vehicle/1"); ln(m, "Retarder/2", "Vehicle/2"); ln(m, "Service_Brake/1", "Vehicle/3");
ln(m, "Gearbox_Clutch/2", "Vehicle/4"); ln(m, "grade/1", "Vehicle/5");
ln(m, "Gearbox_Clutch/3", "To_sum/1"); ln(m, "Retarder/1", "To_sum/2");
ln(m, "Gearbox_Clutch/4", "to_rpm/1");
ln(m, "Vehicle/1", "v_out/1"); ln(m, "Vehicle/2", "a_out/1"); ln(m, "Engine/1", "Te_out/1");
ln(m, "To_sum/1", "To_out/1"); ln(m, "to_rpm/1", "rpm_out/1"); ln(m, "Service_Brake/1", "F_brake_out/1");
ln(m, "Retarder/1", "T_ret_out/1"); ln(m, "Service_Brake/2", "Tb_truck_out/1");
blk(m, "Sinks/Terminator", "F_res_unused"); ln(m, "Vehicle/3", "F_res_unused/1");
Simulink.BlockDiagram.arrangeSystem(model);
for s = ["Engine", "Gearbox_Clutch", "Retarder", "Service_Brake", "Vehicle"]
    Simulink.BlockDiagram.arrangeSystem(model + "/" + s);
end
if opts.Save
    save_system(model, file);
end
end

% =====================================================================
function build_engine(s)
% Engine: desired torque from the throttle / engine brake maps, then a rise-rate
% limited first-order tracker (turbo lag) as the actual torque state. The engine
% brake works with the B switch, retarder step 5, or (profile g_motor_brake_auto 1)
% a service brake input of at least P.eng.auto_eb_brake, all only while the applied
% throttle is at most P.eng.eb_thr_max.
blk(s, "Math Operations/Gain", "rpm", "Gain", "60/(2*pi)");
blk(s, "Discontinuities/Saturation", "rpm_sat", "UpperLimit", "P.eng.curve_rpm(end)", "LowerLimit", "P.eng.curve_rpm(1)");
lut1(s, "full_load", "P.eng.curve_rpm", "P.eng.T_max*P.eng.curve_frac");
blk(s, "Math Operations/Gain", "fric_slope", "Gain", "P.eng.fric1");
blk(s, "Math Operations/Bias", "friction", "Bias", "P.eng.fric0");
lut2(s, "part_load_frac", "P.eng.thr_bp", "P.eng.rpm_bp", "P.eng.frac_table");
blk(s, "Math Operations/Sum", "span", "Inputs", "+-");
blk(s, "Math Operations/Product", "frac_x_span", "Inputs", "2");
blk(s, "Math Operations/Sum", "T_throttle", "Inputs", "++");
lut1(s, "engine_brake", "P.eng.eb_rpm", "P.eng.eb_nm");
cmp(s, "ret_step5", ">=", "P.ret.eb_level");
cmp(s, "eb_switch", ">", "0.5");
cmp(s, "auto_eb_brake", ">=", "P.eng.auto_eb_brake");
blk(s, "Logic and Bit Operations/Logical Operator", "eb_or", "Operator", "OR", "Inputs", "3");
cmp(s, "throttle_zero", "<=", "P.eng.eb_thr_max");
blk(s, "Logic and Bit Operations/Logical Operator", "eb_on", "Operator", "AND");
blk(s, "Signal Routing/Switch", "select", "Criteria", "u2 > Threshold", "Threshold", "0.5");
blk(s, "Sources/Constant", "idle_w", "Value", "P.eng.idle_rpm*2*pi/60");
blk(s, "Math Operations/Sum", "idle_err", "Inputs", "+-");
blk(s, "Math Operations/Gain", "gov_gain", "Gain", "P.eng.gov_gain");
blk(s, "Discontinuities/Saturation", "gov_sat", "UpperLimit", "P.eng.T_max", "LowerLimit", "0");
blk(s, "Math Operations/Sum", "T_des", "Inputs", "++");
blk(s, "Math Operations/Sum", "T_err", "Inputs", "+-");
blk(s, "Math Operations/Gain", "inv_tau", "Gain", "1/P.eng.tau");
lut2(s, "rise_rate", "P.eng.rate_te_bp", "P.eng.rate_rpm_bp", "P.eng.rate_up_table");
blk(s, "Sources/Constant", "rate_open", "Value", "P.eng.rate_open");
blk(s, "Signal Routing/Switch", "rate_sel", "Criteria", "u2 > Threshold", "Threshold", "0.5");
blk(s, "Sources/Constant", "rate_down", "Value", "-P.eng.rate_down");
blk(s, "Discontinuities/Saturation Dynamic", "rate_limit");
blk(s, "Continuous/Integrator", "Te_state", "InitialCondition", "P.init.Te0");

ln(s, "w_e/1", "rpm/1"); ln(s, "rpm/1", "rpm_sat/1"); ln(s, "rpm_sat/1", "full_load/1");
ln(s, "rpm_sat/1", "fric_slope/1"); ln(s, "fric_slope/1", "friction/1");
ln(s, "thr/1", "part_load_frac/1"); ln(s, "rpm_sat/1", "part_load_frac/2");
ln(s, "full_load/1", "span/1"); ln(s, "friction/1", "span/2");
ln(s, "part_load_frac/1", "frac_x_span/1"); ln(s, "span/1", "frac_x_span/2");
ln(s, "friction/1", "T_throttle/1"); ln(s, "frac_x_span/1", "T_throttle/2");
ln(s, "rpm/1", "engine_brake/1");
ln(s, "ret_level/1", "ret_step5/1"); ln(s, "eb_flag/1", "eb_switch/1");
ln(s, "eb_switch/1", "eb_or/1"); ln(s, "ret_step5/1", "eb_or/2");
ln(s, "brk/1", "auto_eb_brake/1"); ln(s, "auto_eb_brake/1", "eb_or/3");
ln(s, "thr/1", "throttle_zero/1"); ln(s, "eb_or/1", "eb_on/1"); ln(s, "throttle_zero/1", "eb_on/2");
ln(s, "engine_brake/1", "select/1"); ln(s, "eb_on/1", "select/2"); ln(s, "T_throttle/1", "select/3");
ln(s, "idle_w/1", "idle_err/1"); ln(s, "w_e/1", "idle_err/2"); ln(s, "idle_err/1", "gov_gain/1");
ln(s, "gov_gain/1", "gov_sat/1");
ln(s, "select/1", "T_des/1"); ln(s, "gov_sat/1", "T_des/2");
ln(s, "T_des/1", "T_err/1"); ln(s, "Te_state/1", "T_err/2"); ln(s, "T_err/1", "inv_tau/1");
ln(s, "Te_state/1", "rise_rate/1"); ln(s, "rpm_sat/1", "rise_rate/2");
ln(s, "rise_rate/1", "rate_sel/1"); ln(s, "locked/1", "rate_sel/2"); ln(s, "rate_open/1", "rate_sel/3");
ln(s, "rate_sel/1", "rate_limit/1"); ln(s, "inv_tau/1", "rate_limit/2"); ln(s, "rate_down/1", "rate_limit/3");
ln(s, "rate_limit/1", "Te_state/1"); ln(s, "Te_state/1", "Te/1");
end

function build_gearbox(s)
% Gearbox + clutch.
%  Locked (clutch < P.gb.clutch_lock, in gear, above 0.9*idle): engine speed =
%  ratio*output speed, drive torque = Te, reflected inertia J_locked.
%  Slipping / open: clutch torque = clamp(K_slip*e*slip, +-C_clutch*e), e = (1-c)^clutch_exp,
%  free engine with J_free, pulled down to the launch speed the game holds.
%  Drive force = eta*ratio*fd/r*(drive torque), with eta_coast instead of eta when
%  the drive torque is negative (engine braking / friction). The reported output torque (the
%  Tier 1 channel) is eta*ratio*(1-c)*(Te - J_report*d(w_e)/dt), plus the retarder
%  at the root.
blk(s, "Math Operations/Gain", "w_out", "Gain", "P.veh.fd/P.veh.r");
blk(s, "Math Operations/Product", "ratio_w_out", "Inputs", "2");
cmp(s, "in_gear", ">", "0.1");
cmp(s, "clutch_closed", "<", "P.gb.clutch_lock");
cmp(s, "above_lock_min", ">=", "0.9*P.eng.idle_rpm*2*pi/60");
blk(s, "Logic and Bit Operations/Logical Operator", "lock", "Operator", "AND", "Inputs", "3");
blk(s, "Sources/Constant", "one", "Value", "1");
blk(s, "Math Operations/Sum", "one_minus_c", "Inputs", "+-");
% clutch slip torque, engagement e = (1-c)^P.gb.clutch_exp
blk(s, "Discontinuities/Saturation", "one_minus_c_sat", "UpperLimit", "1", "LowerLimit", "0");
blk(s, "Sources/Constant", "clutch_exp", "Value", "P.gb.clutch_exp");
blk(s, "Math Operations/Math Function", "engagement", "Operator", "pow");
blk(s, "Math Operations/Sum", "slip", "Inputs", "+-");
blk(s, "Math Operations/Product", "engaged_slip", "Inputs", "2");
blk(s, "Math Operations/Gain", "K_slip", "Gain", "P.gb.K_slip");
blk(s, "Math Operations/Gain", "capacity", "Gain", "P.gb.C_clutch");
blk(s, "Math Operations/Gain", "neg_capacity", "Gain", "-1");
blk(s, "Discontinuities/Saturation Dynamic", "T_clutch");
blk(s, "Math Operations/Product", "T_clutch_gear", "Inputs", "2");
blk(s, "Signal Routing/Switch", "T_drive", "Criteria", "u2 > Threshold", "Threshold", "0.5");
blk(s, "Math Operations/Gain", "eta", "Gain", "P.gb.eta");
blk(s, "Math Operations/Product", "drive_out", "Inputs", "3");
blk(s, "Sources/Constant", "eta_drive", "Value", "1");
blk(s, "Sources/Constant", "eta_coast_rel", "Value", "P.gb.eta_coast/P.gb.eta");
cmp(s, "driving", ">=", "0");
blk(s, "Signal Routing/Switch", "eta_select", "Criteria", "u2 > Threshold", "Threshold", "0.5");
blk(s, "Math Operations/Gain", "to_force", "Gain", "P.veh.fd/P.veh.r");
blk(s, "Math Operations/Product", "lock_ratio2", "Inputs", "3");
blk(s, "Math Operations/Gain", "reflected_mass", "Gain", "P.gb.J_locked*(P.veh.fd/P.veh.r)^2");
% reported output torque
blk(s, "Math Operations/Product", "lock_ratio_a", "Inputs", "3");
blk(s, "Math Operations/Gain", "report_inertia", "Gain", "P.gb.J_report*P.veh.fd/P.veh.r");
blk(s, "Math Operations/Sum", "Te_report", "Inputs", "+-");
blk(s, "Math Operations/Product", "k", "Inputs", "2");
blk(s, "Math Operations/Product", "To_report", "Inputs", "3");
% free / slipping engine speed
blk(s, "Math Operations/Sum", "T_engine_load", "Inputs", "+-");
blk(s, "Sources/Constant", "launch_w", "Value", "P.gb.launch_rpm*2*pi/60");
blk(s, "Math Operations/MinMax", "w_cap", "Function", "max", "Inputs", "2");
blk(s, "Math Operations/Sum", "over_cap", "Inputs", "+-");
blk(s, "Discontinuities/Saturation", "over_pos", "UpperLimit", "inf", "LowerLimit", "0");
blk(s, "Math Operations/Gain", "slip_gain", "Gain", "P.gb.slip_gain");
cmp(s, "slipping", "<", "0.99");
blk(s, "Logic and Bit Operations/Logical Operator", "slip_in_gear", "Operator", "AND");
blk(s, "Math Operations/Product", "launch_hold", "Inputs", "2");
blk(s, "Math Operations/Sum", "T_engine_net", "Inputs", "+-");
blk(s, "Math Operations/Gain", "inv_J_free", "Gain", "1/P.eng.J_free");
% While locked, the free-engine state tracks ratio*output speed (time constant
% P.gb.track_tau) so it starts from the right speed when the clutch opens.
blk(s, "Math Operations/Sum", "track_err", "Inputs", "+-");
blk(s, "Math Operations/Gain", "track_gain", "Gain", "1/P.gb.track_tau");
blk(s, "Signal Routing/Switch", "dw_select", "Criteria", "u2 > Threshold", "Threshold", "0.5");
blk(s, "Continuous/Integrator", "w_engine", "InitialCondition", "P.init.we0", ...
    "LimitOutput", "on", "UpperSaturationLimit", "P.eng.max_rpm*2*pi/60", "LowerSaturationLimit", "0");
blk(s, "Signal Routing/Switch", "w_select", "Criteria", "u2 > Threshold", "Threshold", "0.5");

ln(s, "v/1", "w_out/1"); ln(s, "ratio/1", "ratio_w_out/1"); ln(s, "w_out/1", "ratio_w_out/2");
ln(s, "ratio/1", "in_gear/1"); ln(s, "clutch/1", "clutch_closed/1"); ln(s, "ratio_w_out/1", "above_lock_min/1");
ln(s, "in_gear/1", "lock/1"); ln(s, "clutch_closed/1", "lock/2"); ln(s, "above_lock_min/1", "lock/3");
ln(s, "one/1", "one_minus_c/1"); ln(s, "clutch/1", "one_minus_c/2");
ln(s, "w_engine/1", "slip/1"); ln(s, "ratio_w_out/1", "slip/2");
ln(s, "one_minus_c/1", "one_minus_c_sat/1"); ln(s, "one_minus_c_sat/1", "engagement/1"); ln(s, "clutch_exp/1", "engagement/2");
ln(s, "engagement/1", "engaged_slip/1"); ln(s, "slip/1", "engaged_slip/2"); ln(s, "engaged_slip/1", "K_slip/1");
ln(s, "engagement/1", "capacity/1"); ln(s, "capacity/1", "neg_capacity/1");
ln(s, "capacity/1", "T_clutch/1"); ln(s, "K_slip/1", "T_clutch/2"); ln(s, "neg_capacity/1", "T_clutch/3");
ln(s, "T_clutch/1", "T_clutch_gear/1"); ln(s, "in_gear/1", "T_clutch_gear/2");
ln(s, "Te/1", "T_drive/1"); ln(s, "lock/1", "T_drive/2"); ln(s, "T_clutch_gear/1", "T_drive/3");
ln(s, "ratio/1", "eta/1");
ln(s, "T_drive/1", "driving/1"); ln(s, "eta_drive/1", "eta_select/1"); ln(s, "driving/1", "eta_select/2"); ln(s, "eta_coast_rel/1", "eta_select/3");
ln(s, "eta/1", "drive_out/1"); ln(s, "T_drive/1", "drive_out/2"); ln(s, "eta_select/1", "drive_out/3"); ln(s, "drive_out/1", "to_force/1");
ln(s, "to_force/1", "F_eng/1");
ln(s, "lock/1", "lock_ratio2/1"); ln(s, "eta/1", "lock_ratio2/2"); ln(s, "ratio/1", "lock_ratio2/3");
ln(s, "lock_ratio2/1", "reflected_mass/1"); ln(s, "reflected_mass/1", "m_rot/1");
ln(s, "lock/1", "lock_ratio_a/1"); ln(s, "ratio/1", "lock_ratio_a/2"); ln(s, "a_body/1", "lock_ratio_a/3");
ln(s, "lock_ratio_a/1", "report_inertia/1");
ln(s, "Te/1", "Te_report/1"); ln(s, "report_inertia/1", "Te_report/2");
ln(s, "one_minus_c/1", "k/1"); ln(s, "in_gear/1", "k/2");
ln(s, "eta/1", "To_report/1"); ln(s, "k/1", "To_report/2"); ln(s, "Te_report/1", "To_report/3");
ln(s, "To_report/1", "To_eng/1");
ln(s, "Te/1", "T_engine_load/1"); ln(s, "T_clutch_gear/1", "T_engine_load/2");
ln(s, "ratio_w_out/1", "w_cap/1"); ln(s, "launch_w/1", "w_cap/2");
ln(s, "w_engine/1", "over_cap/1"); ln(s, "w_cap/1", "over_cap/2"); ln(s, "over_cap/1", "over_pos/1");
ln(s, "over_pos/1", "slip_gain/1");
ln(s, "clutch/1", "slipping/1"); ln(s, "slipping/1", "slip_in_gear/1"); ln(s, "in_gear/1", "slip_in_gear/2");
ln(s, "slip_gain/1", "launch_hold/1"); ln(s, "slip_in_gear/1", "launch_hold/2");
ln(s, "T_engine_load/1", "T_engine_net/1"); ln(s, "launch_hold/1", "T_engine_net/2");
ln(s, "T_engine_net/1", "inv_J_free/1");
ln(s, "ratio_w_out/1", "track_err/1"); ln(s, "w_engine/1", "track_err/2"); ln(s, "track_err/1", "track_gain/1");
ln(s, "track_gain/1", "dw_select/1"); ln(s, "lock/1", "dw_select/2"); ln(s, "inv_J_free/1", "dw_select/3");
ln(s, "dw_select/1", "w_engine/1");
ln(s, "ratio_w_out/1", "w_select/1"); ln(s, "lock/1", "w_select/2"); ln(s, "w_engine/1", "w_select/3");
ln(s, "w_select/1", "w_e/1"); ln(s, "lock/1", "locked/1");
end

function build_retarder(s)
% Retarder: torque at the output shaft from a (level, output shaft speed) table,
% with a short first-order lag.
blk(s, "Math Operations/Gain", "w_out", "Gain", "P.veh.fd/P.veh.r");
lut2(s, "retarder_map", "P.ret.level_bp", "P.ret.w_bp", "P.ret.table");
blk(s, "Math Operations/Sum", "lag_err", "Inputs", "+-");
blk(s, "Math Operations/Gain", "inv_tau", "Gain", "1/P.ret.tau");
blk(s, "Continuous/Integrator", "T_state", "InitialCondition", "P.init.ret0");
blk(s, "Math Operations/Gain", "to_force", "Gain", "P.veh.fd/P.veh.r");
ln(s, "v/1", "w_out/1"); ln(s, "ret_level/1", "retarder_map/1"); ln(s, "w_out/1", "retarder_map/2");
ln(s, "retarder_map/1", "lag_err/1"); ln(s, "T_state/1", "lag_err/2"); ln(s, "lag_err/1", "inv_tau/1");
ln(s, "inv_tau/1", "T_state/1"); ln(s, "T_state/1", "T_ret/1"); ln(s, "T_state/1", "to_force/1");
ln(s, "to_force/1", "F_ret/1");
end

function build_brake(s)
% Service brake: truck wheel torque = input * gain(temperature, wheel speed),
% optional pressure lag (P.brk.tau, bypassed below 5 ms), plus the trailer brake
% force table when P.veh.trailer = 1, limited to P.brk.mu_max*m*g (tyre friction).
blk(s, "Math Operations/Gain", "w_wheel", "Gain", "1/P.veh.r");
lut2(s, "brake_gain", "P.brk.temp_bp", "P.brk.w_bp", "P.brk.gain_table");
blk(s, "Math Operations/Product", "T_cmd", "Inputs", "2");
blk(s, "Math Operations/Sum", "lag_err", "Inputs", "+-");
blk(s, "Math Operations/Gain", "inv_tau", "Gain", "1/max(P.brk.tau,0.01)");
blk(s, "Continuous/Integrator", "T_lag", "InitialCondition", "0");
blk(s, "Sources/Constant", "use_lag", "Value", "double(P.brk.tau > 0.005)");
blk(s, "Signal Routing/Switch", "lag_select", "Criteria", "u2 > Threshold", "Threshold", "0.5");
blk(s, "Math Operations/Gain", "truck_force", "Gain", "1/P.veh.r");
lut1(s, "trailer_brake", "P.brk.trailer_bp", "P.brk.trailer_force");
blk(s, "Math Operations/Gain", "trailer_on", "Gain", "P.veh.trailer");
blk(s, "Math Operations/Sum", "F_total", "Inputs", "++");
blk(s, "Sources/Constant", "friction_limit", "Value", "P.brk.mu_max*P.veh.m*P.veh.g");
blk(s, "Math Operations/MinMax", "F_limited", "Function", "min", "Inputs", "2");
ln(s, "v/1", "w_wheel/1"); ln(s, "brake_temp/1", "brake_gain/1"); ln(s, "w_wheel/1", "brake_gain/2");
ln(s, "brk/1", "T_cmd/1"); ln(s, "brake_gain/1", "T_cmd/2");
ln(s, "T_cmd/1", "lag_err/1"); ln(s, "T_lag/1", "lag_err/2"); ln(s, "lag_err/1", "inv_tau/1");
ln(s, "inv_tau/1", "T_lag/1");
ln(s, "T_lag/1", "lag_select/1"); ln(s, "use_lag/1", "lag_select/2"); ln(s, "T_cmd/1", "lag_select/3");
ln(s, "lag_select/1", "truck_force/1"); ln(s, "lag_select/1", "Tb_truck/1");
ln(s, "brk/1", "trailer_brake/1"); ln(s, "trailer_brake/1", "trailer_on/1");
ln(s, "truck_force/1", "F_total/1"); ln(s, "trailer_on/1", "F_total/2");
ln(s, "F_total/1", "F_limited/1"); ln(s, "friction_limit/1", "F_limited/2"); ln(s, "F_limited/1", "F_brake/1");
end

function build_vehicle(s)
% Vehicle: a = (F_eng + F_ret - F_brake - F_res) / (m + m_rot), passed through a
% first-order chassis lag (P.veh.tau_body, bypassed below 5 ms) and integrated
% to speed (held at >= 0).
blk(s, "Math Operations/Product", "v2", "Inputs", "2");
blk(s, "Math Operations/Gain", "aero", "Gain", "P.veh.c_air");
blk(s, "Math Operations/Gain", "v_scale", "Gain", "10");
blk(s, "Discontinuities/Saturation", "moving", "UpperLimit", "1", "LowerLimit", "0");
blk(s, "Math Operations/Gain", "rolling", "Gain", "P.veh.crr*P.veh.m*P.veh.g");
blk(s, "Math Operations/Gain", "grade_force", "Gain", "P.veh.m*P.veh.g");
blk(s, "Math Operations/Sum", "F_res_sum", "Inputs", "+++");
blk(s, "Math Operations/Sum", "F_net", "Inputs", "++--");
blk(s, "Math Operations/Bias", "m_total", "Bias", "P.veh.m");
blk(s, "Math Operations/Product", "a_wheel", "Inputs", "*/");
blk(s, "Math Operations/Sum", "lag_err", "Inputs", "+-");
blk(s, "Math Operations/Gain", "inv_tau", "Gain", "1/max(P.veh.tau_body,0.01)");
blk(s, "Continuous/Integrator", "a_state", "InitialCondition", "P.init.a0");
blk(s, "Sources/Constant", "use_lag", "Value", "double(P.veh.tau_body > 0.005)");
blk(s, "Signal Routing/Switch", "lag_select", "Criteria", "u2 > Threshold", "Threshold", "0.5");
blk(s, "Continuous/Integrator", "speed", "InitialCondition", "P.init.v0", "LimitOutput", "on", ...
    "LowerSaturationLimit", "0", "UpperSaturationLimit", "inf");
ln(s, "speed/1", "v2/1"); ln(s, "speed/1", "v2/2"); ln(s, "v2/1", "aero/1");
ln(s, "speed/1", "v_scale/1"); ln(s, "v_scale/1", "moving/1"); ln(s, "moving/1", "rolling/1");
ln(s, "grade/1", "grade_force/1");
ln(s, "aero/1", "F_res_sum/1"); ln(s, "rolling/1", "F_res_sum/2"); ln(s, "grade_force/1", "F_res_sum/3");
ln(s, "F_eng/1", "F_net/1"); ln(s, "F_ret/1", "F_net/2"); ln(s, "F_brake/1", "F_net/3"); ln(s, "F_res_sum/1", "F_net/4");
ln(s, "m_rot/1", "m_total/1");
ln(s, "F_net/1", "a_wheel/1"); ln(s, "m_total/1", "a_wheel/2");
ln(s, "a_wheel/1", "lag_err/1"); ln(s, "a_state/1", "lag_err/2"); ln(s, "lag_err/1", "inv_tau/1");
ln(s, "inv_tau/1", "a_state/1");
ln(s, "a_state/1", "lag_select/1"); ln(s, "use_lag/1", "lag_select/2"); ln(s, "a_wheel/1", "lag_select/3");
ln(s, "lag_select/1", "speed/1"); ln(s, "speed/1", "v/1"); ln(s, "lag_select/1", "a/1");
ln(s, "F_res_sum/1", "F_res/1");
end

% =====================================================================
function path = subsystem(model, name, ins, outs)
% Empty subsystem with named ports.
path = model + "/" + name;
add_block("simulink/Ports & Subsystems/Subsystem", path);
delete_line(path, "In1/1", "Out1/1");
delete_block(path + "/In1"); delete_block(path + "/Out1");
for i = 1:numel(ins)
    add_block("simulink/Sources/In1", path + "/" + ins(i), "Port", num2str(i));
end
for i = 1:numel(outs)
    add_block("simulink/Sinks/Out1", path + "/" + outs(i), "Port", num2str(i));
end
end

function blk(sys, lib, name, varargin)
add_block("simulink/" + lib, sys + "/" + name, varargin{:});
end

function cmp(sys, name, op, value)
add_block("simulink/Logic and Bit Operations/Compare To Constant", sys + "/" + name, ...
    "relop", op, "const", value, "OutDataTypeStr", "boolean");
end

function lut1(sys, name, bp, tab)
add_block("simulink/Lookup Tables/1-D Lookup Table", sys + "/" + name, ...
    "BreakpointsForDimension1", bp, "Table", tab, "ExtrapMethod", "Clip");
end

function lut2(sys, name, bp1, bp2, tab)
add_block("simulink/Lookup Tables/2-D Lookup Table", sys + "/" + name, ...
    "BreakpointsForDimension1", bp1, "BreakpointsForDimension2", bp2, "Table", tab, ...
    "ExtrapMethod", "Clip");
end

function ln(sys, from, to)
add_line(sys, from, to, "autorouting", "smart");
end
