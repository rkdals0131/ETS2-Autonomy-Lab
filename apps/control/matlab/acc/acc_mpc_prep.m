function [x, ref, md, umin, umax, ymin, ymax, ywt, sw] = acc_mpc_prep( ...
    mode, v, a_f, gap, vl, al_eff, th, vset, mv_prev, m, kbrk, C)
%ACC_MPC_PREP State, references, online bounds and weights for the MPC (codegen).
%   mode: 0 OFF, 1 CRUISE, 2 FOLLOW, 3 STOP, 4 HOLD, 5 GO, 6 EMERGENCY.
%   x = [gap; v_lead - v; v; a1; a2]: a2 (brake path) is the first-order response of the
%   last applied brake-path request, a1 = measured (filtered) acceleration - a2.
%   FOLLOW-like modes track a shaped closing speed (y2 reference)
%       e = gap - (d0 + th*v_lead)          gap error at the lead's speed
%       e > 0: vr_ref = -min(sqrt(2*A*e + c^2) - c, max(v - v_lead, vr_catchup)), c = a/k:
%              the closing speed from which the ego settles at the target gap braking at
%              about A, linear (slope k_gap) near the target; it never asks to close faster
%              than now unless below the catch-up speed. A = a_approach (comfortable), or the
%              profile through the present state when that needs more (A = vr^2/(2*(e - vr/k)),
%              at most a_approach_max): the truck keeps its speed until the needed deceleration
%              vr^2/(2e) reaches a_approach*vr/(vr + 2c), and never brakes harder than the
%              kinematics need (a required-deceleration / TTC-like approach).
%       e < 0: vr_ref = min(k_open*|e|, vr_open_max) (too close: open the gap gently)
%   STOP (lead stopped): target gap d0; below 1 m/s within 3 m of it no throttle (stop there).
%   plus a weak gap term (y1 = gap - th_nom*v -> d0 + (th - th_nom)*v) and the soft minimum
%   gap y1 >= d0_safe + (safe_frac*th - th_nom)*v. CRUISE tracks the set speed. The speed is
%   kept below the set speed by a soft upper bound in every mode.
%   Mass m (kg, from the estimator): the prediction model is in acceleration units (the
%   allocator turns requests into pedal inputs for this mass), so the mass enters through the
%   reachable range of each path: drive path from coasting + retarder 4 (acc_coast,
%   acc_retarder_decel at m) up to 0.85 x full-throttle acceleration scaled to m; the total
%   lower bound no deeper than coasting + retarder 4 + 95 % of the service brake at input 1.0
%   at this speed and brake effectiveness kbrk (estimator's k_brake), so a heavy truck does not
%   plan with braking it cannot deliver.
%   A lower set speed is approached on a ramp (vset_down_rate, at most 80 % of the no-brake
%   deceleration at this speed and mass) starting from the present speed, so a set-speed step
%   does not make the soft upper bound brake hard.
%#codegen
persistent a2 vs_f
if isempty(a2), a2 = 0; vs_f = 0; end
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
ac = acc_coast(v, C, m);
if v > C.alc.ret_min_v
    u1min = ac - acc_retarder_decel(4, v, C, m);
else
    u1min = ac;
end
% full-throttle acceleration measured at C.veh.m; the force minus rolling resistance scales 1/m
afull = interp1(C.veh.afull_v, C.veh.afull_a, min(max(v, C.veh.afull_v(1)), C.veh.afull_v(end)));
arr = C.veh.crr * C.veh.g;
afull = (afull + arr) * C.veh.m / m - arr;
u1max = max(min(C.mpc.a_max_comf, 0.85 * afull), 0.05);
% set speed: ramp down to a lower value without the service brake; the ramp is previewed in
% the y3 reference and the soft upper bound gets 1 m/s of slack while it lasts
vslack = 0;
vset_ref = repmat(vset, p, 1);
if vset < vs_f
    rate = min(C.fol.vset_down_rate, 0.8 * max(-u1min, 0.05));
    vs_f = max(vset, min(vs_f - rate * C.Ts, v + C.fol.vset_margin));
    vset_ref = max(vset, vs_f - rate * C.Ts * (1:p).');
    vslack = 1.0;
else
    vs_f = vset;
end
vset = vs_f;
if mode == 6
    amin = C.mpc.a_min_emerg;
    sw = 2;
else
    amin = C.mpc.a_min_comf;
    sw = 1;
end
% deepest deceleration the service brake can add at this speed and mass (input 1.0)
vb = min(max(v, C.veh.brk_v(1)), C.veh.brk_v(end));
brk_cap = interp1(C.veh.brk_v, C.veh.brk_a(:, end).', vb) * kbrk;
amin = max(amin, u1min - 0.95 * brk_cap);
umin = [u1min; amin - u1min];
umax = [u1max; 0];
ymax = [1e3; 1e3; vset + C.fol.vset_margin + vslack];
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
    cap = min(max(v - vl, C.fol.vr_catchup), C.fol.vr_ref_max);
    % profile deceleration A: the comfortable a_approach, or the profile through the present
    % state when the truck is already inside the comfortable one (late detection, cut-in, lead
    % braking): sqrt(2*A*e + c^2) - c = v - v_lead with c = A/k gives A = vc^2/(2*(e - vc/k)),
    % so it brakes at what the kinematics need, not more. c scales with A so that the slope
    % near the target stays k (a fixed c made the last metres of a stop too stiff).
    A = C.fol.a_approach;
    vc = v - vl;
    if e > 0 && vc > 0
        room = e - vc / k;
        if room > 0
            A = min(max(A, vc^2 / (2 * room)), C.fol.a_approach_max);
        else
            A = C.fol.a_approach_max;
        end
    end
    c = A / k;
    ei = e;
    vli = vl;
    for i = 1:p
        if ei > 0
            prof = sqrt(2 * A * ei + c^2) - c;
            vr_ref(i) = -min(prof, cap);
        else
            vr_ref(i) = min(-C.fol.k_open * ei, C.fol.vr_open_max);   % too close: open gently
        end
        % never ask for a speed above the set speed (lead faster than the set speed): the
        % reference would otherwise fight the soft upper bound on y3 and the MPC hunts there
        vr_ref(i) = max(vr_ref(i), vli - vset);
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
    ref = [repmat(d0 + (th - thn) * v, p, 1), vr_ref, vset_ref];
    ymin = [C.fol.d0_safe + (C.fol.safe_frac * th - thn) * v; -1e3; -1e3];
    ywt = C.mpc.w_y_follow(:);
else
    x = [d0 + thn * v + 200; 0; v; a1; a2];
    ref = [repmat(d0 + (th - thn) * v + 200, p, 1), zeros(p, 1), vset_ref];
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
