function model = build_acc_plant()
%BUILD_ACC_PLANT Create acc_plant.slx: longitudinal plant + the game's automatic gearbox.
%   build_acc_plant() rebuilds acc_plant.slx in this folder from
%     - apps/sysid/matlab/longitudinal/build_longitudinal_model.m (built in memory with
%       Save=false and copied into the subsystem "Longitudinal"; that folder is not changed)
%     - a Stateflow chart "AutoGearbox" (states NEUTRAL, LAUNCH_*, DRIVE, SHIFT_*), the
%       shift model fitted in acc_plant_params.m
%     - "IdleCreep" (MATLAB Function): the automatic's creep force at low speed with the
%       throttle closed while still rolling in gear (P.acc.creep_*), added through the grade
%       input as -F/(m*g)
%   Parameters live in the model workspace (MATLAB code: P = acc_plant_params() and the
%   AT_* scalars the chart reads), so the model runs from this folder without setup.
%
%   Root inputs : 1 thr_cmd [0,1]  2 brk_cmd [0,1]  3 ret_step {0..5}  4 eb_cmd (0/1)
%                 5 brake_temp (deg C)  6 grade (sin theta)
%   Root outputs: 1 v (m/s) 2 a (m/s^2) 3 rpm 4 gear (SDK, 0 neutral) 5 Te (Nm)
%                 6 clutch (0 engaged..1 open) 7 thr_app (applied throttle) 8 F_brake (N)
here = fileparts(mfilename("fullpath"));
P = acc_plant_params();                     % also puts the longitudinal folder on the path
model = 'acc_plant';
tmp = 'acc_lon_tmp';
if bdIsLoaded(model), close_system(model, 0); end
if bdIsLoaded(tmp), close_system(tmp, 0); end
file = fullfile(here, model + ".slx");
if isfile(file), delete(file); end

new_system(model);
load_system(model);
set_param(model, "Solver", "ode3", "FixedStep", num2str(P.sim.step), "StopTime", "60", ...
    "SaveOutput", "on", "OutputSaveName", "yout", "SaveFormat", "Dataset", ...
    "SaveTime", "on", "SaveState", "off", "LoadExternalInput", "off", ...
    "AlgebraicLoopMsg", "error", "ReturnWorkspaceOutputs", "on", ...
    "SignalLogging", "on", "SignalLoggingName", "logsout");
set_param(model, "Description", ['ETS2 Scania S 6x2 + empty box trailer, longitudinal plant with the ' ...
    'game''s real automatic gearbox. Built by build_acc_plant.m; parameters in acc_plant_params.m.']);

% model workspace: P and the chart parameters
hws = get_param(model, "ModelWorkspace");
hws.DataSource = "MATLAB Code";
hws.MATLABCode = workspace_code(P);
hws.reload();

% ---------------- longitudinal plant, copied from the sysid model ----------------------
build_longitudinal_model(Name=tmp, Save=false);
lon = [model '/Longitudinal'];
add_block("simulink/Ports & Subsystems/Subsystem", lon);
delete_line(lon, "In1/1", "Out1/1"); delete_block(lon + "/In1"); delete_block(lon + "/Out1");
Simulink.BlockDiagram.copyContentsToSubsystem(tmp, lon);
close_system(tmp, 0);
set_param(lon, "Position", [520 100 700 420]);
% Service brake force x measured/model ratio (input, speed): see P.acc.brk_scale
sb = [lon '/Service_Brake'];
delete_line(sb, "F_limited/1", "F_brake/1");
add_block("simulink/Lookup Tables/2-D Lookup Table", [sb '/acc_brake_scale'], ...
    "BreakpointsForDimension1", "P.acc.brk_b", "BreakpointsForDimension2", "P.acc.brk_v", ...
    "Table", "P.acc.brk_scale.'", "ExtrapMethod", "Clip");
add_block("simulink/Math Operations/Product", [sb '/acc_scaled'], "Inputs", "2");
ln(sb, "brk/1", "acc_brake_scale/1"); ln(sb, "v/1", "acc_brake_scale/2");
ln(sb, "F_limited/1", "acc_scaled/1"); ln(sb, "acc_brake_scale/1", "acc_scaled/2");
ln(sb, "acc_scaled/1", "F_brake/1");
Simulink.BlockDiagram.arrangeSystem(sb);

% ---------------- root ports -------------------------------------------------------------
ins = ["thr_cmd", "brk_cmd", "ret_step", "eb_cmd", "brake_temp", "grade"];
for i = 1:numel(ins)
    add_block("simulink/Sources/In1", [model '/' char(ins(i))], "Port", num2str(i), ...
        "Position", [30 60+60*i 60 74+60*i]);
end
outs = ["v", "a", "rpm", "gear", "Te", "clutch", "thr_app", "F_brake"];
for i = 1:numel(outs)
    add_block("simulink/Sinks/Out1", [model '/' char(outs(i))], "Port", num2str(i), ...
        "Position", [900 40+50*i 930 54+50*i]);
end
blk(model, "Discontinuities/Saturation", "thr_sat", "UpperLimit", "1", "LowerLimit", "0");
blk(model, "Discontinuities/Saturation", "brk_sat", "UpperLimit", "1", "LowerLimit", "0");
blk(model, "Discontinuities/Saturation", "ret_sat", "UpperLimit", "5", "LowerLimit", "0");
blk(model, "Math Operations/Rounding Function", "ret_round", "Operator", "round");
blk(model, "Math Operations/Gain", "ret_level", "Gain", "0.2");   % def auxiliary_brakes 0.2 .. 1.0

% ---------------- automatic gearbox chart -------------------------------------------------
build_gearbox_chart(model, P);

ln(model, "thr_cmd/1", "thr_sat/1"); ln(model, "brk_cmd/1", "brk_sat/1");
ln(model, "ret_step/1", "ret_sat/1"); ln(model, "ret_sat/1", "ret_round/1"); ln(model, "ret_round/1", "ret_level/1");
% chart inputs: v a thr brk ret eb rpm
% the chart reads the plant one chart sample late (it is direct feedthrough)
fb = ["v_fb", "a_fb", "rpm_fb"]; fbSrc = [1 2 5]; fbDst = [1 2 7];   % a: plant (lagged) acceleration
fbIc = ["P.init.v0", "P.init.a0", "P.init.we0*30/pi"];
for i = 1:numel(fb)
    blk(model, "Discrete/Unit Delay", fb(i), "SampleTime", num2str(P.at.dt), "InitialCondition", fbIc(i));
    ln(model, sprintf("Longitudinal/%d", fbSrc(i)), fb(i) + "/1"); ln(model, fb(i) + "/1", sprintf("AutoGearbox/%d", fbDst(i)));
end
ln(model, "thr_sat/1", "AutoGearbox/3"); ln(model, "brk_sat/1", "AutoGearbox/4");
ln(model, "ret_round/1", "AutoGearbox/5"); ln(model, "eb_cmd/1", "AutoGearbox/6");
% plant inputs: thr brk ratio clutch ret_level eb_flag brake_temp grade
ln(model, "AutoGearbox/4", "Longitudinal/1"); ln(model, "brk_sat/1", "Longitudinal/2");
ln(model, "AutoGearbox/2", "Longitudinal/3"); ln(model, "AutoGearbox/3", "Longitudinal/4");
ln(model, "ret_level/1", "Longitudinal/5"); ln(model, "eb_cmd/1", "Longitudinal/6");
ln(model, "brake_temp/1", "Longitudinal/7");
% idle creep of the automatic, as an equivalent grade
add_block("simulink/User-Defined Functions/MATLAB Function", [model '/IdleCreep']);
ch = sfroot().find("-isa", "Stateflow.EMChart", "Path", [model '/IdleCreep']);
ch.Script = char(strjoin([
"function F = IdleCreep(v, thr, gear, P)"
"% creep force (N) of the automatic: after rolling in gear (v > creep_on_v) with the throttle"
"% closed, the clutch keeps slipping at idle and pushes until the truck stands still"
"% (v < creep_off_v: the clutch opens, docs/31 정차 유지: no creep from standstill)"
"persistent rolling"
"if isempty(rolling), rolling = false; end"
"if gear >= 1 && v > P.acc.creep_on_v, rolling = true; end"
"if v < P.acc.creep_off_v, rolling = false; end"
"F = 0;"
"if rolling && thr < 0.02"
"    F = P.acc.creep_force * min(max((P.acc.creep_v_zero - v) / (P.acc.creep_v_zero - P.acc.creep_v_full), 0), 1);"
"end"], newline));
d = ch.find("-isa", "Stateflow.Data", "Name", "P"); d.Scope = "Parameter"; d.Tunable = false;
blk(model, "Math Operations/Gain", "creep_grade", "Gain", "-1/(P.veh.m*P.veh.g)");
blk(model, "Math Operations/Sum", "grade_total", "Inputs", "++");
ln(model, "v_fb/1", "IdleCreep/1"); ln(model, "AutoGearbox/4", "IdleCreep/2"); ln(model, "AutoGearbox/1", "IdleCreep/3");
ln(model, "IdleCreep/1", "creep_grade/1");
ln(model, "grade/1", "grade_total/1"); ln(model, "creep_grade/1", "grade_total/2");
ln(model, "grade_total/1", "Longitudinal/8");
% outputs
% at standstill the plant's lagged force/mass stays negative (brake held); the game
% reports 0 there, so the acceleration output is zeroed while stopped and not pushed
cmp(model, "moving", ">", "0.02"); cmp(model, "pushing", ">", "0");
blk(model, "Logic and Bit Operations/Logical Operator", "a_valid", "Operator", "OR");
blk(model, "Signal Attributes/Data Type Conversion", "a_valid_d", "OutDataTypeStr", "double");
blk(model, "Math Operations/Product", "a_zeroed", "Inputs", "2");
ln(model, "Longitudinal/1", "moving/1"); ln(model, "Longitudinal/2", "pushing/1");
ln(model, "moving/1", "a_valid/1"); ln(model, "pushing/1", "a_valid/2"); ln(model, "a_valid/1", "a_valid_d/1");
ln(model, "Longitudinal/2", "a_zeroed/1"); ln(model, "a_valid_d/1", "a_zeroed/2");
ln(model, "Longitudinal/1", "v/1"); ln(model, "a_zeroed/1", "a/1");
ln(model, "Longitudinal/5", "rpm/1"); ln(model, "AutoGearbox/1", "gear/1");
ln(model, "Longitudinal/3", "Te/1"); ln(model, "AutoGearbox/3", "clutch/1");
ln(model, "AutoGearbox/4", "thr_app/1"); ln(model, "Longitudinal/6", "F_brake/1");
for j = 7:8
    t = sprintf("unused_%d", j);
    blk(model, "Sinks/Terminator", t); ln(model, sprintf("Longitudinal/%d", j), t + "/1");
end
Simulink.BlockDiagram.arrangeSystem(model);
save_system(model, file);
end

% =====================================================================
function code = workspace_code(P)
% Model workspace code: P and one AT_<field> variable per gearbox parameter
f = fieldnames(P.at);
lines = ["% Generated by build_acc_plant.m (do not edit; edit acc_plant_params.m)"
         "P = acc_plant_params();"
         "AT_init_gear = 0;   % SDK gear at t = 0 (0: start in neutral at standstill)"];
for i = 1:numel(f)
    lines(end+1) = sprintf("AT_%s = P.at.%s;", f{i}, f{i}); %#ok<AGROW>
end
code = strjoin(lines, newline);
end

function build_gearbox_chart(model, P)
path = [model '/AutoGearbox'];
add_block("sflib/Chart", path, "Position", [300 120 420 330]);
rt = sfroot;
ch = rt.find("-isa", "Stateflow.Chart", "Path", path);
ch.ActionLanguage = "MATLAB";
ch.ChartUpdate = "DISCRETE";
ch.SampleTime = num2str(P.at.dt);
ch.Description = ['Game automatic gearbox (G33CM-R, real automatic). Fitted to acc-20261020-192243, ' ...
    'see acc_plant_params.m. Upshift above up_rpm(thr, gear) to the highest gear that keeps >= 899/910/935 rpm ' ...
    '(gear skipping), predictive downshift, retarder-5/engine-brake downshift to <= 2,500 rpm, ' ...
    'neutral near standstill, launch with a clutch capacity ramp. Shift: 0.5 s open, 0.233 s close.'];
% data
inputs = ["v", "a", "thr", "brk", "ret", "eb", "rpm"];
for i = 1:numel(inputs), data(ch, inputs(i), "Input"); end
outputs = ["gear", "ratio", "clutch", "thr_app"];
for i = 1:numel(outputs), data(ch, outputs(i), "Output", "0"); end
data(ch, "target", "Local", "0");
data(ch, "down", "Local", "0");
data(ch, "tl", "Local", "0");
f = fieldnames(P.at);
for i = 1:numel(f), data(ch, "AT_" + f{i}, "Parameter"); end
data(ch, "AT_init_gear", "Parameter");

% states
W = 170; H = 90;
sN  = state(ch, ["NEUTRAL", "entry: gear = 0; ratio = 0; clutch = 0; target = 0;", "during: thr_app = 0;"], [40 60 W H]);
sLE = state(ch, ["LAUNCH_ENGAGE", "entry: clutch = 1; thr_app = 0; gear = AT_launch_gear; target = gear;"], [300 60 W H]);
sLS = state(ch, ["LAUNCH_SLIP", "entry: ratio = AT_ratio(gear); tl = 0;", ...
    "during: tl = tl + AT_dt; thr_app = thr; clutch = launch_clutch();"], [560 60 W+40 H]);
sLC = state(ch, ["LAUNCH_CLOSE", "during: thr_app = thr; clutch = max(0, clutch - AT_dt/AT_launch_t_close);"], [560 240 W+40 H]);
sD  = state(ch, ["DRIVE", "entry: clutch = 0;", ...
    "during: thr_app = thr; target = select_gear(gear, ret >= 4.5 || eb > 0.5);"], [300 240 W+20 H+10]);
sSO = state(ch, ["SHIFT_OPEN", "entry: down = double(target < gear);", ...
    "during: clutch = min(1, clutch + AT_dt/AT_t_ramp_open);", ...
    "if down > 0.5 && clutch >= 0.99, thr_app = AT_blip; else, thr_app = max(0, thr_app - AT_dt/0.1); end"], [300 430 W+60 H+10]);
sSC = state(ch, ["SHIFT_CLOSE", "entry: gear = target; ratio = AT_ratio(gear);", ...
    "during: clutch = max(0, clutch - AT_dt/AT_t_close); thr_app = 0;"], [620 430 W+20 H]);
sTR = state(ch, ["THR_RETURN", "entry: clutch = 0;", ...
    "during: thr_app = min(thr, thr_app + max(thr, 0.05)*AT_dt/AT_t_thr);"], [620 600 W+20 H]);

% default transition through a junction: start in DRIVE when AT_init_gear > 0 (replays)
j = Stateflow.Junction(ch); j.Position.Center = [20 300]; j.Position.Radius = 7;
t = Stateflow.Transition(ch); t.Destination = j; t.SourceEndpoint = [20 260]; t.MidPoint = [20 280];
trans(ch, j, sD, "[AT_init_gear > 0]{gear = AT_init_gear; target = gear; ratio = AT_ratio(gear); clutch = 0;}", 1);
trans(ch, j, sN, "", 2);

trans(ch, sN, sLE, "[thr > AT_creep_thr && v < 2]", 1);
trans(ch, sN, sSC, "[v >= 2 && (thr > AT_creep_thr || (brk < 0.05 && after(0.5, sec)))]{target = engage_gear(); clutch = 1;}", 2);
trans(ch, sLE, sN, "[thr <= AT_creep_thr]", 1);
trans(ch, sLE, sLS, "[after(AT_launch_t_engage, sec)]", 2);
trans(ch, sLS, sN, "[thr <= AT_creep_thr && v < 1]", 1);
trans(ch, sLS, sLC, "[thr <= AT_creep_thr || AT_ratio(gear)*v*AT_kv >= AT_launch_sync*rpm]", 2);
trans(ch, sLC, sD, "[clutch <= 0]", 1);
trans(ch, sD, sN, "[neutral_cond(gear)]", 1);
trans(ch, sD, sLS, "[thr > AT_creep_thr && v*AT_kv*AT_ratio(gear) < AT_neutral_rpm]", 2);
trans(ch, sD, sSO, "[target >= 1 && target ~= gear && after(AT_dwell, sec)]", 3);
trans(ch, sSO, sN, "[neutral_cond(gear)]", 1);
trans(ch, sSO, sSC, "[after(AT_t_open, sec)]", 2);
trans(ch, sSC, sTR, "[after(AT_t_close, sec)]", 1);
trans(ch, sTR, sD, "[after(AT_t_thr, sec)]", 1);

% MATLAB functions
fn(ch, "tg = select_gear(g, bm)", [900 60 220 60], strjoin([
"function tg = select_gear(g, bm)"
"% Target SDK gear from the chart inputs v, a, thr. bm: retarder step 5 or engine brake."
"tg = g;"
"if g < 1, return; end"
"r = AT_ratio;"
"vp = max(0, v + min(a, 0)*AT_t_pred);"
"rpm_now = v*AT_kv*r(g);"
"rpm_pred = vp*AT_kv*r(g);"
"if bm"
"    if rpm_now < AT_eb_trig"
"        for k = AT_min_gear:(g - 1)"
"            if v*AT_kv*r(k) <= AT_eb_max"
"                tg = k; return;"
"            end"
"        end"
"    end"
"    return;"
"end"
"th = min(max(thr, 0), 1);"
"dn = AT_dn0 + th^3*AT_dn0_thr - (AT_dn_slope + th^3*AT_dn_slope_thr)*(12 - g);"
"if rpm_pred < dn && g > AT_min_gear"
"    tg = AT_min_gear;"
"    for k = (g - 1):-1:AT_min_gear"
"        if vp*AT_kv*r(k) >= AT_dn_target"
"            tg = k; return;"
"        end"
"    end"
"    return;"
"end"
"up = interp1(AT_up_thr, AT_up_rpm, th) - interp1(AT_up_thr, AT_up_slope, th)*(g - 4);"
"if rpm_now > up && a > -1.0 && g < AT_top_gear"
"    % the target follows from the threshold speed, not from the sampled one (the game's"
"    % choice is within a few rpm of the limits, see acc_plant_params.m)"
"    ru = up;"
"    if rpm_now > up + 30, ru = rpm_now; end"
"    tg = g + 1;"
"    for k = AT_top_gear:-1:(g + 1)"
"        lim = AT_up_min(min(k, numel(AT_up_min)));"
"        if ru*r(k)/r(g) >= lim"
"            tg = k; return;"
"        end"
"    end"
"end"], newline));
fn(ch, "y = neutral_cond(g)", [900 140 220 60], strjoin([
"function y = neutral_cond(g)"
"% The clutch opens (gear 0) near standstill or early under strong braking."
"y = false;"
"if g < 1 || thr > 0.05, return; end"
"rpm_now = v*AT_kv*AT_ratio(g);"
"vp = max(0, v + min(a, 0)*AT_t_pred);"
"if rpm_now < AT_neutral_rpm"
"    y = true;"
"elseif brk >= AT_neutral_brk && vp*AT_kv*AT_ratio(g) < AT_neutral_brk_rpm"
"    y = true;"
"end"], newline));
fn(ch, "c = launch_clutch()", [900 220 220 60], strjoin([
"function c = launch_clutch()"
"% Clutch command while slipping: capacity ramps to cap_max(thr) in cap_t(thr) seconds (fit)."
"th = min(max(thr, 0), 1);"
"cap = interp1(AT_launch_thr, AT_launch_cap_max, th)*min(1, tl/interp1(AT_launch_thr, AT_launch_cap_t, th));"
"e = min(1, cap/AT_C_clutch);"
"c = 1 - e^(1/AT_clutch_exp);"], newline));
fn(ch, "g = engage_gear()", [900 300 220 60], strjoin([
"function g = engage_gear()"
"% Gear to engage when rolling in neutral: the highest one with >= 1,100 rpm."
"g = AT_min_gear;"
"for k = AT_top_gear:-1:AT_min_gear"
"    if v*AT_kv*AT_ratio(k) >= 1100"
"        g = k; return;"
"    end"
"end"], newline));
end

function d = data(ch, name, scope, init)
d = Stateflow.Data(ch);
d.Name = char(name);
d.Scope = char(scope);
if nargin > 3, d.Props.InitialValue = char(init); end
end

function s = state(ch, lines, pos)
s = Stateflow.State(ch);
s.LabelString = char(strjoin(lines, newline));
s.Position = pos;
end

function t = trans(ch, src, dst, label, order)
t = Stateflow.Transition(ch);
t.Source = src; t.Destination = dst;
t.LabelString = char(label);
t.ExecutionOrder = order;
end

function f = fn(ch, label, pos, script)
f = Stateflow.EMFunction(ch);
f.LabelString = char(label);
f.Position = pos;
f.Script = char(script);
end

function blk(sys, lib, name, varargin)
add_block("simulink/" + lib, string(sys) + "/" + name, varargin{:});
end

function ln(sys, from, to)
add_line(sys, from, to, "autorouting", "smart");
end

function cmp(sys, name, op, value)
add_block("simulink/Logic and Bit Operations/Compare To Constant", string(sys) + "/" + name, ...
    "relop", op, "const", value, "OutDataTypeStr", "boolean");
end
