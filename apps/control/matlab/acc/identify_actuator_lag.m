function R = identify_actuator_lag()
%IDENTIFY_ACTUATOR_LAG First-order lag (+ dead time) from commanded to measured acceleration.
%   R = identify_actuator_lag() fits process models (System Identification Toolbox,
%   procest "P1D": K*exp(-Td*s)/(1 + Tp1*s)) to game recordings:
%     brake   : acc-20261020-192243 blend:0:0.02/0.05/0.1 (service brake step at 70 km/h,
%               retarder 0). Input = net deceleration the brake table promises for the step
%               (P.acc.brk_target), output = measured acceleration change (Savitzky-Golay).
%     throttle: acc-20261020-185529 tipin:4/6/8 (throttle 0 -> 0.8 at about 1,150 rpm, gear
%               held by the harness). Input = steady acceleration change the engine map
%               promises at the start speed (part-load map, full-load curve, eta*ratio*fd/r),
%               output = measured acceleration change.
%     retarder: acc-20261020-192243 ladder steps 1->2->3->4 (input = model step decel).
%   The MPC model uses one lag per path (drive/retarder path and brake path), chosen from
%   these game numbers (acc_controller_params.m: 0.5 s and 0.33 s); identify_closed_loop_lag.m
%   checks the lag through the allocator in closed loop. Writes results/actuator_lag.json.
here = fileparts(mfilename("fullpath"));
P = acc_plant_params();
R = struct("brake", [], "throttle", [], "retarder", []);

% ---------------- brake steps -------------------------------------------------------------
D = acc_load_recording("acc-20261020-192243");
acc = sg_derivative(D.speed, D.t, 11);
for b = ["0.02", "0.05", "0.1"]
    k = find(D.phase == "blend:0:" + b);
    kb = k(find(D.cmd_brk(k) > 0, 1));
    w = (kb - 60):(kb + 180);
    a0 = mean(acc(kb-60:kb-5), "omitnan");
    target = -interp2(P.acc.brk_b, P.acc.brk_v, P.acc.brk_target, str2double(b), D.speed(kb));
    u = target * (D.cmd_brk(w) > 0);
    y = acc(w) - a0;
    R.brake = [R.brake, fit_p1d(u, y, D.t(w), "brake " + b)];
end

% ---------------- throttle tip-ins (sequential gearbox, fixed gear) -----------------------
D2 = acc_load_recording("acc-20261020-185529");
acc2 = sg_derivative(D2.speed, D2.t, 11);
names = unique(D2.phase(startsWith(D2.phase, "tipin:")), "stable");
for n = names.'
    k = find(D2.phase == n);
    k0 = k(1);
    g = D2.gear(k0);
    if g < 1 || D2.ratio(k0) < 0.5, continue; end
    w = (k0 - 30):k(end);
    rpm0 = D2.rpm(k0);
    full = interp1(P.eng.curve_rpm, P.eng.T_max * P.eng.curve_frac, rpm0);
    fric = P.eng.fric0 + P.eng.fric1 * rpm0;
    Te08 = fric + interp2(P.eng.rpm_bp, P.eng.thr_bp, P.eng.frac_table, rpm0, 0.8) * (full - fric);
    k_f = P.gb.eta * D2.ratio(k0) * P.veh.fd / P.veh.r;
    m_eff = P.veh.m + P.gb.J_locked * (D2.ratio(k0) * P.veh.fd / P.veh.r)^2;
    du = k_f * (Te08 - fric) / m_eff;
    a0 = mean(acc2(k0-30:k0-3), "omitnan");
    u = du * (D2.t(w) >= D2.t(k0));
    y = acc2(w) - a0;
    R.throttle = [R.throttle, fit_p1d(u, y, D2.t(w), string(n) + sprintf(" (gear %d, %.0f rpm)", g, rpm0))];
end

% ---------------- retarder ladder steps 1..4 (same frame in the SDK) -----------------------
lad = startsWith(D.phase, "ladder:");
ch = find(diff(D.ret_step) ~= 0 & D.ret_step(2:end) <= 4 & D.ret_step(2:end) > D.ret_step(1:end-1) & lad(2:end)) + 1;
for kb = ch.'
    s = D.ret_step(kb);
    if s < 2, continue; end
    w = (kb - 40):(kb + 100);
    a0 = mean(acc(kb-40:kb-3), "omitnan");
    wo = D.speed(kb) / P.veh.r * P.veh.fd;
    slope = P.ret.table(:, end).' / P.ret.w_bp(end);      % Nm per output-shaft rad/s per level
    ds = interp1(P.ret.level_bp, slope, 0.2*s) - interp1(P.ret.level_bp, slope, 0.2*(s-1));
    du = ds * wo * P.veh.fd / P.veh.r / P.veh.m;
    u = du * (D.t(w) >= D.t(kb));
    R.retarder = [R.retarder, fit_p1d(u, acc(w) - a0, D.t(w), sprintf("retarder %d->%d", s-1, s))];
end
% summary over the fits that explain the step (fit >= 50 %)
for f = ["brake", "throttle", "retarder"]
    r = R.(f); r = r([r.fit_pct] >= 50);
    R.summary.(f) = struct("n", numel(r), "Tp1_median_s", median([r.Tp1_s]), "Td_median_s", median([r.Td_s]), ...
        "lag_median_s", median([r.Tp1_s] + [r.Td_s]), "K_median", median([r.K]));
    fprintf("%-9s n=%d  Tp1 %.2f s  Td %.2f s  Tp1+Td %.2f s  K %.2f (median of fits >= 50 %%)\n", f, numel(r), ...
        R.summary.(f).Tp1_median_s, R.summary.(f).Td_median_s, R.summary.(f).lag_median_s, R.summary.(f).K_median);
end
fid = fopen(fullfile(here, "results", "actuator_lag.json"), "w");
fwrite(fid, jsonencode(R, PrettyPrint=true)); fclose(fid);
end

function r = fit_p1d(u, y, t, name)
ok = ~isnan(y) & ~isnan(u);
u = u(ok); y = y(ok); t = t(ok);
Ts = median(diff(t));
tu = (t(1):Ts:t(end)).';
data = iddata(interp1(t, y, tu), interp1(t, u, tu), Ts);
opt = procestOptions("Display", "off");
sys = idproc("P1D");
sys.Structure.Td.Maximum = 0.5;
sys.Structure.Tp1.Maximum = 3;
sys = procest(data, sys, opt);
[~, fitPct] = compare(data, sys);
r = struct("name", name, "K", sys.Kp, "Tp1_s", sys.Tp1, "Td_s", sys.Td, "fit_pct", fitPct);
fprintf("%-34s K %.2f  Tp1 %.3f s  Td %.3f s  fit %.0f %%\n", name, sys.Kp, sys.Tp1, sys.Td, fitPct);
end
