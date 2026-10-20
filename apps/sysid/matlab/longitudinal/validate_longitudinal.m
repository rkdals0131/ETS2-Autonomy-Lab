function [R, sweep] = validate_longitudinal(opts)
%VALIDATE_LONGITUDINAL Replay recorded inputs through longitudinal_plant.slx.
%   R = validate_longitudinal() simulates every phase segment of the validation
%   runs (initial speed, engine torque and engine speed taken from the first frame
%   of the segment), and the whole run in one open-loop replay, then compares the
%   simulated speed, engine torque and gearbox output torque with the Tier 1 log.
%   RMS errors are pooled per run and phase kind. Results go to
%   results/validation.json, results/validation.mat and results/*.png.
%
%   Options:
%     Variants   ["fitted","baseline"]  parameter variants (longitudinal_params)
%     Sweeps     struct array (param, values, kinds): one-parameter sweeps on the
%                segments of the given phase kinds of all runs ([] to skip)
%     Plots      save PNG plots (default true)
%     Save       write results/validation.* (default true)
%     Set        {"gb.C_clutch", 35000, ...} overrides applied after longitudinal_params
arguments
    opts.Runs (1,:) string = ["gear-20261020-151650", "brake-20261020-152718", ...
        "sysid-20261020-153130", "sysid-20261020-144933"]
    opts.Variants (1,:) string = ["fitted", "baseline"]
    opts.Sweeps = default_sweeps()
    opts.Plots (1,1) logical = true
    opts.Save (1,1) logical = true
    opts.Set (1,:) cell = {}
    opts.SegmentMax (1,1) double = 30
end
here = fileparts(mfilename("fullpath"));
resDir = fullfile(here, "results");
if ~isfolder(resDir), mkdir(resDir); end
model = "longitudinal_plant";
if ~isfile(fullfile(here, model + ".slx")), build_longitudinal_model(); end
load_system(fullfile(here, model + ".slx"));
cleanup = onCleanup(@() set_param(model, "FastRestart", "off"));

R = struct("run", {}, "variant", {}, "mass", {}, "segments", {}, "kinds", {}, "whole", {});
for run = opts.Runs
    D = load_recording(run);
    segs = segments(D, opts.SegmentMax);
    for variant = opts.Variants
        [P, massNote] = run_params(run, D, variant, opts.Set);
        fprintf("== %s  %s  (m %.0f kg, crr %.4f, %d segments)\n", run, variant, P.veh.m, P.veh.crr, numel(segs));
        S = simulate_segments(model, D, segs, P);
        W = simulate_whole(model, D, P);
        r.run = run; r.variant = variant; r.mass = struct("m", P.veh.m, "crr", P.veh.crr, "note", massNote);
        r.segments = S; r.kinds = pool_kinds(S); r.whole = W;
        R(end+1) = r; %#ok<AGROW>
        print_kinds(r);
        if opts.Plots && variant == "fitted"
            plot_run(D, S, W, run, resDir);
        end
    end
end

% one-parameter sweeps on selected phase kinds of all runs
sweep = struct("param", {}, "value", {}, "kinds", {}, "rms_v", {}, "rms_a", {}, "rms_te", {});
for sw = opts.Sweeps(:).'
    fprintf("== sweep P.%s on %s\n", sw.param, strjoin(sw.kinds, ", "));
    for value = sw.values
        ev = []; ea = []; et = [];
        for run = opts.Runs
            D = load_recording(run);
            P = run_params(run, D, "fitted", [opts.Set, {sw.param, value}]);
            segs = segments(D, opts.SegmentMax);
            k = ismember([segs.kind], sw.kinds);
            if ~any(k), continue; end
            S = simulate_segments(model, D, segs(k), P);
            ev = [ev; vertcat(S.err_v)]; ea = [ea; vertcat(S.err_a)]; et = [et; vertcat(S.err_te)]; %#ok<AGROW>
        end
        sweep(end+1) = struct("param", sw.param, "value", value, "kinds", strjoin(sw.kinds, ","), ...
            "rms_v", rms(ev, "omitnan"), "rms_a", rms(ea, "omitnan"), "rms_te", rms(et, "omitnan")); %#ok<AGROW>
        fprintf("   %-14s %8.3f: speed rms %.3f m/s, accel rms %.3f m/s^2, Te rms %.0f Nm\n", ...
            sw.param, value, sweep(end).rms_v, sweep(end).rms_a, sweep(end).rms_te);
    end
end
if ~opts.Save, return; end

% ---- save small results ----------------------------------------------------------
summary = struct("created", string(datetime("now", "Format", "yyyy-MM-dd HH:mm")), ...
    "sweeps", sweep, "runs", []);
runs = struct("run", {}, "variant", {}, "mass", {}, "kinds", {}, "whole", {});
for r = R
    w = rmfield(r.whole, ["t", "v_sim", "v_meas"]);
    runs(end+1) = struct("run", r.run, "variant", r.variant, "mass", r.mass, ...
        "kinds", r.kinds, "whole", w); %#ok<AGROW>
end
summary.runs = runs;
fid = fopen(fullfile(resDir, "validation.json"), "w");
fwrite(fid, jsonencode(summary, "PrettyPrint", true)); fclose(fid);
Rsmall = R;
for i = 1:numel(Rsmall)
    Rsmall(i).segments = rmfield(Rsmall(i).segments, ["t", "y", "err_v", "err_a", "err_te", "err_to"]);
    Rsmall(i).whole = rmfield(Rsmall(i).whole, ["t", "v_sim", "v_meas"]);
end
save(fullfile(resDir, "validation.mat"), "Rsmall", "sweep");
end

% =====================================================================
function s = default_sweeps()
s = struct("param", {"veh.tau_body", "gb.C_clutch"}, ...
    "values", {[0 0.1 0.15 0.2 0.25 0.3 0.4], [150000 190000 230000 305000 400000]}, ...
    "kinds", {["brake", "brake_step", "throttle_step", "full", "half", "eb"], ...
              ["launch", "accel", "throttle_step", "pre_brake"]});
end

function [P, note] = run_params(run, D, variant, set)
% Parameters for one run: configuration from the trailer flag, per-run mass for the
% fitted trailer runs, then the overrides in `set` ({"group.name", value, ...}).
F = longitudinal_fitted_tables();
trailer = mode(D.trailer) > 0;
cfg = "truck"; if trailer, cfg = "trailer"; end
P = longitudinal_params(cfg, variant);
key = matlab.lang.makeValidName(run);
if variant ~= "fitted"
    note = "docs value";
elseif trailer
    P.veh.m = F.mass.(key).m; P.veh.crr = F.mass.(key).crr;
    note = "per-run fit (engine-torque force balance)";
else
    note = sprintf("docs/30 12.0 t (this run's fit %.0f kg agrees, its crr does not)", F.mass.(key).m);
end
for i = 1:2:numel(set)
    parts = split(string(set{i}), ".");
    P.(parts(1)).(parts(2)) = set{i+1};
end
end

function segs = segments(D, maxLen)
% Contiguous blocks of one phase (and attempt); long blocks are cut into windows.
kind = extractBefore(D.phase + ":", ":");
kind = strtrim(kind);
key = D.phase + "#" + D.attempt;
b = [1; find(key(2:end) ~= key(1:end-1) | diff(D.t) > 0.1) + 1; numel(D.t) + 1];
segs = struct("i0", {}, "i1", {}, "kind", {}, "phase", {});
for k = 1:numel(b) - 1
    i0 = b(k); i1 = b(k+1) - 1;
    if ismember(kind(i0), ["init", "settle", "stop", "standstill", "stopped"]), continue; end
    while i0 <= i1
        j = find(D.t(i0:i1) - D.t(i0) <= maxLen, 1, "last") + i0 - 1;
        if i1 - j < 90, j = i1; end            % do not leave a stub under 1.5 s
        if D.t(j) - D.t(i0) >= 1.5
            segs(end+1) = struct("i0", i0, "i1", j, "kind", kind(i0), "phase", D.phase(i0)); %#ok<AGROW>
        end
        i0 = j + 1;
    end
end
end

function U = inputs(D, idx)
thr = D.eff_thr(idx); thr(thr < 1e-3) = 0;
brk = D.eff_brk(idx); brk(brk < 1e-3) = 0;
temp = fillmissing(D.brake_temp(idx), "previous");
temp = fillmissing(temp, "next"); temp(isnan(temp)) = 50;
t = D.t(idx) - D.t(idx(1));
U = [t, thr, brk, D.ratio(idx), D.clutch(idx), D.ret_level(idx), D.eb(idx), temp, zeros(numel(idx), 1)];
end

function P = initial_state(P, D, i0, aMeas)
P.init.v0 = max(D.speed(i0), 0);
P.init.Te0 = D.Te(i0);
P.init.we0 = D.we(i0);
a0 = aMeas(i0);
if isnan(a0), a0 = 0; end
P.init.a0 = a0;
if D.ret_level(i0) > 0
    P.init.ret0 = interp2(P.ret.w_bp, P.ret.level_bp, P.ret.table, ...
        min(D.wo(i0), P.ret.w_bp(end)), D.ret_level(i0));
else
    P.init.ret0 = 0;
end
end

function S = simulate_segments(model, D, segs, P)
set_param(model, "FastRestart", "on");
aMeas = sg_derivative(D.speed, D.t, 15);
S = struct("kind", {}, "phase", {}, "t0", {}, "dur", {}, "t", {}, "y", {}, ...
    "err_v", {}, "err_a", {}, "err_te", {}, "err_to", {}, ...
    "rms_v", {}, "rms_te", {}, "rms_to", {}, "end_v", {});
for k = 1:numel(segs)
    idx = (segs(k).i0:segs(k).i1).';
    U = inputs(D, idx);
    Pk = initial_state(P, D, idx(1), aMeas);
    in = Simulink.SimulationInput(model);
    in = in.setVariable("P", Pk);
    in = in.setExternalInput(U);
    in = in.setModelParameter("StopTime", num2str(U(end, 1)));
    out = sim(in);
    y = interp1(out.tout, out.yout, U(:, 1), "linear", "extrap");
    s.kind = segs(k).kind; s.phase = segs(k).phase; s.t0 = D.t(idx(1)); s.dur = U(end, 1);
    s.t = U(:, 1); s.y = y(:, 1:5);
    s.err_v = y(:, 1) - D.speed(idx);
    s.err_a = y(:, 2) - aMeas(idx);
    s.err_te = y(:, 3) - D.Te(idx);
    s.err_to = y(:, 4) - D.To(idx);
    s.rms_v = rms(s.err_v); s.rms_te = rms(s.err_te); s.rms_to = rms(s.err_to);
    s.end_v = s.err_v(end);
    S(end+1) = s; %#ok<AGROW>
end
end

function W = simulate_whole(model, D, P)
% One open-loop replay of the whole run from the first moving frame.
i0 = find(D.speed > 0.5 | D.eff_thr > 0, 1);
idx = (i0:numel(D.t)).';
U = inputs(D, idx);
P = initial_state(P, D, i0, sg_derivative(D.speed, D.t, 15));
set_param(model, "FastRestart", "on");
in = Simulink.SimulationInput(model);
in = in.setVariable("P", P);
in = in.setExternalInput(U);
in = in.setModelParameter("StopTime", num2str(U(end, 1)));
out = sim(in);
y = interp1(out.tout, out.yout, U(:, 1), "linear", "extrap");
W.t = U(:, 1); W.v_sim = y(:, 1); W.v_meas = D.speed(idx);
W.duration_s = U(end, 1);
W.rms_v = rms(y(:, 1) - D.speed(idx));
W.max_abs_v = max(abs(y(:, 1) - D.speed(idx)));
W.rms_te = rms(y(:, 3) - D.Te(idx));
W.rms_to = rms(y(:, 4) - D.To(idx));
dist_sim = trapz(W.t, W.v_sim); dist_meas = trapz(W.t, W.v_meas);
W.distance_err_pct = 100 * (dist_sim - dist_meas) / dist_meas;
end

function K = pool_kinds(S)
kinds = unique([S.kind], "stable");
K = struct("kind", {}, "segments", {}, "seconds", {}, "rms_v", {}, "rms_te", {}, "rms_to", {}, ...
    "mean_abs_end_v", {});
for k = kinds
    s = S([S.kind] == k);
    ev = vertcat(s.err_v); et = vertcat(s.err_te); eo = vertcat(s.err_to);
    K(end+1) = struct("kind", k, "segments", numel(s), "seconds", round(sum([s.dur]), 1), ...
        "rms_v", round(rms(ev), 3), "rms_te", round(rms(et)), "rms_to", round(rms(eo)), ...
        "mean_abs_end_v", round(mean(abs([s.end_v])), 3)); %#ok<AGROW>
end
ev = vertcat(S.err_v); et = vertcat(S.err_te); eo = vertcat(S.err_to);
K(end+1) = struct("kind", "ALL", "segments", numel(S), "seconds", round(sum([S.dur]), 1), ...
    "rms_v", round(rms(ev), 3), "rms_te", round(rms(et)), "rms_to", round(rms(eo)), ...
    "mean_abs_end_v", round(mean(abs([S.end_v])), 3));
end

function print_kinds(r)
for k = r.kinds
    fprintf("   %-14s n=%3d %6.1f s  v %.3f m/s  Te %5d Nm  To %5d Nm  |end v| %.3f\n", ...
        k.kind, k.segments, k.seconds, k.rms_v, k.rms_te, k.rms_to, k.mean_abs_end_v);
end
fprintf("   whole run %.0f s: v rms %.2f (max %.2f) m/s, distance %+.1f %%, Te %d, To %d Nm\n", ...
    r.whole.duration_s, r.whole.rms_v, r.whole.max_abs_v, r.whole.distance_err_pct, ...
    round(r.whole.rms_te), round(r.whole.rms_to));
end

function plot_run(D, S, W, run, resDir)
% Whole-run speed plus up to six representative segments (speed / Te / To).
f = figure("Visible", "off", "Position", [0 0 1400 900]);
tl = tiledlayout(f, 4, 3, "TileSpacing", "compact");
title(tl, run + " (fitted parameters)", "Interpreter", "none");
ax = nexttile(tl, [1 3]);
plot(ax, W.t, W.v_meas, "k", W.t, W.v_sim, "r"); grid(ax, "on");
ylabel(ax, "speed m/s"); legend(ax, "game", "model", "Location", "best");
title(ax, sprintf("whole run open loop: rms %.2f m/s, distance %+.1f %%", W.rms_v, W.distance_err_pct));
want = ["eb", "full", "coast", "half", "ret", "brake", "accel", "throttle_step", "brake_step", "coast", "sine", "launch"];
pick = [];
for w = want
    j = find([S.kind] == w & [S.dur] > 3, 1);
    if ~isempty(j) && ~ismember(j, pick), pick(end+1) = j; end %#ok<AGROW>
    if numel(pick) == 3, break; end
end
for j = pick
    s = S(j);
    idx = find(D.t >= s.t0 - 1e-9, 1) + (0:numel(s.t) - 1);
    ax = nexttile(tl); plot(ax, s.t, D.speed(idx), "k", s.t, s.y(:, 1), "r"); grid(ax, "on");
    title(ax, s.phase + sprintf(" (v rms %.2f)", s.rms_v), "Interpreter", "none"); ylabel(ax, "m/s");
end
for j = pick
    s = S(j);
    idx = find(D.t >= s.t0 - 1e-9, 1) + (0:numel(s.t) - 1);
    ax = nexttile(tl); plot(ax, s.t, D.Te(idx), "k", s.t, s.y(:, 3), "r"); grid(ax, "on");
    title(ax, sprintf("Te (rms %.0f Nm)", s.rms_te)); ylabel(ax, "Nm");
end
for j = pick
    s = S(j);
    idx = find(D.t >= s.t0 - 1e-9, 1) + (0:numel(s.t) - 1);
    ax = nexttile(tl); plot(ax, s.t, D.To(idx), "k", s.t, s.y(:, 4), "r"); grid(ax, "on");
    title(ax, sprintf("To (rms %.0f Nm)", s.rms_to)); ylabel(ax, "Nm"); xlabel(ax, "s");
end
exportgraphics(f, fullfile(resDir, run + ".png"), "Resolution", 80);
close(f);
end
