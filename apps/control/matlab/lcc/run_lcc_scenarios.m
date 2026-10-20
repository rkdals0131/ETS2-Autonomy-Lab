function [Res, T] = run_lcc_scenarios(opts)
%RUN_LCC_SCENARIOS Baseline vs MPC on lcc_closed_loop.slx in the lane_test.py scenarios.
%   [Res, T] = run_lcc_scenarios() runs every scenario of lcc_scenarios.m with the truck alone
%   and with the trailer, each with the baseline (lateral_controller.hpp port, LCC_CTRL = 1)
%   and the MPC (LCC_Controller.slx, LCC_CTRL = 2), on the same plant. It writes
%     results/scenario_<name>_<config>.json   metrics + time series of both (0.1 s)
%     results/scenario_<name>_<config>.png    lateral error, wheel angle, rate, trailer rear axle
%     results/scenarios_summary.json          the table T (one row per scenario/config/controller)
%   and prints the comparison. Options: Names, Configs (["truck" "trailer"]), Wander (yaw-rate
%   wander amplitude, 0 = deterministic plant; lcc_plant_params().dist.amp_game reproduces the
%   game's straight-line wander), Tag (file suffix), ControllerMode ("Normal" or
%   "Software-in-the-loop (SIL)"), Plot, Save, Rebuild (rebuild the three .slx first).
arguments
    opts.Names (1,:) string = string.empty
    opts.Configs (1,:) string = ["truck", "trailer"]
    opts.Wander (1,1) double = 0
    opts.Tag (1,1) string = ""
    opts.ControllerMode (1,1) string = "Normal"
    opts.Plot (1,1) logical = true
    opts.Save (1,1) logical = true
    opts.Rebuild (1,1) logical = false
end
here = fileparts(mfilename("fullpath"));
cd(here);
models = ["lcc_plant", "LCC_Controller", "lcc_closed_loop"];
builders = {@build_lcc_plant, @build_lcc_controller, @build_lcc_closed_loop};
for i = 1:3
    if opts.Rebuild || ~isfile(fullfile(here, models(i) + ".slx"))
        builders{i}();
        if bdIsLoaded(models(i)), close_system(models(i), 0); end
    end
end
lcc_controller_init();
cleanup = onCleanup(@() evalin("base", "clear LCC_CTRL"));
cl = 'lcc_closed_loop';
load_system(cl);
set_param([cl '/Lateral/MPC/Controller'], "SimulationMode", opts.ControllerMode);
S = lcc_scenarios();
if ~isempty(opts.Names), S = S(ismember([S.name], opts.Names)); end
ctrlName = ["baseline", "mpc"];
Res = struct([]); T = struct([]);
for config = opts.Configs
    for s = S
        runs = struct([]);
        for ctrl = 1:2
            [in, lane, P] = lcc_scenario_input(s, config, ctrl, Wander=opts.Wander);
            t0 = tic;
            out = sim(in);
            R = lcc_collect(out);
            m = lcc_scenario_metrics(R, lane, P);
            wall = toc(t0);
            r.controller = ctrlName(ctrl); r.R = R; r.m = m; r.wall_s = wall;
            if isempty(runs), runs = r; else, runs(end+1) = r; end %#ok<AGROW>
            row = summary_row(s, config, ctrlName(ctrl), m);
            if isempty(T), T = row; else, T(end+1) = row; end %#ok<AGROW>
        end
        fprintf("%-16s %-7s  %s\n%-24s  %s\n", s.name, config, line(runs(1).m), "", line(runs(2).m));
        q.scenario = s; q.config = config; q.runs = runs;
        if isempty(Res), Res = q; else, Res(end+1) = q; end %#ok<AGROW>
        base = fullfile(here, "results", "scenario_" + s.name + "_" + config + opts.Tag);
        if opts.Save, save_json(base + ".json", s, config, runs); end
        if opts.Plot, plot_runs(base + ".png", s, config, runs, lane); end
    end
end
if opts.Save && isempty(opts.Names)
    fid = fopen(fullfile(here, "results", "scenarios_summary" + opts.Tag + ".json"), "w");
    fwrite(fid, jsonencode(T, PrettyPrint=true)); fclose(fid);
end
print_table(T);
end

% =====================================================================
function row = summary_row(s, config, ctrl, m)
row.scenario = s.name; row.config = config; row.controller = ctrl; row.kmh = s.kmh;
f = ["max_offset_first150m", "max_offset_after150m", "rms_offset_after150m", "mean_offset_in_curves", ...
     "inward_offset_in_curves", "max_heading_error_deg_after150m", "max_wheel_rate", "max_wheel_rate_cmd", ...
     "saturated_ticks", "cmd_sign_reversals", "max_abs_delta"];
for k = f, row.(k) = m.(k); end
row.status_counts = m.status_counts;
tf = ["trailer_rear_max_m", "trailer_rear_inward_in_curves_m", "trailer_rear_max_inward_m", "trailer_rear_max_outward_m"];
for k = tf
    if isfield(m, k), row.(k) = m.(k); else, row.(k) = NaN; end
end
end

function s = line(m)
s = sprintf("max %5.1f mm  rms %5.1f  curve in %6.1f  rate %.3f rad/s  sat %2d", ...
    1000*m.max_offset_after150m, 1000*m.rms_offset_after150m, 1000*m.inward_offset_in_curves, ...
    m.max_wheel_rate, m.saturated_ticks);
if isfield(m, "trailer_rear_max_m")
    s = s + sprintf("  trailer rear max %4.0f mm in %4.0f", 1000*m.trailer_rear_max_m, 1000*m.trailer_rear_inward_in_curves_m);
end
end

function save_json(file, s, config, runs)
J.scenario = s; J.config = config;
for i = 1:numel(runs)
    R = runs(i).R; m = runs(i).m;
    k = 1:5:numel(R.t);                  % 0.1 s
    sig = struct("t", R.t(k), "ey", R.ey(k), "epsi", R.epsi(k), "delta", R.delta(k), "wheel_angle_cmd", R.wheel_angle_cmd(k), ...
        "wheel_rate_cmd", R.wheel_rate_cmd(k), "u", R.u(k), "hitch", R.hitch(k), "kappa", R.preview0(k), "status", R.status(k));
    if isfield(m, "trailer_rear_error"), sig.trailer_rear_error = m.trailer_rear_error(k); m = rmfield(m, "trailer_rear_error"); end
    J.(runs(i).controller) = struct("metrics", m, "signals", sig, "wall_s", runs(i).wall_s);
end
fid = fopen(file, "w"); fwrite(fid, jsonencode(J)); fclose(fid);
end

function plot_runs(file, s, config, runs, lane)
f = figure("Visible", "off", "Position", [100 100 1100 850]);
tl = tiledlayout(f, 4, 1, "TileSpacing", "compact");
title(tl, sprintf("%s (%s)", s.title, config), "Interpreter", "none");
col = [0.35 0.35 0.35; 0.85 0.33 0.10];
nexttile; hold on;
for i = 1:2, plot(runs(i).R.s, 1000 * runs(i).R.ey, "Color", col(i, :)); end
ylabel("lateral error mm"); legend("baseline", "MPC", "Location", "best", "AutoUpdate", "off"); grid on;
yl = max(abs(ylim)); ylim([-1 1] * min(yl, 80));
yyaxis right; plot(lane.s, [0; diff(lane.theta)] / 0.25, ":", "Color", [0.3 0.5 0.9]); ylabel("curvature 1/m");
nexttile; hold on;
for i = 1:2, plot(runs(i).R.s, runs(i).R.delta, "Color", col(i, :)); end
ylabel("wheel angle rad"); grid on;
nexttile; hold on;
for i = 1:2, plot(runs(i).R.s(2:end), diff(runs(i).R.delta) / 0.02, "Color", col(i, :)); end
ylabel("wheel rate rad/s"); grid on;
nexttile; hold on;
if isfield(runs(1).m, "trailer_rear_error")
    for i = 1:2, plot(runs(i).R.s, 1000 * runs(i).m.trailer_rear_error, "Color", col(i, :)); end
    ylabel("trailer rear axle mm");
else
    for i = 1:2, plot(runs(i).R.s, rad2deg(runs(i).R.epsi), "Color", col(i, :)); end
    ylabel("heading error deg");
end
xlabel("lane distance m"); grid on;
exportgraphics(f, file); close(f);
end

function print_table(T)
fprintf("\n%-16s %-7s %-8s %8s %8s %9s %9s %7s %4s %9s %9s\n", "scenario", "config", "ctrl", "max mm", "rms mm", "curve mm", "rate r/s", "headdeg", "sat", "trl max", "trl in");
for r = T
    fprintf("%-16s %-7s %-8s %8.1f %8.1f %9.1f %9.3f %7.2f %4d %9.0f %9.0f\n", r.scenario, r.config, r.controller, ...
        1000*r.max_offset_after150m, 1000*r.rms_offset_after150m, 1000*r.inward_offset_in_curves, r.max_wheel_rate, ...
        r.max_heading_error_deg_after150m, r.saturated_ticks, 1000*r.trailer_rear_max_m, 1000*r.trailer_rear_inward_in_curves_m);
end
end
