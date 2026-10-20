function thr = acc_inverse_throttle(a_req, v, gear, rpm, C)
%ACC_INVERSE_THROTTLE Throttle for a steady acceleration a_req in the current gear (codegen).
%   Engine torque needed = (m_eff*a_req + rolling + air) / (eta*ratio*fd/r); throttle from
%   the part-load map (Te - fric) = (1 - (1 - thr)^1.33)*(full(rpm) - fric) (docs/31 엔진).
%   In neutral (SDK gear 0, standstill launch) gear C.veh.launch_gear at C.veh.launch_rpm.
%#codegen
g = round(gear);
if g < 1 || g > numel(C.veh.ratio)
    g = C.veh.launch_gear;
    r = C.veh.launch_rpm;
else
    r = max(rpm, v * C.veh.fd / C.veh.r * 60 / (2*pi) * C.veh.ratio(g));
    if r < 700, r = C.veh.launch_rpm; end
end
ratio = C.veh.ratio(g);
kf = C.veh.eta * ratio * C.veh.fd / C.veh.r;
m_eff = C.veh.m + 4.6 * (ratio * C.veh.fd / C.veh.r)^2;   % engine-side inertia 4.6 kg m^2 (longitudinal fit)
F_res = C.veh.c_air * v^2 + C.veh.crr * C.veh.m * C.veh.g;
Te = (m_eff * a_req + F_res) / kf;
rr = min(max(r, C.veh.curve_rpm(1)), C.veh.curve_rpm(end));
full = C.veh.T_max * interp1(C.veh.curve_rpm, C.veh.curve_frac, rr);
fric = -(C.veh.fric0 + C.veh.fric1 * rr);
f = (Te - fric) / max(full - fric, 1);
f = min(max(f, 0), 1);
thr = 1 - (1 - f)^(1 / C.veh.thr_shape);
end
