function V = verify_lcc_codegen(opts)
%VERIFY_LCC_CODEGEN Compare the generated C++ (SIL) with the model on closed-loop scenarios.
%   V = verify_lcc_codegen() runs each scenario/config of opts.Cases on lcc_closed_loop.slx with
%   the MPC in Normal mode and records the 9 controller inputs (69 doubles) at the 20 ms ticks.
%   It then replays those inputs into LCC_Controller.slx as a top model twice: Normal mode and
%   Software-in-the-loop (the ERT C++ code built by Embedded Coder, compiled with the host
%   compiler in its default debug build, with execution-time profiling) and compares the three
%   outputs:
%     closed loop vs Normal replay  (the replay is faithful)
%     Normal replay vs SIL          (the generated code matches the model)
%   Results: results/codegen_verification.json.
arguments
    opts.Cases (:,2) string = ["R400_80" "trailer"; "S300_80" "trailer"; "offset3m_60" "truck"; "R40_15" "trailer"]
end
here = fileparts(mfilename("fullpath"));
cd(here);
model = 'LCC_Controller';
lcc_controller_init();
cleanup = onCleanup(@() evalin("base", "clear LCC_CTRL"));
cg = fullfile(here, "codegen");
if ~isfolder(cg), mkdir(cg); end
Simulink.fileGenControl("set", "CodeGenFolder", cg, "CacheFolder", fullfile(cg, "cache"), "createDir", true);
restore = onCleanup(@() Simulink.fileGenControl("reset"));
S = lcc_scenarios();
outNames = ["wheel_angle_cmd_rad", "wheel_rate_cmd_rad_s", "status"];
V.scenarios = struct([]);
for c = 1:size(opts.Cases, 1)
    s = S([S.name] == opts.Cases(c, 1));
    config = opts.Cases(c, 2);
    load_system("lcc_closed_loop");
    set_param("lcc_closed_loop/Lateral/MPC/Controller", "SimulationMode", "Normal");
    [in, lane] = lcc_scenario_input(s, config, 2); %#ok<ASGLU>
    out = sim(in);
    R = lcc_collect(out);
    t = R.t;
    cin = R.ctrl_in;
    ycl = [R.wheel_angle_cmd R.wheel_rate_cmd R.status];
    ds = input_dataset(cin, t);
    load_system(model);
    set_param(model, "SimulationMode", "normal");
    yN = run_top(model, ds, t, false);
    set_param(model, "SimulationMode", "software-in-the-loop (sil)");
    [yS, prof] = run_top(model, ds, t, true);
    set_param(model, "SimulationMode", "normal");
    r.name = s.name + "_" + config;
    r.samples = numel(t);
    r.max_abs_diff_closedloop_vs_normal = max(abs(ycl - yN), [], 1);
    r.max_abs_diff_normal_vs_sil = max(abs(yN - yS), [], 1);
    r.outputs = outNames;
    r.step_time_us = prof;
    fprintf("%-22s %d samples\n  closed loop vs Normal replay: %s\n  Normal replay vs SIL:         %s\n", r.name, numel(t), ...
        mat2str(r.max_abs_diff_closedloop_vs_normal, 3), mat2str(r.max_abs_diff_normal_vs_sil, 3));
    if isfield(prof, "mean_us")
        fprintf("  SIL step time (debug build): mean %.0f us, max %.0f us\n", prof.mean_us, prof.max_us);
    end
    if isempty(V.scenarios), V.scenarios = r; else, V.scenarios(end+1) = r; end
end
close_system(model, 0);
close_system("lcc_closed_loop", 0);
V.tolerance = 1e-9;
V.pass = all(arrayfun(@(r) all(r.max_abs_diff_normal_vs_sil <= V.tolerance), V.scenarios));
fid = fopen(fullfile(here, "results", "codegen_verification.json"), "w");
fwrite(fid, jsonencode(V, PrettyPrint=true)); fclose(fid);
fprintf("SIL equivalence (|diff| <= %g): %d\n", V.tolerance, V.pass);
end

% =====================================================================
function ds = input_dataset(cin, t)
% ctrl_in columns: enable, speed, ey, epsi, preview(61), delta, wheelbase, trailer, hitch
names = ["enable", "speed_mps", "lateral_error_m", "heading_error_rad", "curvature_preview", ...
         "wheel_angle_rad", "wheelbase_m", "trailer_connected", "hitch_angle_rad"];
cols = {1, 2, 3, 4, 5:65, 66, 67, 68, 69};
ds = Simulink.SimulationData.Dataset;
for i = 1:numel(names)
    d = cin(:, cols{i});
    if ismember(names(i), ["enable", "trailer_connected"]), d = logical(d); end
    ds = ds.addElement(timeseries(d, t, "Name", names(i)), names(i));
end
end

function [y, prof] = run_top(model, ds, t, profile)
in = Simulink.SimulationInput(model);
in = in.setExternalInput(ds);
in = in.setModelParameter("StopTime", num2str(t(end)), "SaveOutput", "on", ...
    "OutputSaveName", "yout", "SaveFormat", "Dataset", "ReturnWorkspaceOutputs", "on");
prof = struct();
if profile
    in = in.setModelParameter("CodeExecutionProfiling", "on", "CodeProfilingSaveOptions", "SummaryOnly", ...
        "CodeExecutionProfileVariable", "executionProfile");
end
out = sim(in);
y = zeros(numel(t), 3);
for i = 1:3
    ts = out.yout{i}.Values;
    d = double(squeeze(ts.Data)); tt = ts.Time;
    idx = discretize(t + 1e-9, [tt; inf]);
    y(:, i) = d(idx);
end
if profile
    try
        ep = out.executionProfile;
        calls = arrayfun(@(x) double(x.NumCalls), ep.Sections);
        [~, k] = max(calls);
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
