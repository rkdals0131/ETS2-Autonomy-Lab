function [m_use, m_est, m_sigma, valid, k_brake, diag] = acc_mass_estimate(v, a, rpm, gear, thr, brk, ret, eb, pitch, prior, C)
%ACC_MASS_ESTIMATE Online vehicle mass from the identified drive-force model (codegen).
%   [m_use, m_est, m_sigma, valid, diag] = acc_mass_estimate(v, a, rpm, gear, thr, brk, ret,
%   eb, pitch, prior, C) is called once per controller step (C.Ts) with the ego speed v (m/s),
%   the measured longitudinal acceleration a (m/s^2, dv/dt, gravity not included), engine rpm,
%   SDK engine gear (0 neutral), the applied throttle, service brake, retarder step and engine
%   brake flag, the road pitch (rad, nose up positive; non-finite = unknown -> 0) and the mass
%   prior (kg; <= 0 or non-finite = unknown -> C.est.m_ref).
%
%   Model (force balance with the clutch closed, per unit of the reference mass m_ref):
%       y   = a + g*(crr + sin(pitch))
%       phi = (lag(F_drive - c_air*v^2) - J*(ratio*fd/r)^2*a) / m_ref
%       y   = theta*phi,   theta = m_ref/m
%   F_drive = eta*ratio*fd/r*Te, Te from the throttle through the part-load map, the full-load
%   curve and the friction line, rate-limited upward by the turbo-lag table (C.veh.rate_up);
%   lag() is the 0.2 s body lag. Both sides pass the same first-order filter (C.est.tau_f).
%   theta is estimated by scalar recursive least squares with forgetting (C.est.T_mem s of
%   informative data), residuals clipped at C.est.innov_gate predicted sigmas. A sample is used
%   only if the informative conditions (throttle >= thr_min, no brake / retarder / engine
%   brake, gear 1-14 with the clutch closed, same gear, rpm and speed range, torque model close
%   to the request, phi >= phi_min, |pitch| small) held for t_pre s before it and t_post s after
%   it (a delay line of t_post s). Otherwise theta and P are held.
%
%   m_use: mass for the controller (rate-limited estimate; C.veh.m when C.est.adaptive is
%   false). m_est: estimate (kg), m_sigma: 1-sigma (kg, from P with the larger of the assumed
%   and the observed residual sigma),
%   valid: converged on driving data (informative time >= valid_time, m_sigma/m_est <=
%   valid_sigma). diag = [used, y, phi, theta, P, informative time, re-openings, k_brake]
%   for analysis.
%   k_brake: service-brake deceleration per unit input relative to the measured table
%   (C.veh.brk_a). Before any braking it is acc_brake_scale(C, m_use); while the brake input has
%   been on for kb_hold s (no retarder 5 / engine brake, > 3 m/s) it is learned from the
%   measured deceleration minus coasting and the retarder share against the table deceleration
%   of the applied input passed through the response lag (kb_lag), first order kb_tau, at most
%   the scaled table (only weaker), so an
%   unknown heavy truck that brakes before it ever accelerated gets the input it needs within
%   about 1-1.5 s. Once learned it is kept (it already contains the mass); a restart or a
%   re-opening of the mass estimate forgets it. C.est.adaptive = false: k_brake = 1.
%   A persistent one-sided residual (change_z over change_T s) re-opens P to the prior's
%   (mass changed without a new prior).
%#codegen
persistent init theta P s2 t_info m_prev_prior Te_m F_lag yf phif yb pb w cnt gear_last m_out zbar n_open
persistent kb_tot kb_n bh bt pred_f meas_f
Ts = C.Ts;
E = C.est;
nd = round(E.t_post / Ts);                 % delay line length (samples)
npre = round(E.t_pre / Ts);
nb = nd + 1;
mref = E.m_ref;
if isempty(init)
    init = false; theta = 1; P = 1; s2 = E.noise^2; t_info = 0; m_prev_prior = -1;
    Te_m = 0; F_lag = 0; yf = 0; phif = 0; yb = zeros(1, nb); pb = zeros(1, nb); w = 1;
    cnt = 0; gear_last = 0; m_out = mref; zbar = 0; n_open = 0;
    kb_tot = 1; kb_n = 0; bh = 0; bt = 0; pred_f = 0; meas_f = 0;
end

% ---------------- prior and (re)start ----------------
prior_ok = isfinite(prior) && prior > 0;
if prior_ok
    m0 = min(max(prior, E.m_min), E.m_max);
    sig0 = E.prior_sigma_known;
else
    m0 = mref;
    sig0 = E.prior_sigma_unknown;
end
if ~init || abs(m0 - m_prev_prior) > E.prior_reset_kg
    init = true;
    m_prev_prior = m0;
    theta = mref / m0;
    P = (sig0 * theta / E.noise)^2;
    s2 = E.noise^2;
    t_info = 0;
    cnt = 0;
    m_out = m0;
    zbar = 0;
    kb_n = 0;
end
P0 = (sig0 * mref / m0 / E.noise)^2;

% ---------------- drive force model ----------------
g = round(gear);
geared = g >= 1 && g <= numel(C.veh.ratio);
if geared
    ratio = C.veh.ratio(g);
else
    ratio = 0;
end
kw = ratio * C.veh.fd / C.veh.r;                     % wheel force per engine torque / eta
rr = min(max(rpm, C.veh.curve_rpm(1)), C.veh.curve_rpm(end));
full = C.veh.T_max * interp1(C.veh.curve_rpm, C.veh.curve_frac, rr);
fric = -(C.veh.fric0 + C.veh.fric1 * rr);
th = min(max(thr, 0), 1);
if ~isfinite(th), th = 0; end
Te_req = fric + (1 - (1 - th)^C.veh.thr_shape) * (full - fric);
if geared && Te_req > Te_m
    rate = interp2(C.veh.rate_rpm_bp, C.veh.rate_te_bp, C.veh.rate_up, ...
        min(max(rr, C.veh.rate_rpm_bp(1)), C.veh.rate_rpm_bp(end)), ...
        min(max(Te_m, C.veh.rate_te_bp(1)), C.veh.rate_te_bp(end)));
    Te_m = min(Te_req, Te_m + rate * Ts);
else
    Te_m = Te_req;                                   % drops within a frame; clutch open: free
end
vv = max(v, 0);
if ~isfinite(vv), vv = 0; end
F_raw = C.veh.eta * kw * Te_m - C.veh.c_air * vv^2;
ab = 1 - exp(-Ts / C.veh.tau_body);
F_lag = F_lag + ab * (F_raw - F_lag);

a_ok = isfinite(a);
am = a;
if ~a_ok, am = 0; end
pitch_ok = isfinite(pitch);
pt = pitch;
if ~pitch_ok, pt = 0; end
crr_on = min(1, vv * 10);
y_raw = am + C.veh.g * (C.veh.crr * crr_on + sin(pt));
phi_raw = (F_lag - C.veh.J_locked * kw^2 * am) / mref;
af = 1 - exp(-Ts / E.tau_f);
yf = yf + af * (y_raw - yf);
phif = phif + af * (phi_raw - phif);

% ---------------- informative window ----------------
locked = false;
if geared
    rpm_road = vv * kw * 60 / (2*pi);
    locked = abs(rpm - rpm_road) <= E.slip_rel * rpm_road + E.slip_abs;
end
gate = geared && locked && g == gear_last && vv >= E.v_min && th >= E.thr_min && ...
    ~(brk > E.brk_max) && ret == 0 && ~eb && rpm >= E.rpm_min && rpm <= E.rpm_max && ...
    Te_req - Te_m <= E.te_lag_max && phi_raw >= E.phi_min && abs(pt) <= E.pitch_max && a_ok;
gear_last = g;
if gate
    cnt = min(cnt + 1, npre + nb + 1);
else
    cnt = 0;
end
yb(w) = yf; pb(w) = phif;
w = w + 1;
if w > nb, w = 1; end                                % w now points at the sample from nd steps ago

used = cnt >= npre + nb;
if used
    yk = yb(w); pk = pb(w);
    lam = 1 - Ts / E.T_mem;
    sp = max(s2, (E.noise / 3)^2) * (1 + pk^2 * P);  % predicted residual variance
    e = yk - theta * pk;
    % change detection: a persistent one-sided residual (mean normalised residual over about
    % change_T s of informative data beyond change_z) re-opens the covariance, e.g. a trailer
    % coupled or loaded without a new mass_prior_kg; otherwise the clipped, converged RLS would
    % need minutes to move
    zbar = zbar + Ts / E.change_T * (e / sqrt(sp) - zbar);
    if t_info >= E.valid_time && abs(zbar) > E.change_z
        P = P0;
        zbar = 0;
        n_open = n_open + 1;
        kb_n = 0;
        sp = max(s2, (E.noise / 3)^2) * (1 + pk^2 * P);
    end
    lim = E.innov_gate * sqrt(sp);
    e = min(max(e, -lim), lim);
    K = P * pk / (lam + pk^2 * P);
    theta = theta + K * e;
    P = min(P / (lam + pk^2 * P), P0);
    theta = min(max(theta, mref / E.m_max), mref / E.m_min);
    s2 = lam * s2 + (1 - lam) * e^2;
    t_info = t_info + Ts;
end

m_est = mref / theta;
m_sigma = m_est * max(E.noise, sqrt(s2)) * sqrt(P) / theta;
valid = t_info >= E.valid_time && m_sigma <= E.valid_sigma * m_est;
step = E.rate_kgps * Ts;
m_out = m_out + min(max(m_est - m_out, -step), step);
if E.adaptive
    m_use = m_out;
else
    m_use = C.veh.m;
end
% ---------------- service brake effectiveness ----------------
b = brk;
if ~isfinite(b), b = 0; end
if b >= 0.01
    bt = bt + Ts;
else
    bt = 0;
end
bh = b;
ksc = acc_brake_scale(C, m_use);
pred = acc_brake_decel(b, vv, C);                    % table, reference mass
if ret >= 1 && ret <= 4
    kret = C.veh.ret_blend;
    retd = acc_retarder_decel(ret, vv, C, m_use);
else
    kret = 0; retd = 0;
end
meas = (acc_coast(vv, C, m_use, g, rpm) - am) - kret * retd;
apr = 1 - exp(-Ts / E.kb_lag);                      % brake + body + command lag of the response
ame = 1 - exp(-Ts / E.kb_filter);
pred_f = pred_f + apr * (pred - pred_f);
meas_f = meas_f + ame * (meas - meas_f);
if bt >= E.kb_hold && b >= 0.01 && pred_f * ksc >= E.kb_pred_min && vv > 3 && ret <= 4 && ~eb && a_ok
    if kb_n == 0, kb_tot = ksc; end
    r = min(max(meas_f / pred_f, E.kb_min), E.kb_max_rel * ksc);   % never stronger than the scaled table (kept conservative)
    kb_tot = kb_tot + Ts / E.kb_tau * (r - kb_tot);
    kb_n = kb_n + 1;
end
if ~E.adaptive
    k_brake = 1;
elseif kb_n > 0
    k_brake = kb_tot;
else
    k_brake = ksc;
end
diag = [double(used), yb(w), pb(w), theta, P, t_info, n_open, k_brake];
end
