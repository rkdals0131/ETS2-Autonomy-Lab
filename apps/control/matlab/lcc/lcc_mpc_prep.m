function [Ad, Bd, Cm, Dm, x0, md, ref, umin, umax, mv_ext, mode, fb_angle, fb_rate, obs, pred_rows, conv] = ...
    lcc_mpc_prep(enable, v, ey, epsi, preview, delta, wheelbase, trailer, hitch, mem, C)
%LCC_MPC_PREP Inputs of the Adaptive MPC block for one 20 ms step (MATLAB Function "MPC_Prep").
%   Contract inputs: enable, v (speed_mps), ey (lateral_error_m, left +), epsi (heading_error_rad),
%   preview (curvature_preview, 61 points at 1 m from the reference point), delta
%   (wheel_angle_rad, measured: C.act.age_s old), wheelbase (wheelbase_m), trailer
%   (trailer_connected), hitch (hitch_angle_rad). mem = [epsi_pred; kb; mpc_active; delta_pred;
%   ub; u(k-1) .. u(k-nd-1)] of the last step (Unit Delay fed by lcc_mpc_post).
%   Outputs: plant model at this speed (Ad, Bd, Cm, Dm), state x0 (the measured errors and hitch,
%   the wheel angle carried forward over its age with the input then acting, the inputs still on
%   their way), previewed curvature md ((p+1) x 1, midpoint of each step at v*Ts*(k+0.5) ahead,
%   plus the bias kb), references ref (p x 6), input bounds, the MV applied last step, mode
%   (1 MPC, 0 off, 3 low speed, 4 bad input), the fallback law (lateral_controller.hpp
%   linearized pure pursuit), the observer estimates obs = [kb; ub] (curvature bias, rack input
%   bias), the epsi and delta rows of the one-step prediction [Ad(2:3,:) Bd(2:3,:)] for the
%   observers, and conv = [angle_per_unit*a(v), c(v)] of the VirtualEps
%   table (lcc_mpc_post turns the input into its rate feedforward).
%#codegen
p = C.p; Ts = C.Ts;
valid = isfinite(v) && isfinite(ey) && isfinite(epsi) && isfinite(delta) && isfinite(wheelbase) ...
    && all(isfinite(preview)) && isfinite(hitch) && wheelbase > 1.5 && wheelbase < 8;
vs = 0; eys = 0; epsis = 0; ds = 0; Lw = 3.158; hs = 0; pv = zeros(numel(preview), 1);
if valid
    vs = max(v, 0); eys = ey; epsis = epsi; ds = delta; Lw = wheelbase; pv = preview(:);
    if trailer, hs = hitch; end
end
if ~enable
    mode = 0;
elseif ~valid
    mode = 4;
elseif vs < C.v_min
    mode = 3;
else
    mode = 1;
end
active = mem(3) > 0.5;

nh = C.act.nd + 1;
kmh = vs * 3.6;
[ea, ec] = eps_plant(C, kmh);
conv = [C.angle_per_unit * ea, ec];

vm = max(vs, C.v_min);
[Ad, Bd, Cm, Dm, slip, rack] = lcc_mpc_model(vm, Lw, trailer, C);
% inputs on their way (model inputs): the last moves, or (first step) the input that holds the
% measured angle
past = zeros(nh, 1);
if active
    past = mem(6:5 + nh);
else
    past(:) = min(max(rack(2) * ds / rack(1), -C.u_max), C.u_max);
end
% the measured angle is C.act.age_s old: carry it forward with the input that acted meanwhile
j = min(max(round((C.act.delay_s + C.act.age_s / 2) / Ts), 1), nh);
dnow = ds + C.act.age_s * (rack(1) * past(j) - rack(2) * ds);
x0 = [eys; epsis; dnow; hs; past];

% ---------------- observers (integral action) ----------------
% curvature bias kb: yaw-rate disturbance, from the heading error the model predicted last step
% rack input bias ub: the game rack off the VirtualEps table (a, c), from the predicted wheel
% angle (measured below predicted: the rack does less, send more); lcc_mpc_post sends mv + ub so
% that the rack behaves as the model
kb = 0; ub = 0;
if mode == 1 && active
    kb = mem(2); ub = mem(5);
    if vs > C.obs.v_min
        rho = epsis - mem(1);                    % measured - predicted heading error
        kb = min(max(kb - rho / (vs * C.obs.T), -C.obs.max), C.obs.max);
    end
    ub = min(max(ub - (dnow - mem(4)) / (rack(1) * C.obs.T_rack), -C.obs.u_max), C.obs.u_max);
end
obs = [kb; ub];
md = zeros(p + 1, 1);
for k = 0:p
    md(k + 1) = preview_at(pv, vm * Ts * (k + 0.5), C) + kb;
end
ref = zeros(p, 6);
for k = 1:p
    ref(k, 2) = slip * md(k + 1);              % steady heading error of a curve (sideslip)
end
% ---------------- input bounds (VirtualEps clamps |u| at 1; it gets mv + ub) ----------------
umin = -C.u_max - ub; umax = C.u_max - ub;
mv_ext = past(1);
pred_rows = [Ad(2:3, :) Bd(2:3, :)];

% ---------------- fallback: lateral_controller.hpp (curvature feedforward + linearized pure pursuit) ----
d = C.fb.lookahead_m + vs * C.fb.lookahead_time_s;
if trailer
    dL = interp_clamped(C.yaw.kmh, C.yaw.dL_trailer, kmh);
else
    dL = interp_clamped(C.yaw.kmh, C.yaw.dL_truck, kmh);
end
Le = Lw + dL;
kap = pv(1) - 2 * eys / d^2 - 2 * sin(epsis) / d;
kr = vs * (preview_at(pv, 6, C) - pv(1)) / 6;
kk = Le * kap;
fb_angle = atan(kk);
fb_rate = Le * kr / (1 + kk^2);
end

% =====================================================================
function k = preview_at(pv, dist, C)
x = min(max(dist / C.preview_ds, 0), numel(pv) - 1.000001);
i = floor(x); f = x - i;
k = pv(i + 1) * (1 - f) + pv(i + 2) * f;
end

function [a, c] = eps_plant(C, kmh)
a = interp_clamped(C.eps.kmh, C.eps.a, kmh);
c = interp_clamped(C.eps.kmh, C.eps.c, kmh);
end

function y = interp_clamped(xs, ys, x)
n = numel(xs);
if x <= xs(1)
    y = ys(1);
elseif x >= xs(n)
    y = ys(n);
else
    i = 1;
    while x > xs(i + 1), i = i + 1; end
    f = (x - xs(i)) / (xs(i + 1) - xs(i));
    y = ys(i) + f * (ys(i + 1) - ys(i));
end
end
