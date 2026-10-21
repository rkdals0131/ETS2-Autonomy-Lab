function V = validate_lcc_plant(opts)
%VALIDATE_LCC_PLANT Compare lcc_plant against the game recordings.
%   V = validate_lcc_plant() writes results/plant_validation.json and results/plant_*.png:
%   1. open-loop replays (sysid-20261020-153130 trailer, -113045 truck alone), plant equations
%      (lcc_vehicle_deriv.m, steering table of lcc_plant_params.m) driven by recorded signals:
%        steering   recorded command u -> applied steering eff (rate pulses, sines, turns)
%        yaw        recorded eff -> yaw rate (heading derivative) in the sines and turns
%        trailer    recorded truck yaw rate and speed -> hitch angle and trailer yaw rate
%   2. trailer/truck yaw-rate frequency response of the 12 sines: game vs model
%   3. closed loop: the baseline controller (lcc_baseline.m) on lcc_closed_loop.slx in the 17
%      lane_test.py scenarios at the game's mean speeds, against the game summaries
%      (truck alone lanetest-20261020-121048/-123506, trailer -154003/-155105)
%   4. actuator approximation for the MPC: wheel angle vs the planned wheel angle through the
%      plant's SteeringActuator (VirtualEps + frame delay + measured steering), fitted lag
arguments
    opts.ClosedLoop (1,1) logical = true
end
here = fileparts(mfilename("fullpath"));
cd(here);
V = struct();
runs = ["sysid-20261020-153130", "sysid-20261020-113045"];
kinds = ["trailer", "truck"];
for i = 1:2
    D = lcc_load_recording(runs(i));
    P = lcc_plant_params(kinds(i));
    V.(kinds(i)).replay = replay(D, P);
    V.(kinds(i)).run = runs(i);
end
V.trailer.frequency_response = trailer_frequency(lcc_load_recording(runs(1)), lcc_plant_params("trailer"));
plot_replay(lcc_load_recording(runs(1)), lcc_plant_params("trailer"), fullfile(here, "results", "plant_replay_trailer.png"));
plot_frequency(V.trailer.frequency_response, fullfile(here, "results", "plant_trailer_sines.png"));
V.actuator = actuator_fit();
if opts.ClosedLoop
    V.closed_loop = closed_loop_compare(here);
    plot_closed_loop(V.closed_loop, fullfile(here, "results", "plant_closed_loop.png"));
end
fid = fopen(fullfile(here, "results", "plant_validation.json"), "w");
fwrite(fid, jsonencode(V, PrettyPrint=true)); fclose(fid);
print_summary(V);
end

% =====================================================================
function out = replay(D, P)
% per phase: steering, yaw, trailer replays (frame-by-frame Euler of the plant equations)
dt = [diff(D.t); 0.0167];
r_meas = gradient(D.psi, D.t);
g_meas = D.psi - D.psi_t;
kinds = ["rate_pulse", "rate_hold", "sine", "turn"];
out = struct([]);
ph = unique(D.phase, "stable");
for i = 1:numel(ph)
    if ~any(startsWith(ph(i), kinds)), continue; end
    k = find(D.phase == ph(i));
    if numel(k) < 30, continue; end
    k = k(:);
    n = numel(k);
    % steering: eff from the recorded command, one frame delay
    eff = zeros(n, 1); eff(1) = D.eff(k(1));
    for j = 1:n-1
        v = D.speed(k(j)); kmh = abs(v) * 3.6;
        a = interp1(P.steer.kmh, P.steer.a, min(kmh, P.steer.kmh(end)));
        c = P.steer.c_scale * interp1(P.steer.kmh, P.steer.c, min(kmh, P.steer.kmh(end)));
        u = D.cmd(k(max(j - 1, 1)));
        eff(j+1) = min(max(eff(j) + dt(k(j)) * (a * u - c * eff(j)), -P.geo.eff_max), P.geo.eff_max);
    end
    % yaw rate from the recorded applied steering
    rmod = zeros(n, 1);
    for j = 1:n
        [~, rmod(j)] = lcc_vehicle_deriv(zeros(7, 1), P.geo.wheel_angle_per_unit * D.eff(k(j)), D.speed(k(j)), P.geo.L, 0, P);
    end
    s.phase = ph(i);
    s.kmh = round(mean(D.speed(k)) * 3.6, 1);
    s.eff_fit_pct = fitpct(D.eff(k), eff);
    s.eff_rms = rms(D.eff(k) - eff);
    s.yaw_fit_pct = fitpct(r_meas(k), rmod);
    s.yaw_rms = rms(r_meas(k) - rmod);
    s.hitch_fit_pct = NaN; s.hitch_rms_deg = NaN;
    if P.trailer_connected && all(isfinite(g_meas(k)))
        g = zeros(n, 1); g(1) = g_meas(k(1));
        for j = 1:n-1
            v = D.speed(k(j)); r = r_meas(k(j));
            slip = interp1(P.yaw.kmh, P.yaw.slip, min(abs(v) * 3.6, P.yaw.kmh(end)));
            vy = -slip * r;
            rt = (v * sin(g(j)) + (vy - P.trailer.a * r) * cos(g(j))) / P.trailer.Lt;
            g(j+1) = g(j) + dt(k(j)) * (r - rt);
        end
        s.hitch_fit_pct = fitpct(g_meas(k), g);
        s.hitch_rms_deg = rad2deg(rms(g_meas(k) - g));
    end
    if isempty(out), out = s; else, out(end+1) = s; end %#ok<AGROW>
end
end

function f = fitpct(y, yh)
f = 100 * (1 - norm(y - yh) / max(norm(y - mean(y)), eps));
end

function F = trailer_frequency(D, P)
r = gradient(D.psi, D.t); rt = gradient(D.psi_t, D.t);
F = struct([]);
ph = unique(D.phase, "stable");
for i = 1:numel(ph)
    if ~startsWith(ph(i), "sine"), continue; end
    k = D.phase == ph(i); t = D.t(k) - D.t(find(k, 1));
    p = split(ph(i), ":"); f = str2double(p(3));
    X = [sin(2*pi*f*t) cos(2*pi*f*t) ones(size(t)) t];
    cr = X \ r(k); ct = X \ rt(k); cd = X \ (P.geo.wheel_angle_per_unit * D.eff(k));
    zr = complex(cr(1), cr(2)); zt = complex(ct(1), ct(2)); zd = complex(cd(1), cd(2));
    v = mean(D.speed(k)); w = 2 * pi * f;
    slip = interp1(P.yaw.kmh, P.yaw.slip, v * 3.6);
    a = P.trailer.a + slip; Lt = P.trailer.Lt;
    H = (v - a * 1i * w) / (Lt * 1i * w + v);
    Leff = P.geo.L + interp1(P.yaw.kmh, P.yaw.dL, v * 3.6);
    q.phase = ph(i); q.kmh = round(v * 3.6, 1); q.hz = f;
    q.game_gain = abs(zt / zr); q.game_phase_deg = rad2deg(angle(zt / zr));
    q.model_gain = abs(H); q.model_phase_deg = rad2deg(angle(H));
    q.game_yaw_gain = abs(zr / zd); q.model_yaw_gain = v / Leff;
    q.game_yaw_phase_deg = rad2deg(angle(zr / zd));
    if isempty(F), F = q; else, F(end+1) = q; end %#ok<AGROW>
end
end

function A = actuator_fit()
% planned wheel angle -> actual wheel angle through VirtualEps (50 Hz), one game frame delay and
% the measured steering plant; plans are smooth random steering-rate sequences held for 20 ms
% (as the MPC sends them). Fit a first-order lag d(delta)/dt = (delta_cmd - delta)/tau.
A = struct([]);
P = lcc_plant_params("trailer");
rng(3);
for kmh = [20 40 60 80 100]
    v = kmh / 3.6; Ts = 0.02; h = 0.001; T = 60;
    nk = round(T / Ts);
    amp = 0.02 * (100 / kmh)^1.5;        % comparable lateral acceleration
    rate = filter(0.05, [1 -0.95], randn(nk, 1)); rate = rate / max(abs(rate)) * amp;
    cmd = cumsum(rate) * Ts; cmd = cmd - mean(cmd);
    rate = [diff(cmd) / Ts; 0];
    eff = 0; delay = round(P.steer.delay / h); ubuf = zeros(delay + 1, 1);
    delta = zeros(nk, 1); u = 0;
    a = interp1(P.steer.kmh, P.steer.a, min(kmh, P.steer.kmh(end))); c = P.steer.c_scale * interp1(P.steer.kmh, P.steer.c, min(kmh, P.steer.kmh(end)));
    for k = 1:nk
        delta(k) = P.geo.wheel_angle_per_unit * eff;
        u = lcc_virtual_eps(cmd(min(k + 1, nk)), rate(k), eff, v, P.eps, P.geo.wheel_angle_per_unit);
        for j = 1:round(Ts / h)
            ubuf = [u; ubuf(1:end-1)];
            eff = eff + h * (a * ubuf(end) - c * eff);
        end
    end
    % delta(k) sampled at the tick; planned angle at the tick is cmd(k)
    taus = 0.001:0.002:0.2; err = zeros(size(taus));
    for j = 1:numel(taus)
        dm = lsim_lag(cmd, rate, taus(j), Ts, delta(1));
        err(j) = rms(dm - delta);
    end
    [e, j] = min(err);
    q.kmh = kmh; q.tau_s = taus(j); q.rms_rad = e; q.signal_rms_rad = rms(cmd);
    q.fit_pct = fitpct(delta, lsim_lag(cmd, rate, taus(j), Ts, delta(1)));
    if isempty(A), A = q; else, A(end+1) = q; end %#ok<AGROW>
end
end

function d = lsim_lag(cmd, rate, tau, Ts, d0)
% first-order lag of the planned angle that ramps linearly over each step (as in lcc_mpc_model)
n = numel(cmd); d = zeros(n, 1); d(1) = d0;
Ac = [-1/tau 1/tau; 0 0]; Bc = [0; 1];
M = expm([Ac Bc; 0 0 0] * Ts);
x = [d0; cmd(1)];
for k = 1:n-1
    x = M(1:2, 1:2) * [x(1); cmd(k)] + M(1:2, 3) * rate(k);
    d(k+1) = x(1);
end
end

function CL = closed_loop_compare(here)
S = lcc_scenarios();
for m = ["lcc_plant", "LCC_Controller", "lcc_closed_loop"]
    if ~isfile(fullfile(here, m + ".slx"))
        feval("build_" + lower(regexprep(m, "^LCC_", "lcc_")));
    end
end
load_system("lcc_closed_loop");
game.trailer = [read_game("lanetest-20261020-154003"), read_game("lanetest-20261020-155105")];
game.truck = [read_game("lanetest-20261020-121048"), read_game("lanetest-20261020-123506")];
CL = struct([]);
sysid_delay = lcc_plant_params().steer.delay;   % the recordings were made with the sysid-era delay
for kind = ["trailer", "truck"]
    G = game.(kind);
    for s = S
        g = G([G.index] == s.index);
        if isempty(g), continue; end
        g = g(end);
        % noise-free (steady curve offsets) and with the slow yaw-rate wander (straight RMS)
        [in, lane, P] = lcc_scenario_input(s, kind, 1, Kmh=g.kmh_mean, Delay=sysid_delay);
        m = lcc_scenario_metrics(lcc_collect(sim(in)), lane, P);
        [in, lane, P] = lcc_scenario_input(s, kind, 1, Kmh=g.kmh_mean, Wander=P.dist.amp_game, Delay=sysid_delay);
        mw = lcc_scenario_metrics(lcc_collect(sim(in)), lane, P);
        q.config = kind; q.name = s.name; q.kmh = g.kmh_mean;
        q.game_max_after150 = g.max_offset_after150m; q.model_max_after150 = m.max_offset_after150m;
        q.model_wander_max_after150 = mw.max_offset_after150m;
        q.game_rms_after150 = g.rms_offset_after150m; q.model_rms_after150 = m.rms_offset_after150m;
        q.model_wander_rms_after150 = mw.rms_offset_after150m;
        q.game_inward_curves = g.inward; q.model_inward_curves = m.inward_offset_in_curves;
        q.game_saturated_frames = g.saturated_frames; q.model_saturated_ticks = m.saturated_ticks;
        q.game_max_first150 = g.max_offset_first150m; q.model_max_first150 = m.max_offset_first150m;
        if isempty(CL), CL = q; else, CL(end+1) = q; end %#ok<AGROW>
    end
end
end

function G = read_game(run)
% lane_test summary + inward curve offset and mean speed from lateral.jsonl
here = fileparts(mfilename("fullpath"));
J = jsondecode(fileread(fullfile(here, "..", "..", "..", "..", "bridge", "recordings", run, "summary.json")));
D = lcc_load_recording(run);
L = D.lat;
G = struct([]);
sc = unique(L.scenario).';
for i = 1:numel(J)
    j = J(i);
    if iscell(J), j = J{i}; end
    if ~isfield(j, "max_offset_after150m"), continue; end
    idx = sc(i);                            % summary order = scenario order of the run
    k = find(L.scenario == idx);
    tr = L.remaining(k(1)) - L.remaining(k);
    after = k(tr > 150);
    curved = after(abs(L.path_curvature(after)) > 1e-4);
    q.index = idx; q.name = string(j.name);
    q.kmh_mean = mean(L.speed(after)) * 3.6;
    q.max_offset_after150m = j.max_offset_after150m;
    q.rms_offset_after150m = j.rms_offset_after150m;
    q.max_offset_first150m = j.max_offset_first150m;
    q.saturated_frames = j.saturated_frames;
    q.inward = NaN;
    if ~isempty(curved), q.inward = mean(L.offset(curved) .* sign(L.path_curvature(curved))); end
    if isempty(G), G = q; else, G(end+1) = q; end %#ok<AGROW>
end
end

function plot_replay(D, P, file)
k = find(D.phase == "sine:60:0.2:0.029" | D.phase == "sine:60:0.5:0.029" | D.phase == "sine:60:1.0:0.029");
t = D.t(k) - D.t(k(1));
r = gradient(D.psi, D.t); g = D.psi - D.psi_t;
rm = zeros(size(k)); gm = zeros(size(k)); gm(1) = g(k(1));
for j = 1:numel(k)
    [~, rm(j)] = lcc_vehicle_deriv(zeros(7, 1), 0.7 * D.eff(k(j)), D.speed(k(j)), P.geo.L, 0, P);
    if j > 1
        v = D.speed(k(j-1)); rr = r(k(j-1)); slip = interp1(P.yaw.kmh, P.yaw.slip, v * 3.6);
        rt = (v * sin(gm(j-1)) + (-slip * rr - P.trailer.a * rr) * cos(gm(j-1))) / P.trailer.Lt;
        gm(j) = gm(j-1) + (D.t(k(j)) - D.t(k(j-1))) * (rr - rt);
    end
end
f = figure("Visible", "off", "Position", [100 100 1000 600]);
subplot(2, 1, 1); plot(t, r(k), t, rm, "--"); ylabel("yaw rate rad/s"); legend("game (heading rate)", "plant");
title("sysid-20261020-153130, trailer, 60 km/h sines 0.2/0.5/1.0 Hz"); grid on;
subplot(2, 1, 2); plot(t, rad2deg(g(k)), t, rad2deg(gm), "--"); ylabel("hitch angle deg"); xlabel("s");
legend("game", "plant (driven by the game truck yaw rate)"); grid on;
exportgraphics(f, file); close(f);
end

function plot_frequency(F, file)
f = figure("Visible", "off", "Position", [100 100 900 500]);
kmh = unique([F.kmh]);
for i = 1:numel(kmh)
    q = F([F.kmh] == kmh(i));
    subplot(1, 2, 1); hold on; plot([q.hz], [q.game_gain], "o-", "DisplayName", sprintf("game %g km/h", kmh(i)));
    plot([q.hz], [q.model_gain], "x--", "DisplayName", "model"); ylabel("trailer/truck yaw-rate gain"); xlabel("Hz"); grid on;
    subplot(1, 2, 2); hold on; plot([q.hz], [q.game_phase_deg], "o-"); plot([q.hz], [q.model_phase_deg], "x--");
    ylabel("phase deg"); xlabel("Hz"); grid on;
end
subplot(1, 2, 1); legend("Location", "southwest");
exportgraphics(f, file); close(f);
end

function plot_closed_loop(CL, file)
f = figure("Visible", "off", "Position", [100 100 1100 500]);
for i = 1:2
    kind = ["truck", "trailer"]; q = CL([CL.config] == kind(i));
    c = ~isnan([q.game_inward_curves]);
    subplot(1, 2, i);
    bar(1000 * [[q(c).game_inward_curves].' [q(c).model_inward_curves].']);
    set(gca, "XTickLabel", [q(c).name], "XTickLabelRotation", 45, "TickLabelInterpreter", "none"); grid on;
    ylabel("mean curve offset mm (+ inward)"); legend("game", "plant + baseline"); title(kind(i));
end
exportgraphics(f, file); close(f);
end

function print_summary(V)
sysid_delay = lcc_plant_params().steer.delay;   % the recordings were made with the sysid-era delay
for kind = ["trailer", "truck"]
    fprintf("== %s replay (%s)\n", kind, V.(kind).run);
    for s = V.(kind).replay
        fprintf("  %-26s %5.1f km/h  eff fit %5.1f %%  yaw fit %5.1f %%  hitch fit %5.1f %%\n", s.phase, s.kmh, s.eff_fit_pct, s.yaw_fit_pct, s.hitch_fit_pct);
    end
end
fprintf("== trailer/truck yaw-rate response (game vs model)\n");
for q = V.trailer.frequency_response
    fprintf("  %5.1f km/h %.1f Hz  gain %.3f / %.3f  phase %6.1f / %6.1f deg\n", q.kmh, q.hz, q.game_gain, q.model_gain, q.game_phase_deg, q.model_phase_deg);
end
fprintf("== actuator lag fit\n");
for q = V.actuator
    fprintf("  %3d km/h tau %.3f s  fit %.1f %%\n", q.kmh, q.tau_s, q.fit_pct);
end
if isfield(V, "closed_loop")
    fprintf("== closed loop, baseline: game / plant / plant with wander (mm)\n");
    for q = V.closed_loop
        fprintf("  %-8s %-16s %5.1f km/h  max %5.1f / %5.1f / %5.1f  rms %5.1f / %5.1f / %5.1f  inward %6.1f / %6.1f  sat %d / %d\n", ...
            q.config, q.name, q.kmh, 1000*q.game_max_after150, 1000*q.model_max_after150, 1000*q.model_wander_max_after150, ...
            1000*q.game_rms_after150, 1000*q.model_rms_after150, 1000*q.model_wander_rms_after150, ...
            1000*q.game_inward_curves, 1000*q.model_inward_curves, q.game_saturated_frames, q.model_saturated_ticks);
    end
end
end
