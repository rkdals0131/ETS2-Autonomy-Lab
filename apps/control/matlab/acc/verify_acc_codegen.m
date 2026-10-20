function V = verify_acc_codegen(opts)
%VERIFY_ACC_CODEGEN Compare the generated C++ (SIL) with the model on closed-loop scenarios.
%   V = verify_acc_codegen() runs each scenario in opts.Names on acc_closed_loop.slx with the
%   controller in Normal mode and records the 18 controller inputs at the controller samples.
%   It then replays those inputs into ACC_Controller.slx as a top model twice: Normal mode
%   and Software-in-the-loop (the ERT C++ code built by Embedded Coder, compiled with the
%   host compiler, with execution-time profiling) and compares the nine outputs:
%     closed loop vs Normal replay  (the replay is faithful)
%     Normal replay vs SIL          (the generated code matches the model)
%   Results: results/codegen_verification.json.
arguments
    opts.Names (1,:) string = ["lead_brake_4", "stop_and_go_hold", "lead_emergency_stop", "mass_mixed"]
    opts.PlantMass (1,:) double = [20300 20300 20300 40000]   % per scenario (40 t: the estimator moves)
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
outNames = ["throttle", "brake", "retarder_step", "engine_brake", "a_des_mps2", "mode", ...
            "mass_est_kg", "mass_sigma_kg", "mass_valid"];
nOut = numel(outNames);
V.scenarios = struct([]);
for iname = 1:numel(opts.Names)
    name = opts.Names(iname);
    s = S([S.name] == name);
    load_system('acc_closed_loop');
    set_param('acc_closed_loop/Controller', "SimulationMode", "Normal");
    out = sim(acc_scenario_input(s, PlantMass=opts.PlantMass(iname)));
    t = (0:0.05:s.T).';
    cin = sample(out.yout{22}.Values, t);                 % 18 inputs, double
    ycl = zeros(numel(t), nOut);
    srcIdx = [7 8 9 10 11 12 19 20 21];                    % closed-loop outports of the controller outputs
    for i = 1:nOut, ycl(:, i) = sample(out.yout{srcIdx(i)}.Values, t); end
    ds = input_dataset(cin, t);
    load_system(model);
    set_param(model, "SimulationMode", "normal");
    yN = run_top(model, ds, s.T, t, false);
    set_param(model, "SimulationMode", "software-in-the-loop (sil)");
    [yS, prof] = run_top(model, ds, s.T, t, true);
    set_param(model, "SimulationMode", "normal");
    r.name = name;
    r.plant_mass_kg = opts.PlantMass(iname);
    r.samples = numel(t);
    r.max_abs_diff_closedloop_vs_normal = max(abs(ycl - yN), [], 1);
    r.max_abs_diff_normal_vs_sil = max(abs(yN - yS), [], 1);
    r.output_scale = max(1, max(abs(yN), [], 1));          % relative check for the kg outputs
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
V.tolerance = 1e-9;                                        % times max(1, |output|)
V.pass = all(arrayfun(@(r) all(r.max_abs_diff_normal_vs_sil <= V.tolerance * r.output_scale), V.scenarios));
fid = fopen(fullfile(here, "results", "codegen_verification.json"), "w");
fwrite(fid, jsonencode(V, PrettyPrint=true)); fclose(fid);
fprintf("SIL equivalence (|diff| <= %g x max(1, |y|)): %d\n", V.tolerance, V.pass);
end

% =====================================================================
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
         "lead_gap_m", "lead_speed_mps", "lead_accel_mps2", "resume", "engine_rpm", "gear", ...
         "pitch_rad", "mass_prior_kg", "actuation_valid", "throttle_applied", "brake_applied", "retarder_applied"];
ds = Simulink.SimulationData.Dataset;
for i = 1:numel(names)
    d = cin(:, i);
    switch names(i)
        case {"enable", "lead_valid", "resume", "actuation_valid"}, d = logical(d);
        case {"gear", "retarder_applied"}, d = int32(d);
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
n = numel(out.yout.getElementNames());
y = zeros(numel(t), n);
for i = 1:n, y(:, i) = sample(out.yout{i}.Values, t); end
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
