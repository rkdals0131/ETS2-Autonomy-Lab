function model = build_acc_closed_loop()
%BUILD_ACC_CLOSED_LOOP Create acc_closed_loop.slx: ACC_Controller + acc_plant + lead vehicle.
%   build_acc_closed_loop() rebuilds acc_closed_loop.slx in this folder. Both the controller
%   (ACC_Controller.slx, 0.05 s) and the plant (acc_plant.slx, ode3 5 ms) are Model blocks;
%   set the controller block's SimulationMode to "Software-in-the-loop (SIL)" to run the
%   generated code (verify_acc_codegen.m does this).
%   Scenario signals come from the base-workspace variables written by run_acc_scenarios.m:
%     scn_lead   [t  x_lead  v_lead  a_lead]      lead rear bumper position, speed, accel
%     scn_disc   [t  lead_valid enable set_speed time_gap resume]  (held, no interpolation)
%     scn_grade  [t  grade]  road grade sin(theta) into the plant (interpolated)
%     scn_mass_prior   mass_prior_kg input of the controller (0 = unknown)
%     scn_pitch_known  1: the controller gets pitch_rad = asin(grade), 0: pitch_rad = 0
%     scn_act_valid    1: actuation_valid with the plant's applied throttle and the delayed
%                      brake / retarder commands, 0: the estimator uses the controller's own
%   (acc_scenario_input.m sets all of them.) The plant mass is P.veh.m in the plant's model
%   workspace. The ego front bumper starts at x = 0; lead_gap = x_lead - x_ego.
here = fileparts(mfilename("fullpath"));
model = 'acc_closed_loop';
if bdIsLoaded(model), close_system(model, 0); end
for m = ["acc_plant", "ACC_Controller"]      % referenced models: use the saved versions
    if bdIsLoaded(m), close_system(m, 0); end
end
file = fullfile(here, model + ".slx");
if isfile(file), delete(file); end
P = acc_plant_params();
new_system(model);
load_system(model);
set_param(model, "Solver", "ode3", "FixedStep", num2str(P.sim.step), "StopTime", "60", ...
    "SaveOutput", "on", "OutputSaveName", "yout", "SaveFormat", "Dataset", "SaveTime", "on", ...
    "ReturnWorkspaceOutputs", "on", "AutoInsertRateTranBlk", "on", "SignalLogging", "off");
set_param(model, "Description", "ACC closed loop: ACC_Controller (Model block) + acc_plant (Model block) + scripted lead vehicle. Built by build_acc_closed_loop.m.");

% ---------------- scenario sources ----------------
add_block("simulink/Sources/From Workspace", [model '/lead'], "VariableName", "scn_lead", ...
    "Interpolate", "on", "OutputAfterFinalValue", "Holding final value", "SampleTime", "0");
add_block("simulink/Sources/From Workspace", [model '/driver'], "VariableName", "scn_disc", ...
    "Interpolate", "off", "OutputAfterFinalValue", "Holding final value", "SampleTime", "0.05");
add_block("simulink/Sources/From Workspace", [model '/road'], "VariableName", "scn_grade", ...
    "Interpolate", "on", "OutputAfterFinalValue", "Holding final value", "SampleTime", "0");
add_block("simulink/Signal Routing/Demux", [model '/lead_demux'], "Outputs", "3");
add_block("simulink/Signal Routing/Demux", [model '/driver_demux'], "Outputs", "5");
ln(model, "lead/1", "lead_demux/1"); ln(model, "driver/1", "driver_demux/1");

% ---------------- controller ----------------
add_block("simulink/Ports & Subsystems/Model", [model '/Controller'], "ModelName", "ACC_Controller", ...
    "Position", [500 100 700 520]);
% ---------------- plant ----------------
add_block("simulink/Ports & Subsystems/Model", [model '/Plant'], "ModelName", "acc_plant", ...
    "Position", [900 150 1080 450]);

% conversions into the controller types
conv(model, "enable_b", "boolean"); conv(model, "valid_b", "boolean"); conv(model, "resume_b", "boolean");
conv(model, "gear_i", "int32");
ln(model, "driver_demux/2", "enable_b/1"); ln(model, "driver_demux/1", "valid_b/1"); ln(model, "driver_demux/5", "resume_b/1");
% gap = x_lead - x_ego
add_block("simulink/Continuous/Integrator", [model '/x_ego'], "InitialCondition", "0");
add_block("simulink/Math Operations/Sum", [model '/gap'], "Inputs", "+-");
ln(model, "Plant/1", "x_ego/1"); ln(model, "lead_demux/1", "gap/1"); ln(model, "x_ego/1", "gap/2");
% controller inputs (contract order)
ln(model, "enable_b/1", "Controller/1");          % enable
ln(model, "driver_demux/3", "Controller/2");      % set_speed_mps
ln(model, "driver_demux/4", "Controller/3");      % time_gap_s
ln(model, "Plant/1", "Controller/4");             % ego_speed_mps
ln(model, "Plant/2", "Controller/5");             % ego_accel_mps2
ln(model, "valid_b/1", "Controller/6");           % lead_valid
ln(model, "gap/1", "Controller/7");               % lead_gap_m
ln(model, "lead_demux/2", "Controller/8");        % lead_speed_mps
ln(model, "lead_demux/3", "Controller/9");        % lead_accel_mps2
ln(model, "resume_b/1", "Controller/10");         % resume
ln(model, "Plant/3", "Controller/11");            % engine_rpm
ln(model, "Plant/4", "gear_i/1"); ln(model, "gear_i/1", "Controller/12");   % gear
% plant inputs: thr brk ret eb brake_temp grade
conv(model, "ret_d", "double"); conv(model, "eb_d", "double");
add_block("simulink/Sources/Constant", [model '/brake_temp'], "Value", "acc_plant_params().acc.brake_temp");
% command latency: game frame + one 50/60 Hz sample, about 35 ms (docs/31 조향 계통,
% "제어 루프 지연"); also breaks the algebraic loop between the two Model blocks
cmds = ["thr", "brk", "ret", "eb"];
for i = 1:4
    add_block("simulink/Discrete/Delay", model + "/lat_" + cmds(i), "DelayLength", "7", ...
        "SampleTime", num2str(P.sim.step), "InitialCondition", "0");
end
ln(model, "Controller/1", "lat_thr/1"); ln(model, "lat_thr/1", "Plant/1");
ln(model, "Controller/2", "lat_brk/1"); ln(model, "lat_brk/1", "Plant/2");
ln(model, "Controller/3", "ret_d/1"); ln(model, "ret_d/1", "lat_ret/1"); ln(model, "lat_ret/1", "Plant/3");
ln(model, "Controller/4", "eb_d/1"); ln(model, "eb_d/1", "lat_eb/1"); ln(model, "lat_eb/1", "Plant/4");
ln(model, "brake_temp/1", "Plant/5"); ln(model, "road/1", "Plant/6");
% mass-estimator inputs of the controller
add_block("simulink/Math Operations/Trigonometric Function", [model '/pitch_asin'], "Operator", "asin");
add_block("simulink/Math Operations/Gain", [model '/pitch_known'], "Gain", "scn_pitch_known");
add_block("simulink/Sources/Constant", [model '/mass_prior'], "Value", "scn_mass_prior");
add_block("simulink/Sources/Constant", [model '/act_valid'], "Value", "scn_act_valid", "OutDataTypeStr", "boolean");
conv(model, "ret_app_i", "int32");
ln(model, "road/1", "pitch_asin/1"); ln(model, "pitch_asin/1", "pitch_known/1");
ln(model, "pitch_known/1", "Controller/13");      % pitch_rad
ln(model, "mass_prior/1", "Controller/14");       % mass_prior_kg
ln(model, "act_valid/1", "Controller/15");        % actuation_valid
ln(model, "Plant/7", "Controller/16");            % throttle_applied (plant thr_app: shift cuts)
ln(model, "lat_brk/1", "Controller/17");          % brake_applied (delayed command)
ln(model, "lat_ret/1", "ret_app_i/1"); ln(model, "ret_app_i/1", "Controller/18");   % retarder_applied

% ---------------- logged outputs ----------------
names = ["v", "a", "gap", "v_lead", "a_lead", "lead_valid", "throttle", "brake", "retarder", ...
         "engine_brake", "a_des", "mode", "gear", "rpm", "clutch", "F_brake", "x_ego", "set_speed", ...
         "mass_est", "mass_sigma", "mass_valid"];
src = ["Plant/1", "Plant/2", "gap/1", "lead_demux/2", "lead_demux/3", "driver_demux/1", ...
       "Controller/1", "Controller/2", "Controller/3", "Controller/4", "Controller/5", "Controller/6", ...
       "Plant/4", "Plant/3", "Plant/6", "Plant/8", "x_ego/1", "driver_demux/3", ...
       "Controller/7", "Controller/8", "Controller/9"];
for i = 1:numel(names)
    add_block("simulink/Sinks/Out1", model + "/" + names(i) + "_out", "Port", num2str(i));
    if ismember(names(i), ["throttle", "brake", "retarder", "engine_brake", "a_des", "mode", "mass_est", "mass_sigma", "mass_valid"])
        c = names(i) + "_log";
        add_block("simulink/Signal Attributes/Data Type Conversion", model + "/" + c, "OutDataTypeStr", "double");
        ln(model, src(i), c + "/1"); ln(model, c + "/1", names(i) + "_out/1");
    else
        ln(model, src(i), names(i) + "_out/1");
    end
end
% the 18 controller inputs as seen by the controller (for the SIL / code comparison)
add_block("simulink/Signal Routing/Mux", model + "/ctrl_in_mux", "Inputs", "18");
cin = ["enable_b/1", "driver_demux/3", "driver_demux/4", "Plant/1", "Plant/2", "valid_b/1", ...
       "gap/1", "lead_demux/2", "lead_demux/3", "resume_b/1", "Plant/3", "gear_i/1", ...
       "pitch_known/1", "mass_prior/1", "act_valid/1", "Plant/7", "lat_brk/1", "ret_app_i/1"];
for i = 1:numel(cin)
    c = sprintf("cin_%d", i);
    add_block("simulink/Signal Attributes/Data Type Conversion", model + "/" + c, "OutDataTypeStr", "double");
    ln(model, cin(i), c + "/1"); ln(model, c + "/1", sprintf("ctrl_in_mux/%d", i));
end
add_block("simulink/Sinks/Out1", model + "/ctrl_in_out", "Port", num2str(numel(names) + 1));
ln(model, "ctrl_in_mux/1", "ctrl_in_out/1");
for k = 5
    add_block("simulink/Sinks/Terminator", sprintf("%s/plant_unused_%d", model, k));
    ln(model, sprintf("Plant/%d", k), sprintf("plant_unused_%d/1", k));
end
Simulink.BlockDiagram.arrangeSystem(model);
save_system(model, file);
end

function conv(model, name, type)
add_block("simulink/Signal Attributes/Data Type Conversion", model + "/" + name, "OutDataTypeStr", type);
end

function ln(sys, from, to)
add_line(sys, from, to, "autorouting", "smart");
end
