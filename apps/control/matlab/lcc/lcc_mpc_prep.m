function [Ad, Bd, Cm, Dm, x0, md, ref, umin, umax, mv_ext, mode, fb_angle, fb_rate, kb, epsi_row] = ...
    lcc_mpc_prep(enable, v, ey, epsi, preview, delta, wheelbase, trailer, hitch, mem, C)
%LCC_MPC_PREP Inputs of the Adaptive MPC block for one 20 ms step (MATLAB Function "MPC_Prep").
%   Contract inputs: enable, v (speed_mps), ey (lateral_error_m, left +), epsi (heading_error_rad),
%   preview (curvature_preview, 61 points at 1 m from the reference point), delta
%   (wheel_angle_rad, actual), wheelbase (wheelbase_m), trailer (trailer_connected), hitch
%   (hitch_angle_rad). mem = [delta_cmd; mv; epsi_pred; kb; mpc_active] of the last step
%   (Unit Delay fed by lcc_mpc_post).
%   Outputs: plant model at this speed (Ad, Bd, Cm, Dm), measured state x0, previewed
%   curvature md ((p+1) x 1, midpoint of each step at v*Ts*(k+0.5) ahead, plus the bias kb),
%   references ref (p x 5), steering-rate bounds, the MV applied last step, mode
%   (1 MPC, 0 off, 3 low speed, 4 bad input), the fallback law (lateral_controller.hpp
%   linearized pure pursuit), the bias estimate kb and the epsi row of the one-step prediction
%   [Ad(2,:) Bd(2,:)] for the observer.
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
active = mem(5) > 0.5;

% ---------------- curvature bias observer (yaw-rate disturbance) ----------------
kb = 0;
if mode == 1 && active
    kb = mem(4);
    if vs > C.obs.v_min
        rho = epsis - mem(3);                    % measured - predicted heading error
        kb = min(max(kb - rho / (vs * C.obs.T), -C.obs.max), C.obs.max);
    end
end

% ---------------- model, state, preview ----------------
vm = max(vs, C.v_min);
[Ad, Bd, Cm, Dm, slip] = lcc_mpc_model(vm, Lw, trailer, C);
dcmd = ds;
if active, dcmd = mem(1); end
x0 = [eys; epsis; ds; dcmd; hs];
md = zeros(p + 1, 1);
for k = 0:p
    md(k + 1) = preview_at(pv, vm * Ts * (k + 0.5), C) + kb;
end
ref = zeros(p, 5);
for k = 1:p
    ref(k, 2) = slip * md(k + 1);              % steady heading error of a curve (sideslip)
end
% ---------------- steering-rate bounds (VirtualEps |u| <= 1) ----------------
kmh = vs * 3.6;
[a, c] = eps_plant(C, kmh);
rate = C.rate_margin * C.angle_per_unit * max(a - c * abs(ds) / C.angle_per_unit, 0.1 * a);
umin = -rate; umax = rate;
mv_ext = 0;
if active, mv_ext = mem(2); end
epsi_row = [Ad(2, :) Bd(2, :)];

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
