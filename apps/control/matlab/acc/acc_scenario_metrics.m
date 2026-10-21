function m = acc_scenario_metrics(R, s)
%ACC_SCENARIO_METRICS Safety, comfort and actuator-use numbers of one closed-loop run.
%   R: signals on a 50 ms grid (run_acc_scenarios.m), s: the scenario. Jerk is the
%   derivative of the ego acceleration after a 0.25 s moving average (the plant's
%   acceleration jumps when the clutch opens for a gear change; those 0.73 s windows are
%   reported separately as shift jerk, since the controller cannot remove them; so is the\n%   instant the truck comes to rest, v < 0.3 m/s).
t = R.t; dt = t(2) - t(1);
lv = R.lead_valid > 0.5;
m.name = s.name;
if any(lv)
    m.min_gap_m = min(R.gap(lv));
    closing = lv & (R.v - R.v_lead) > 0.1;
    ttc = inf(size(t)); ttc(closing) = R.gap(closing) ./ (R.v(closing) - R.v_lead(closing));
    m.min_ttc_s = min(ttc);
    if isinf(m.min_ttc_s), m.min_ttc_s = 99; end
else
    m.min_gap_m = NaN; m.min_ttc_s = 99;
end
m.collision = any(lv & R.gap <= 0);
a = movmean(R.a, 5);
jerk = [0; diff(a)] / dt;
shift = false(size(t));
ch = find(diff(R.gear) ~= 0) + 1;
for k = ch.'
    shift(t >= t(k) - 0.6 & t <= t(k) + 0.6) = true;
end
stopping = R.v < 0.3;                              % the last instant of a stop (a jumps to 0)
m.max_abs_jerk_mps3 = max(abs(jerk(~shift & ~stopping)));
m.max_abs_jerk_incl_shifts_mps3 = max(abs(jerk));
m.rms_jerk_mps3 = rms(jerk(~shift));
n1 = round(1/dt);                                % ISO 15622 style: change of acceleration over 1 s
j1 = abs(R.a(1+n1:end) - R.a(1:end-n1));
m.max_jerk_1s_mps3 = max(j1);
m.max_abs_ades_rate_mps3 = max(abs(diff(R.a_des))) / dt;
m.peak_decel_mps2 = max(0, -min(R.a));
m.peak_accel_mps2 = max(R.a);
m.rms_accel_mps2 = rms(R.a);
m.min_a_des_mps2 = min(R.a_des);
bon = R.brake > 0.002 & R.v > 0.1;              % service brake while moving (HOLD excluded)
m.brake_time_s = sum(bon) * dt;
m.brake_applications = sum(diff([0; bon]) > 0);
m.brake_max = max(R.brake);
m.brake_energy_kJ = sum(R.F_brake .* R.v) * dt / 1000;
m.retarder_changes = sum(diff(R.retarder) ~= 0);
m.retarder5_s = sum(R.retarder >= 5) * dt;
m.retarder_max = max(R.retarder);
m.throttle_brake_switches = sum(diff(double(R.throttle > 0.01) - double(bon)) ~= 0 & ...
    abs(diff(double(R.throttle > 0.01) - double(bon))) == 2);
m.gear_changes = numel(ch);
m.modes = unique(R.mode).';
m.final_speed_kmh = R.v(end) * 3.6;
if any(lv), m.final_gap_m = R.gap(end); else, m.final_gap_m = NaN; end
st = find(R.mode == 4, 1);                      % first HOLD
if ~isempty(st)
    m.standstill_gap_m = R.gap(st);
    m.hold_s = sum(R.mode == 4) * dt;
    go = find(R.mode == 5 & t > t(st), 1);
    if ~isempty(go), m.go_time_s = t(go); else, m.go_time_s = NaN; end
else
    m.standstill_gap_m = NaN; m.hold_s = 0; m.go_time_s = NaN;
end
% approach: gap at which the truck starts slowing for a lead it closes on (a_des <= -0.2 m/s^2
% while closing faster than 0.3 m/s); gap settled: the time after which the gap stays within
% max(2 m, 5 %) of d0 + th*v_lead with |v - v_lead| <= 0.5 m/s to the end of the run
ap = find(lv & R.a_des <= -0.2 & (R.v - R.v_lead) > 0.3, 1);
if ~isempty(ap), m.approach_start_gap_m = R.gap(ap); m.approach_start_s = t(ap);
else, m.approach_start_gap_m = NaN; m.approach_start_s = NaN; end
tgt = 5 + s.time_gap * R.v_lead;
ok = lv & abs(R.gap - tgt) <= max(2, 0.05 * tgt) & abs(R.v - R.v_lead) <= 0.5;
if any(lv) && ok(end)
    kb = find(~ok, 1, "last");
    if isempty(kb), m.gap_settled_s = 0; else, m.gap_settled_s = t(kb + 1); end
else
    m.gap_settled_s = NaN;
end
m.emergency = any(R.mode == 6);
% speed tracking in the last 10 s (set-speed scenario) and gap error in the last 10 s
last = t > t(end) - 10;
m.final_speed_err_kmh = mean(R.v(last) - R.set_speed(last)) * 3.6;
if any(lv & last)
    m.final_gap_err_m = mean(R.gap(lv & last) - (5 + s.time_gap * R.v(lv & last)));
else
    m.final_gap_err_m = NaN;
end
end
