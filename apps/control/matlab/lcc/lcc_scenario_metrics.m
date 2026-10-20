function m = lcc_scenario_metrics(R, lane, P)
%LCC_SCENARIO_METRICS Lane-keeping metrics of one run (lane_test.py summarize + more).
%   m = lcc_scenario_metrics(lcc_collect(out), lane, P). As lane_test.py, the error metrics
%   count from 150 m travelled on; curves are |curvature_preview(1)| > 1e-4.
%     max_offset_first150m, max_offset_after150m, rms_offset_after150m (m)
%     mean_offset_in_curves (m, left +), inward_offset_in_curves (m, + = inside of the curve)
%     max_heading_error_deg_after150m
%     max_wheel_rate (rad/s, actual front wheel angle), max_wheel_rate_cmd (rad/s)
%     saturated_ticks (|u| >= 0.999 at the 50 Hz ticks; lane_test counts 60 Hz frames)
%     cmd_sign_reversals (u, lane_test definition), max_ay_corr (|a_y - v^2*kappa|, m/s^2)
%     trailer: rear (last) trailer axle lateral error against the lane polyline:
%     trailer_rear_max_m (after 150 m), trailer_rear_inward_in_curves_m (mean, + = inside),
%     trailer_rear_max_inward_m, trailer_rear_max_outward_m
travelled = R.s - R.s(1);
first = travelled <= 150; after = ~first;
curved = after & abs(R.preview0) > 1e-4;
m.max_offset_first150m = max(abs(R.ey(first)));
m.max_offset_after150m = max([abs(R.ey(after)); 0]);
m.rms_offset_after150m = sqrt(mean(R.ey(after).^2));
m.mean_offset_in_curves = NaN; m.inward_offset_in_curves = NaN;
if any(curved)
    m.mean_offset_in_curves = mean(R.ey(curved));
    m.inward_offset_in_curves = mean(R.ey(curved) .* sign(R.preview0(curved)));
end
m.max_heading_error_deg_after150m = rad2deg(max([abs(R.epsi(after)); 0]));
dd = diff(R.delta) / 0.02;
m.max_wheel_rate = max(abs(dd));
m.max_wheel_rate_cmd = max(abs(R.wheel_rate_cmd));
m.saturated_ticks = sum(abs(R.u) >= 0.999);
u = R.u;
m.cmd_sign_reversals = sum(u(1:end-1) .* u(2:end) < 0 & abs(diff(u)) > 0.05);
v = P.init.speed;
m.max_ay_corr = max(abs(R.ay - v^2 * R.kappa));
m.max_abs_delta = max(abs(R.delta));
m.status_counts = histcounts(R.status, -0.5:1:4.5);
if P.trailer_connected
    a = P.trailer.a; Lr = P.trailer.L_rear_axle;
    hx = R.X - a * cos(R.psi); hy = R.Y - a * sin(R.psi);
    px = hx - Lr * cos(R.psi_t); py = hy - Lr * sin(R.psi_t);
    [e, sp] = lcc_project(lane, px, py, R.s - a - Lr);
    % remove the small drift between the plant's world pose and its lane-frame error (ode4 at
    % the curvature steps, <= 1.5 cm): offset of the truck in the world minus its lane error
    e = e - (lcc_project(lane, R.X, R.Y, R.s) - R.ey);
    k = interp1(lane.s, [0; diff(lane.theta)] / 0.25, sp, "previous", 0);
    ok = after;
    m.trailer_rear_max_m = max(abs(e(ok)));
    inw = e .* sign(k);
    c = ok & abs(k) > 1e-4;
    m.trailer_rear_inward_in_curves_m = NaN;
    m.trailer_rear_max_inward_m = 0; m.trailer_rear_max_outward_m = 0;
    if any(c)
        m.trailer_rear_inward_in_curves_m = mean(inw(c));
        m.trailer_rear_max_inward_m = max(inw(c));
        m.trailer_rear_max_outward_m = max(-inw(c));
    end
    m.trailer_rear_error = e;          % per tick (removed before json)
end
end
