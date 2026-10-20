function V = verify_acc_codegen(opts)
%VERIFY_ACC_CODEGEN Compare the generated C++ (SIL) with the model on closed-loop scenarios.
%   V = verify_acc_codegen() runs each scenario in opts.Names on acc_closed_loop.slx with the
%   controller in Normal mode and records the 12 controller inputs at the controller samples.
%   It then replays those inputs into ACC_Controller.slx as a top model twice: Normal mode
%   and Software-in-the-loop (the ERT C++ code built by Embedded Coder, compiled with the
%   host compiler, with execution-time profiling) and compares the six outputs:
%     closed loop vs Normal replay  (the replay is faithful)
%     Normal replay vs SIL          (the generated code matches the model)
%   Results: results/codegen_verification.json.
arguments
    opts.Names (1,:) string = ["lead_brake_4", "stop_and_go_hold", "lead_emergency_stop"]
end
here = fileparts(mfilename("fullpath"));
cd(here);
model = 'ACC_Controller';
acc_controller_init();
cg = fullfile(here, "codegen");
if ~isfolder(cg), mkdir(cg); end
Simulink.fileGenControl("set", "CodeGenFolder", cg, "CacheFolder", fullfile(cg, "cache"), "createDir", true);
restore = onCleanup(@() Simulink.fileGenControl("reset"));
S = acc_scenarios();
outNames = ["throttle", "brake", "retarder_step", "engine_brake", "a_des_mps2", "mode"];
V.scenarios = struct([]);
for name = opts.Names
    s = S([S.name] == name);
    [~, ~, out] = closed_loop(s);
    t = (0:0.05:s.T).';
    cin = sample(out.yout{19}.Values, t);                 % 12 inputs, double
    ycl = zeros(numel(t), 6);
    srcIdx = [7 8 9 10 11 12];                             % closed-loop outports of the controller outputs
    for i = 1:6, ycl(:, i) = sample(out.yout{srcIdx(i)}.Values, t); end
    ds = input_dataset(cin, t);
    load_system(model);
    set_param(model, "SimulationMode", "normal");
    yN = run_top(model, ds, s.T, t, false);
    set_param(model, "SimulationMode", "software-in-the-loop (sil)");
    [yS, prof] = run_top(model, ds, s.T, t, true);
    set_param(model, "SimulationMode", "normal");
    r.name = name;
    r.samples = numel(t);
    r.max_abs_diff_closedloop_vs_normal = max(abs(ycl - yN), [], 1);
    r.max_abs_diff_normal_vs_sil = max(abs(yN - yS), [], 1);
    r.outputs = outNames;
    r.step_time_us = prof;
    fprintf("%-16s %d samples\n  closed loop vs Normal replay: %s\n  Normal replay vs SIL:         %s\n", name, numel(t), ...
        mat2str(r.max_abs_diff_closedloop_vs_normal, 3), mat2str(r.max_abs_diff_normal_vs_sil, 3));
    if ~isempty(fieldnames(prof))
        fprintf("  SIL step time: mean %.1f us, max %.1f us\n", prof.mean_us, prof.max_us);
    end
    if isempty(V.scenarios), V.scenarios = r; else, V.scenarios(end+1) = r; end
end
close_system(model, 0);
V.tolerance = 1e-9;
V.pass = all(arrayfun(@(r) all(r.max_abs_diff_normal_vs_sil <= V.tolerance), V.scenarios));
fid = fopen(fullfile(here, "results", "codegen_verification.json"), "w");
fwrite(fid, jsonencode(V, PrettyPrint=true)); fclose(fid);
fprintf("SIL equivalence (|diff| <= %g): %d\n", V.tolerance, V.pass);
end

% =====================================================================
function [R, m, out] = closed_loop(s)
cl = 'acc_closed_loop';
load_system(cl);
set_param([cl '/Controller'], "SimulationMode", "Normal");
[scn_lead, scn_disc] = acc_scenario_signals(s);
P = acc_plant_params();
g0 = 0;
for k = P.at.top_gear:-1:P.at.min_gear
    if s.v0 * P.at.kv * P.at.ratio(k) >= 1100, g0 = k; break; end
end
P.init.v0 = s.v0;
if g0 > 0
    P.init.we0 = s.v0 * P.at.kv * P.at.ratio(g0) * pi/30;
    P.init.Te0 = (P.veh.c_air*s.v0^2 + P.veh.crr*P.veh.m*P.veh.g) * P.veh.r / (P.veh.fd * P.at.ratio(g0) * P.gb.eta);
end
in = Simulink.SimulationInput(cl);
in = in.setModelParameter("StopTime", num2str(s.T));
in = in.setVariable("scn_lead", scn_lead);
in = in.setVariable("scn_disc", scn_disc);
in = in.setVariable("P", P, "Workspace", "acc_plant");
in = in.setVariable("AT_init_gear", g0, "Workspace", "acc_plant");
out = sim(in);
R = []; m = [];
end

function y = sample(ts, t)
tt = ts.Time; d = double(squeeze(ts.Data));
if size(d, 1) ~= numel(tt), d = d.'; end
% value at each controller sample (the controller holds its outputs between samples)
idx = zeros(numel(t), 1);
for k = 1:numel(t)
    j = find(tt <= t(k) + 1e-9, 1, "last");
    idx(k) = j;
end
y = d(idx, :);
end

function ds = input_dataset(cin, t)
names = ["enable", "set_speed_mps", "time_gap_s", "ego_speed_mps", "ego_accel_mps2", "lead_valid", ...
         "lead_gap_m", "lead_speed_mps", "lead_accel_mps2", "resume", "engine_rpm", "gear"];
ds = Simulink.SimulationData.Dataset;
for i = 1:12
    d = cin(:, i);
    switch names(i)
        case {"enable", "lead_valid", "resume"}, d = logical(d);
        case "gear", d = int32(d);
    end
    ds = ds.addElement(timeseries(d, t, "Name", names(i)), names(i));
end
end

function [y, prof] = run_top(model, ds, T, t, profile)
in = Simulink.SimulationInput(model);
in = in.setExternalInput(ds);
in = in.setModelParameter("StopTime", num2str(T), "SaveOutput", "on", ...
    "OutputSaveName", "yout", "SaveFormat", "Dataset", "ReturnWorkspaceOutputs", "on");
prof = struct();
if profile
    in = in.setModelParameter("CodeExecutionProfiling", "on", "CodeProfilingSaveOptions", "SummaryOnly", ...
        "CodeExecutionProfileVariable", "executionProfile");
end
out = sim(in);
y = zeros(numel(t), 6);
for i = 1:6, y(:, i) = sample(out.yout{i}.Values, t); end
if profile
    try
        ep = out.executionProfile;
        calls = arrayfun(@(x) double(x.NumCalls), ep.Sections);
        [~, k] = max(calls);                     % the step function (initialize runs once)
        sec = ep.Sections(k);
        prof.section = string(sec.Name);
        ticksPerSec = ep.TimerTicksPerSecond;
        prof.mean_us = double(sec.TotalExecutionTimeInTicks) / double(sec.NumCalls) / double(ticksPerSec) * 1e6;
        prof.max_us = double(sec.MaximumExecutionTimeInTicks) / double(ticksPerSec) * 1e6;
        prof.calls = double(sec.NumCalls);
    catch err
        prof.note = string(err.message);
    end
end
end
