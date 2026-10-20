function V = validate_acc_plant(opts)
%VALIDATE_ACC_PLANT Replay acc-20261020-192243 on acc_plant.slx (automatic gearbox).
%   V = validate_acc_plant() replays every block of the automatic-gearbox run from its
%   recorded starting state with the recorded commands (throttle, brake, retarder step):
%   launches at 0.2/0.4/0.7/1.0, full-throttle accelerations to 68-73 km/h, the retarder
%   ladder 1..5..0 from 73 km/h, the nine retarder/brake blends from 70 km/h and the
%   brake-to-stop blocks. Per block: speed RMS (m/s), 0-50 km/h time, gear sequence.
%   For the ladder and the blends it also computes the deceleration tables the way
%   apps/sysid/analyze_acc.py does (decel = -dv/dt over 0.5 s; ladder: median over the
%   second half of each step; blend: median between 65 and 35 km/h) on the game and the
%   model. Writes results/plant_validation.json and results/plant_*.png.
arguments
    opts.Plot (1,1) logical = true
end
here = fileparts(mfilename("fullpath"));
D = acc_load_recording("acc-20261020-192243");
load_system("acc_plant");
blocks = segment_list(D);
V.blocks = struct([]);
for i = 1:numel(blocks)
    b = blocks(i);
    S = replay_segment(D, b.t0, b.t1);
    r.name = b.name; r.kind = b.kind; r.t0 = b.t0; r.t1 = b.t1;
    r.rms_mps = S.rms;
    r.max_err_mps = max(abs(S.err));
    r.end_err_mps = S.err(end);
    r.t50_rec = first_cross(S.t, S.v_rec, 49.5/3.6);
    r.t50_sim = first_cross(S.t, S.v_sim, 49.5/3.6);
    r.gears_rec = gear_seq(S.gear_rec);
    r.gears_sim = gear_seq(S.gear_sim);
    r.decel_rec = NaN; r.decel_sim = NaN; r.steps = [];
    if b.kind == "blend"
        w = S.phase == b.name;
        r.decel_rec = blend_decel(S.t, S.v_rec, w);
        r.decel_sim = blend_decel(S.t, S.v_sim, w);
    elseif b.kind == "ladder"
        r.steps = ladder_decel(S);
    end
    if isempty(V.blocks), V.blocks = r; else, V.blocks(end+1) = r; end
    fprintf("%-22s rms %.3f m/s  max %.2f  t50 %5.2f/%5.2f  gears rec %s | sim %s", b.name, r.rms_mps, ...
        r.max_err_mps, r.t50_rec, r.t50_sim, r.gears_rec, r.gears_sim);
    if ~isnan(r.decel_rec), fprintf("  decel %.2f/%.2f", r.decel_rec, r.decel_sim); end
    fprintf("\n");
    if b.kind == "ladder"
        for s = r.steps, fprintf("   ladder %s: game %.2f model %.2f m/s^2\n", s.key, s.rec, s.sim); end
    end
    if opts.Plot && ismember(b.kind, ["launch", "ladder", "blend"])
        plot_segment(S, b, fullfile(here, "results", "plant_" + regexprep(b.name, "[^\w]", "_") + ".png"));
    end
end
kinds = unique([V.blocks.kind], "stable");
for k = kinds
    m = [V.blocks.kind] == k;
    V.summary.(k) = struct("n", nnz(m), "rms_mean", mean([V.blocks(m).rms_mps]), ...
        "rms_max", max([V.blocks(m).rms_mps]));
    fprintf("%-8s n=%d  speed rms mean %.3f  max %.3f m/s\n", k, nnz(m), V.summary.(k).rms_mean, V.summary.(k).rms_max);
end
fid = fopen(fullfile(here, "results", "plant_validation.json"), "w");
fwrite(fid, jsonencode(V, PrettyPrint=true)); fclose(fid);
end

% =====================================================================
function B = segment_list(D)
% Blocks of the run, each from its first to its last frame.
B = struct("name", {}, "kind", {}, "t0", {}, "t1", {});
add = @(B, name, kind, t0, t1) [B, struct("name", name, "kind", kind, "t0", t0, "t1", t1)];
for thr = ["0.2", "0.4", "0.7", "1.0"]
    w = find(D.phase == "launch:" + thr);
    if thr == "0.2", w = w(D.t(w) > 44); end   % second try (the first lost focus)
    B = add(B, "launch:" + thr, "launch", D.t(w(1)), D.t(w(end)));
    w = find(D.phase == "brake_to_stop:" + thr);
    B = add(B, "brake_to_stop:" + thr, "stop", D.t(w(1)), D.t(w(end)));
end
w = find(D.phase == "accel:ladder");
B = add(B, "accel:ladder", "accel", D.t(w(1)), D.t(w(end)));
w = find(startsWith(D.phase, "ladder:"));
B = add(B, "ladder", "ladder", D.t(w(1)), D.t(w(end)));
names = unique(D.phase(startsWith(D.phase, "blend:")), "stable");
for n = names.'
    w = find(D.phase == "accel:" + n);
    B = add(B, "accel:" + n, "accel", D.t(w(1)), D.t(w(end)));
    w = find(D.phase == n | D.phase == "release:" + n);
    B = add(B, n, "blend", D.t(w(1)), D.t(w(end)));
end
end

function t = first_cross(tt, v, lim)
k = find(v >= lim, 1);
if isempty(k), t = NaN; else, t = tt(k) - tt(1); end
end

function s = gear_seq(g)
g = g(~isnan(g));
k = [true; diff(g) ~= 0];
s = strjoin(string(g(k)), "-");
end

function d = decel(t, v, window)
d = nan(size(v));
for i = 1:numel(v)
    lo = find(t >= t(i) - window/2, 1); hi = find(t <= t(i) + window/2, 1, "last");
    if hi > lo, d(i) = -(v(hi) - v(lo)) / (t(hi) - t(lo)); end
end
end

function a = blend_decel(t, v, w)
d = decel(t, v, 0.5);
m = w & v < 65/3.6 & v > 35/3.6;
a = median(d(m), "omitnan");
end

function steps = ladder_decel(S)
dr = decel(S.t, S.v_rec, 0.5); ds = decel(S.t, S.v_sim, 0.5);
ch = find(diff(S.ret) ~= 0) + 1;
steps = struct("key", {}, "rec", {}, "sim", {});
for n = 1:numel(ch)
    i = ch(n);
    if n < numel(ch), j = ch(n+1) - 1; else, j = numel(S.ret); end
    h = i + floor((j - i + 1)/2):j;
    key = sprintf("%d->%d@%.0fkmh", S.ret(i-1), S.ret(i), S.v_rec(i)*3.6);
    steps(end+1) = struct("key", key, "rec", median(dr(h), "omitnan"), "sim", median(ds(h), "omitnan")); %#ok<AGROW>
end
end

function plot_segment(S, b, file)
f = figure("Visible", "off", "Position", [100 100 900 640]);
try
    f.Theme = "light";
catch
end
tl = tiledlayout(f, 3, 1, "TileSpacing", "compact");
t = S.t - S.t(1);
ax = nexttile(tl); plot(ax, t, S.v_rec*3.6, "k", t, S.v_sim*3.6, "r"); grid(ax, "on");
ylabel(ax, "km/h"); legend(ax, "game", "model", "Location", "best");
title(ax, sprintf("%s  speed rms %.2f m/s", b.name, S.rms), "Interpreter", "none");
ax = nexttile(tl); stairs(ax, t, S.gear_rec, "k"); hold(ax, "on"); stairs(ax, t, S.gear_sim, "r");
grid(ax, "on"); ylabel(ax, "SDK gear");
ax = nexttile(tl); plot(ax, t, S.rpm_rec, "k", t, S.rpm_sim, "r"); grid(ax, "on");
ylabel(ax, "rpm"); xlabel(ax, "s");
exportgraphics(f, file, "Resolution", 90);
close(f);
end
