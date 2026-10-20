function [x, ref, md, umin, umax, ymin, ymax, ywt, sw] = acc_mpc_prep( ...
    mode, v, a_f, gap, vl, al_eff, th, vset, mv_prev, C)
%ACC_MPC_PREP State, references, online bounds and weights for the MPC (codegen).
%   mode: 0 OFF, 1 CRUISE, 2 FOLLOW, 3 STOP, 4 HOLD, 5 GO, 6 EMERGENCY.
%   x = [gap; v_lead - v; v; a1; a2]: a2 (brake path) is the first-order response of the
%   last applied brake-path request, a1 = measured (filtered) acceleration - a2.
%   FOLLOW-like modes track a shaped closing speed (y2 reference)
%       e = gap - (d0 + th*v_lead)          gap error at the lead's speed
%       e > 0: vr_ref = -min(sqrt(2*a*e + c^2) - c, max(v - v_lead, vr_catchup)), c = a/k:
%              the closing speed from which the ego settles at the target gap with at most
%              a = a_approach of deceleration, linear (slope k_gap) near the target; it never
%              asks to close faster than now unless below the catch-up speed.
%       e < 0: vr_ref = min(k_open*|e|, vr_open_max) (too close: open the gap gently)
%   STOP (lead stopped): target gap d0; below 1 m/s within 3 m of it no throttle (stop there).
%   plus a weak gap term (y1 = gap - th_nom*v -> d0 + (th - th_nom)*v) and the soft minimum
%   gap y1 >= d0_safe + (safe_frac*th - th_nom)*v. CRUISE tracks the set speed. The speed is
%   kept below the set speed by a soft upper bound in every mode.
%#codegen
persistent a2
if isempty(a2), a2 = 0; end
k2 = exp(-C.Ts / C.mpc.tau2);
a2 = k2 * a2 + (1 - k2) * mv_prev(2);
if mode == 0 || mode == 4
    a2 = 0;
end
a1 = a_f - a2;
thn = C.mpc.th_nom;
d0 = C.fol.d0;
p = C.mpc.p;                                   % reference rows: preview over the horizon

% drive path range: no-brake deceleration (coasting + retarder 4) .. comfortable acceleration
ac = acc_coast(v, C);
if v > C.alc.ret_min_v
    u1min = ac - acc_retarder_decel(4, v, C);
else
    u1min = ac;
end
afull = interp1(C.veh.afull_v, C.veh.afull_a, min(max(v, C.veh.afull_v(1)), C.veh.afull_v(end)));
u1max = min(C.mpc.a_max_comf, 0.85 * afull);
if mode == 6
    amin = C.mpc.a_min_emerg;
    sw = 2;
else
    amin = C.mpc.a_min_comf;
    sw = 1;
end
umin = [u1min; amin - u1min];
umax = [u1max; 0];
ymax = [1e3; 1e3; vset + C.fol.vset_margin];
md = al_eff;

follow = mode == 2 || mode == 3 || mode == 5 || mode == 6;
if follow
    x = [gap; vl - v; v; a1; a2];
    if mode == 3
        dt = d0;                                   % lead stopped: stop at the standstill gap
    else
        dt = d0 + th * vl;
    end
    e = gap - dt;
    vr_ref = zeros(p, 1);
    % closing speed that settles at the target gap with at most a_approach of deceleration:
    % sqrt(2*a*e + c^2) - c with c = a/k (linear, slope k, near e = 0); never asks to close
    % faster than now unless below the catch-up speed. The profile is previewed over the
    % horizon: the gap error shrinks at the reference closing speed and grows as the target
    % gap shrinks with the lead's predicted speed (lead braking -> room to brake less).
    k = C.fol.k_gap;
    if mode == 3, k = C.fol.k_stop; end
    c = C.fol.a_approach / k;
    cap = min(max(v - vl, C.fol.vr_catchup), C.fol.vr_ref_max);
    ei = e;
    vli = vl;
    for i = 1:p
        if ei > 0
            prof = sqrt(2 * C.fol.a_approach * ei + c^2) - c;
            vr_ref(i) = -min(prof, cap);
        else
            vr_ref(i) = min(-C.fol.k_open * ei, C.fol.vr_open_max);   % too close: open gently
        end
        vln = max(0, vli + al_eff * C.Ts);
        ei = ei + vr_ref(i) * C.Ts;
        if mode ~= 3
            ei = ei - th * (vln - vli);
        end
        vli = vln;
    end
    if mode == 3 && v < 1.0 && gap < d0 + 3.0
        umax(1) = min(umax(1), ac);           % creeping the last metres is not worth a launch
    end
    if mode ~= 5 && al_eff < 0.2
        umax(1) = min(umax(1), C.mpc.a_max_follow);   % catching up gently unless the lead pulls away
    end
    ref = [repmat(d0 + (th - thn) * v, p, 1), vr_ref, repmat(vset, p, 1)];
    ymin = [C.fol.d0_safe + (C.fol.safe_frac * th - thn) * v; -1e3; -1e3];
    ywt = C.mpc.w_y_follow(:);
else
    x = [d0 + thn * v + 200; 0; v; a1; a2];
    ref = [repmat(d0 + (th - thn) * v + 200, p, 1), zeros(p, 1), repmat(vset, p, 1)];
    ymin = [-1e3; -1e3; -1e3];
    ywt = C.mpc.w_y_cruise(:);
end
% Keep the MV bounds reachable from the last move under the hard rate limits of the active
% controller; otherwise a bound that moves faster than the jerk limit (mode change, speed-
% dependent limits) makes the QP infeasible. The bound then follows at the jerk limit.
if sw == 2, j = C.mpc.jerk_emerg; else, j = C.mpc.jerk_comf; end
umax(1) = max(umax(1), mv_prev(1) - j(1) * C.Ts);
umin(1) = min(umin(1), mv_prev(1) + j(2) * C.Ts);
umax(2) = max(umax(2), mv_prev(2) - j(3) * C.Ts);
umin(2) = min(umin(2), mv_prev(2) + j(4) * C.Ts);
end
