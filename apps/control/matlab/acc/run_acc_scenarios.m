function [Res, M] = run_acc_scenarios(opts)
%RUN_ACC_SCENARIOS Closed-loop ACC scenarios on acc_closed_loop.slx (controller + plant).
%   [Res, M] = run_acc_scenarios() runs every scenario of acc_scenarios.m, computes the
%   metrics (acc_scenario_metrics.m), writes results/scenario_<name>.json, results/
%   scenario_<name>.png and results/scenarios_summary.json, and prints a table.
%   Options: Names (subset), ControllerMode ("Normal" or "Software-in-the-loop (SIL)"),
%   Plot (true), Save (true), Rebuild (false: build the .slx files only when missing), Tag
%   (suffix of the result files), Scenarios (struct array instead of acc_scenarios()), and the
%   mass / road options of acc_scenario_input.m: PlantMass (kg), MassPrior (kg, 0 unknown),
%   Adaptive, PitchKnown, ActuationValid, Grade. SaveSignals false writes only the summary.
arguments
    opts.Names (1,:) string = string.empty
    opts.ControllerMode (1,1) string = "Normal"
    opts.Plot (1,1) logical = true
    opts.Save (1,1) logical = true
    opts.Rebuild (1,1) logical = false
    opts.Tag (1,1) string = ""
    opts.PlantMass (1,1) double = 20300
    opts.MassPrior (1,1) double = 0
    opts.Adaptive (1,1) logical = true
    opts.PitchKnown (1,1) logical = true
    opts.ActuationValid (1,1) logical = true
    opts.Grade (:,2) double = [0 0]
    opts.Scenarios struct = struct([])
    opts.SaveSignals (1,1) logical = true   % false: only the summary json (no per-scenario time series)
end
here = fileparts(mfilename("fullpath"));
cd(here);
models = ["acc_plant", "ACC_Controller", "acc_closed_loop"];
builders = {@build_acc_plant, @build_acc_controller, @build_acc_closed_loop};
for i = 1:3
    if opts.Rebuild || ~isfile(fullfile(here, models(i) + ".slx"))
        builders{i}();
    end
end
acc_controller_init();
cl = 'acc_closed_loop';
load_system(cl);
set_param([cl '/Controller'], "SimulationMode", opts.ControllerMode);
if isempty(opts.Scenarios), S = acc_scenarios(); else, S = opts.Scenarios; end
if ~isempty(opts.Names), S = S(ismember([S.name], opts.Names)); end
Res = struct([]); M = struct([]);
for i = 1:numel(S)
    s = S(i);
    in = acc_scenario_input(s, PlantMass=opts.PlantMass, MassPrior=opts.MassPrior, ...
        Adaptive=opts.Adaptive, PitchKnown=opts.PitchKnown, ActuationValid=opts.ActuationValid, Grade=opts.Grade);
    t0 = tic;
    out = sim(in);
    R = collect(out, s);
    R.wall_s = toc(t0);
    m = acc_scenario_metrics(R, s);
    m.plant_mass_kg = opts.PlantMass; m.mass_prior_kg = opts.MassPrior; m.adaptive = opts.Adaptive;
    m.mass_final_kg = R.mass_est(end);
    m.mass_valid_at_s = NaN;
    kv = find(R.mass_valid > 0.5, 1); if ~isempty(kv), m.mass_valid_at_s = R.t(kv); end
    k5 = find(abs(R.mass_est / opts.PlantMass - 1) > 0.05, 1, "last");
    if isempty(k5), m.mass_within5_s = 0; elseif k5 == numel(R.t), m.mass_within5_s = NaN; else, m.mass_within5_s = R.t(k5 + 1); end
    fprintf("%-16s %s  mass %5.0f (%+4.1f %%)  (%.1f s wall)\n", s.name, metric_line(m), m.mass_final_kg, ...
        100 * (m.mass_final_kg / opts.PlantMass - 1), R.wall_s);
    if isempty(Res), Res = R; M = m; else, Res(end+1) = R; M(end+1) = m; end %#ok<AGROW>
    if opts.Save && opts.SaveSignals
        J = struct("scenario", s, "metrics", m, "t", R.t, "signals", rmfield(R, ["t", "name", "wall_s"]));
        fid = fopen(fullfile(here, "results", "scenario_" + s.name + opts.Tag + ".json"), "w");
        fwrite(fid, jsonencode(J)); fclose(fid);
    end
    if opts.Plot
        plot_scenario(R, s, m, fullfile(here, "results", "scenario_" + s.name + opts.Tag + ".png"));
    end
end
if opts.Save && (isempty(opts.Names) || ~opts.SaveSignals)
    fid = fopen(fullfile(here, "results", "scenarios_summary" + opts.Tag + ".json"), "w");
    fwrite(fid, jsonencode(M, PrettyPrint=true)); fclose(fid);
end
end

% =====================================================================
function R = collect(out, s)
names = ["v", "a", "gap", "v_lead", "a_lead", "lead_valid", "throttle", "brake", "retarder", ...
         "engine_brake", "a_des", "mode", "gear", "rpm", "clutch", "F_brake", "x_ego", "set_speed", ...
         "mass_est", "mass_sigma", "mass_valid"];
t = (0:0.05:s.T).';
R.name = s.name;
R.t = t;
y = out.yout;
for i = 1:numel(names)
    e = y{i};                                   % outport i (port order = names)
    tt = e.Values.Time; dd = double(squeeze(e.Values.Data));
    [tt, iu] = unique(tt, "last"); dd = dd(iu);
    if ismember(names(i), ["throttle", "brake", "retarder", "engine_brake", "a_des", "mode", "gear", "lead_valid", "set_speed", "mass_est", "mass_sigma", "mass_valid"])
        R.(names(i)) = interp1(tt, dd, t, "previous", "extrap");
    else
        R.(names(i)) = interp1(tt, dd, t, "linear", "extrap");
    end
end
end

function str = metric_line(m)
str = sprintf("min gap %6.1f m  min TTC %5.1f s  peak decel %4.2f  max|jerk| %4.2f  brake %4.1f s %5.0f kJ  ret changes %2d  final gap %5.1f  collision %d", ...
    m.min_gap_m, m.min_ttc_s, m.peak_decel_mps2, m.max_abs_jerk_mps3, m.brake_time_s, m.brake_energy_kJ, ...
    m.retarder_changes, m.final_gap_m, m.collision);
end

function plot_scenario(R, s, m, file)
f = figure("Visible", "off", "Position", [80 80 1000 1050]);
try
    f.Theme = "light";
catch
end
tl = tiledlayout(f, 6, 1, "TileSpacing", "compact");
title(tl, s.title + "  (" + s.name + ")", "Interpreter", "none");
t = R.t;
lv = R.lead_valid > 0.5;
ax = nexttile(tl); hold(ax, "on"); grid(ax, "on");
plot(ax, t, R.v*3.6, "b", "LineWidth", 1.2); plot(ax, t, R.set_speed*3.6, "k:");
vl = R.v_lead*3.6; vl(~lv) = NaN; plot(ax, t, vl, "r");
ylabel(ax, "km/h"); legend(ax, "ego", "set", "lead", "Location", "best");
ax = nexttile(tl); hold(ax, "on"); grid(ax, "on");
g = R.gap; g(~lv) = NaN; plot(ax, t, g, "b", "LineWidth", 1.2);
dd = 5 + s.time_gap*R.v; dd(~lv) = NaN; plot(ax, t, dd, "k--");
ds = 3 + 0.5*s.time_gap*R.v; ds(~lv) = NaN; plot(ax, t, ds, "r:");
ylabel(ax, "gap m"); legend(ax, "gap", "d0 + th v", "soft min", "Location", "best");
ax = nexttile(tl); hold(ax, "on"); grid(ax, "on");
plot(ax, t, R.a, "b", "LineWidth", 1.2); plot(ax, t, R.a_des, "m");
al = R.a_lead; al(~lv) = NaN; plot(ax, t, al, "r:");
ylabel(ax, "m/s^2"); legend(ax, "ego a", "a_{des}", "lead a", "Location", "best");
ax = nexttile(tl); hold(ax, "on"); grid(ax, "on");
plot(ax, t, R.throttle, "g", "LineWidth", 1.2); plot(ax, t, R.brake*5, "r", "LineWidth", 1.2);
ylabel(ax, "pedal"); legend(ax, "throttle", "brake x5", "Location", "best");
ax = nexttile(tl); hold(ax, "on"); grid(ax, "on");
stairs(ax, t, R.retarder, "m", "LineWidth", 1.2); stairs(ax, t, R.mode, "k"); stairs(ax, t, R.gear/2, "c");
ylabel(ax, "step"); legend(ax, "retarder", "mode", "gear/2", "Location", "best");
text(ax, 0.01, 0.92, sprintf("min gap %.1f m, min TTC %.1f s, peak decel %.2f, max|jerk| %.2f, ret changes %d", ...
    m.min_gap_m, m.min_ttc_s, m.peak_decel_mps2, m.max_abs_jerk_mps3, m.retarder_changes), ...
    "Units", "normalized", "FontSize", 8);
ax = nexttile(tl); hold(ax, "on"); grid(ax, "on");
plot(ax, t, R.mass_est / 1000, "b", "LineWidth", 1.2);
mv = R.mass_est / 1000; mv(R.mass_valid < 0.5) = NaN; plot(ax, t, mv, "g", "LineWidth", 2);
if isfield(m, "plant_mass_kg"), yline(ax, m.plant_mass_kg / 1000, "k--"); end
ylabel(ax, "mass t"); xlabel(ax, "s"); legend(ax, "estimate", "valid", "plant", "Location", "best");
exportgraphics(f, file, "Resolution", 90);
close(f);
end
