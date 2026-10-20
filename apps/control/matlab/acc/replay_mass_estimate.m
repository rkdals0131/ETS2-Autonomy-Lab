function R = replay_mass_estimate(opts)
%REPLAY_MASS_ESTIMATE Run the mass estimator (acc_mass_estimate.m) on recorded game drives.
%   R = replay_mass_estimate() feeds each recording (acc_load_recording) on the controller's
%   50 ms grid through acc_mass_estimate, the same function the ACC_Controller block calls:
%   speed, acceleration -a_lin(3) (SDK local z points backwards; kinematic, no gravity), rpm,
%   SDK engine gear, applied throttle / brake (eff_thr, eff_brk), SDK retarder step, engine
%   brake flag and a pitch source (opts.Pitch). It reports per run and start value the time
%   until the estimate stays within 5 % of the reference mass, the final estimate and the
%   informative time, and writes results/mass_replay.json and results/mass_replay_<run>.png.
%   Reference masses: engine-torque force balance of the longitudinal fit
%   (longitudinal_fitted_tables.m T.mass, docs/31): trailer runs 19.9-20.6 t, truck alone 11.9 t;
%   the acc-* runs use the same empty trailer (20.3 t); brake-20261020-105939 / -120355 are the
%   truck alone (tag axle up / down, 12.0 t, docs/31).
%   Pitch: "grade" = road grade from the world position over +-10 m of travel (recommended
%   source), "body" = SDK truck pitch (2*pi*rot(2); includes suspension pitch), "none" = 0.
%   Swap: two recordings back to back without resetting the estimator or changing the prior
%   (trailer coupled / uncoupled with mass_prior_kg left at 0), to check the change detector.
arguments
    opts.Runs (1,:) string = ["acc-20261020-192243", "acc-20261020-185529", "acc-20261020-190844", ...
        "gear-20261020-151650", "brake-20261020-152718", "sysid-20261020-153130", "sysid-20261020-144933", ...
        "brake-20261020-105939", "brake-20261020-120355"]
    opts.Pitch (1,:) string = ["grade", "body", "none"]
    opts.Starts (1,:) double = [0 12000 40000]     % 0 = unknown prior (20.3 t default)
    opts.Plot (1,1) logical = true
    opts.Save (1,1) logical = true
    opts.Swap (1,1) logical = true
end
here = fileparts(mfilename("fullpath"));
C = acc_controller_params();
ref = dictionary(["gear-20261020-151650", "brake-20261020-152718", "sysid-20261020-153130", ...
    "sysid-20261020-144933"], [19925 20420 20606 11904]);
R = struct([]);
for run = opts.Runs
    D = acc_load_recording(run);
    m_ref = ref_mass(run, ref);
    X = grid_signals(D, C.Ts);
    curves = struct([]);
    for ps = opts.Pitch
        for m0 = opts.Starts
            Cr = C;
            if m0 > 0, Cr.est.prior_sigma_known = Cr.est.prior_sigma_unknown; end   % far start: as uncertain as the default
            o = run_estimator(X, X.pitch.(ps), m0, Cr);
            r.run = run; r.pitch = ps; r.start_kg = m0; r.reference_kg = m_ref;
            r.duration_s = X.t(end);
            r.informative_s = o.info(end);
            r.final_kg = o.m(end);
            r.final_err_pct = 100 * (o.m(end) / m_ref - 1);
            r.final_sigma_kg = o.sig(end);
            r.valid_at_end = o.valid(end);
            inside = abs(o.m / m_ref - 1) <= 0.05;
            k = find(~inside, 1, "last");
            if isempty(k), r.t_within5_s = 0; elseif k == numel(inside), r.t_within5_s = NaN; else, r.t_within5_s = X.t(k + 1); end
            if isnan(r.t_within5_s), r.info_within5_s = NaN; elseif k > 0, r.info_within5_s = o.info(k + 1); else, r.info_within5_s = 0; end
            kv = find(o.valid, 1);
            if isempty(kv), r.t_valid_s = NaN; else, r.t_valid_s = X.t(kv); end
            r.reopenings = o.nopen(end);
            kb = o.kb(X.brk > 0.01 & X.v > 3);
            r.k_brake_median = median(kb); r.k_brake_range = [min(kb) max(kb)];  % learned brake effectiveness while braking
            r.range_after_valid_kg = [NaN NaN];
            if ~isempty(kv), r.range_after_valid_kg = [min(o.m(kv:end)) max(o.m(kv:end))]; end
            fprintf("%-22s pitch %-5s start %5.0f: final %6.0f kg (%+5.1f %%, sigma %4.0f), within 5 %% from %6.1f s (%5.1f s informative), valid at %6.1f s, informative %5.1f of %4.0f s, re-opened %d\n", ...
                run, ps, m0, r.final_kg, r.final_err_pct, r.final_sigma_kg, r.t_within5_s, r.info_within5_s, r.t_valid_s, r.informative_s, r.duration_s, r.reopenings);
            if isempty(R), R = r; else, R(end+1) = r; end %#ok<AGROW>
            c.pitch = ps; c.start = m0; c.m = o.m; c.used = o.used; c.valid = o.valid;
            if isempty(curves), curves = c; else, curves(end+1) = c; end %#ok<AGROW>
        end
    end
    if opts.Plot
        plot_run(X, curves, m_ref, run, fullfile(here, "results", "mass_replay_" + run + ".png"));
    end
end
W = struct([]);
if opts.Swap
    pairs = ["brake-20261020-105939" "gear-20261020-151650"; "gear-20261020-151650" "brake-20261020-105939"];
    for i = 1:size(pairs, 1)
        X1 = grid_signals(acc_load_recording(pairs(i, 1)), C.Ts);
        X2 = grid_signals(acc_load_recording(pairs(i, 2)), C.Ts);
        X = join_signals(X1, X2, C.Ts);
        o = run_estimator(X, X.pitch.grade, 0, C);
        k0 = numel(X1.t);
        o2 = run_estimator(X2, X2.pitch.grade, 0, C);
        m2 = o2.m(end);                          % what the second recording gives on its own
        out = abs(o.m(k0+1:end) / m2 - 1) > 0.05;
        k = find(out, 1, "last");
        w.first = pairs(i, 1); w.second = pairs(i, 2); w.swap_at_s = X.t(k0 + 1);
        w.mass_before_kg = o.m(k0); w.second_alone_kg = m2; w.final_kg = o.m(end);
        if isempty(k), w.within5_after_s = 0; w.info_within5_after_s = 0;
        elseif k == numel(out), w.within5_after_s = NaN; w.info_within5_after_s = NaN;
        else, w.within5_after_s = X.t(k0 + k + 1) - X.t(k0 + 1); w.info_within5_after_s = o.info(k0 + k + 1) - o.info(k0);
        end
        w.reopenings = o.nopen(end);
        fprintf("swap %s -> %s at %.0f s: %.0f kg before, within 5 %% of %.0f kg after %.1f s (%.1f s informative), final %.0f kg, re-opened %d\n", ...
            w.first, w.second, w.swap_at_s, w.mass_before_kg, m2, w.within5_after_s, w.info_within5_after_s, w.final_kg, w.reopenings);
        if isempty(W), W = w; else, W(end+1) = w; end %#ok<AGROW>
    end
end
if opts.Save
    fid = fopen(fullfile(here, "results", "mass_replay.json"), "w");
    fwrite(fid, jsonencode(struct("runs", R, "swaps", W), PrettyPrint=true)); fclose(fid);
end
end

function m = ref_mass(run, ref)
if isKey(ref, run), m = ref(run); elseif startsWith(run, "brake-20261020-1") && run < "brake-20261020-14", m = 11967; else, m = 20300; end
end

function X = join_signals(X1, X2, Ts)
X = X1;
X.t = [X1.t; X1.t(end) + Ts + X2.t];
for f = ["v", "a", "rpm", "gear", "thr", "brk", "ret", "eb"]
    X.(f) = [X1.(f); X2.(f)];
end
for f = ["body", "none", "grade"]
    X.pitch.(f) = [X1.pitch.(f); X2.pitch.(f)];
end
end

% =====================================================================
function X = grid_signals(D, Ts)
% latest frame at or before each controller sample
X.t = (0:Ts:D.t(end)).';
idx = zeros(size(X.t));
j = 1;
for k = 1:numel(X.t)
    while j < numel(D.t) && D.t(j + 1) <= X.t(k) + 1e-9, j = j + 1; end
    idx(k) = j;
end
X.v = max(D.speed(idx), 0);
X.a = -D.a_lin(idx, 3);
X.a(X.v < 0.1) = 0;                                   % contract: 0 at standstill
X.rpm = D.rpm(idx);
X.gear = D.gear(idx);
X.thr = D.eff_thr(idx);
X.brk = D.eff_brk(idx);
X.ret = D.ret_step(idx); X.ret(~isfinite(X.ret)) = 0;
X.eb = D.motor_flag(idx) > 0.5;
X.pitch.body = 2*pi*D.rot(idx, 2);
X.pitch.none = zeros(size(X.t));
% road grade from the world position: rise over +-10 m of horizontal travel
s = cumsum([0; sqrt(sum(diff(D.pos(:, [1 3])).^2, 2))]);
gr = zeros(size(D.t));
i0 = 1; i1 = 1; n = numel(D.t);
for k = 1:n
    while s(i0) < s(k) - 10, i0 = i0 + 1; end
    while i1 < n && s(i1 + 1) <= s(k) + 10, i1 = i1 + 1; end
    if s(i1) - s(i0) > 15, gr(k) = (D.pos(i1, 2) - D.pos(i0, 2)) / (s(i1) - s(i0)); elseif k > 1, gr(k) = gr(k - 1); end
end
X.pitch.grade = atan(gr(idx));
end

function o = run_estimator(X, pitch, m0, C)
clear acc_mass_estimate
n = numel(X.t);
o.m = zeros(n, 1); o.sig = zeros(n, 1); o.valid = false(n, 1); o.used = false(n, 1); o.info = zeros(n, 1); o.nopen = zeros(n, 1); o.kb = ones(n, 1);
for k = 1:n
    [~, m, s, v, ~, dg] = acc_mass_estimate(X.v(k), X.a(k), X.rpm(k), X.gear(k), X.thr(k), X.brk(k), ...
        X.ret(k), X.eb(k), pitch(k), m0, C);
    o.m(k) = m; o.sig(k) = s; o.valid(k) = v; o.used(k) = dg(1) > 0; o.info(k) = dg(6); o.nopen(k) = dg(7); o.kb(k) = dg(8);
end
clear acc_mass_estimate
end

function plot_run(X, curves, m_ref, run, file)
f = figure("Visible", "off", "Position", [80 80 1100 700]);
try, f.Theme = "light"; catch, end
tl = tiledlayout(f, 3, 1, "TileSpacing", "compact");
title(tl, "Mass estimator replay: " + run, "Interpreter", "none");
ax = nexttile(tl, [2 1]); hold(ax, "on"); grid(ax, "on");
cols = lines(numel(curves));
leg = strings(0);
for i = 1:numel(curves)
    c = curves(i);
    ls = "-"; if c.pitch == "body", ls = "--"; elseif c.pitch == "none", ls = ":"; end
    plot(ax, X.t, c.m / 1000, ls, "Color", cols(i, :), "LineWidth", 1.1);
    leg(end+1) = sprintf("%s, start %s", c.pitch, ternary(c.start > 0, sprintf("%.0f t", c.start/1000), "20.3 t (default)")); %#ok<AGROW>
end
yline(ax, m_ref / 1000, "k", "LineWidth", 1.5); yline(ax, m_ref / 1000 * [0.95 1.05], "k:");
ylabel(ax, "mass t"); legend(ax, [leg "reference"], "Location", "eastoutside", "Interpreter", "none");
ax = nexttile(tl); hold(ax, "on"); grid(ax, "on");
plot(ax, X.t, X.v * 3.6 / 100, "b"); plot(ax, X.t, X.thr, "g"); plot(ax, X.t, X.brk * 5, "r");
u = double(curves(1).used); u(u == 0) = NaN;
plot(ax, X.t, 1.1 * u, "k.", "MarkerSize", 4);
ylabel(ax, "-"); xlabel(ax, "s"); legend(ax, "v/100 km/h", "throttle", "brake x5", "informative", "Location", "eastoutside");
exportgraphics(f, file, "Resolution", 80);
close(f);
end

function s = ternary(c, a, b)
if c, s = a; else, s = b; end
end
