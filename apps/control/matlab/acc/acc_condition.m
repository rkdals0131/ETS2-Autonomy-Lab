function [th, vset, v, a_f, lead_ok, gap, vl, al_eff, ttc, dreq] = acc_condition( ...
    set_speed, time_gap, ego_speed, ego_accel, lead_valid, lead_gap, lead_speed, lead_accel, C)
%ACC_CONDITION Input conditioning of ACC_Controller (codegen, called from a MATLAB Function block).
%   Clamps the settings, filters the ego acceleration, limits the lead acceleration used as
%   the MPC disturbance so that the predicted lead speed does not go below zero within
%   C.fol.a_lead_floor_T, and computes the time to collision and the kinematic deceleration
%   the ego truck needs to stay behind the lead.
%#codegen
persistent af
if isempty(af), af = 0; end
th = time_gap;
if ~(th > 0) || ~isfinite(th), th = C.fol.th_default; end
th = min(max(th, C.fol.th_min), C.fol.th_max);
vset = max(0, set_speed);
if ~isfinite(vset), vset = 0; end
v = max(0, ego_speed);
if ~isfinite(v), v = 0; end
a = ego_accel;
if ~isfinite(a), a = af; end
alpha = C.Ts / (C.fol.a_filter_tau + C.Ts);
af = af + alpha * (a - af);
a_f = af;

lead_ok = lead_valid && isfinite(lead_gap) && isfinite(lead_speed) && lead_gap > 0;
if lead_ok
    gap = lead_gap;
    vl = max(0, lead_speed);
    al = lead_accel;
    if ~isfinite(al), al = 0; end
    al_eff = max(al, -vl / C.fol.a_lead_floor_T);
    vrel = v - vl;                                   % closing speed
    % TTC for the mode logic only while closing fast: a planned constant-deceleration stop
    % behind a stopped lead has TTC = v/(2a), which goes to zero by design
    if vrel > C.fol.ttc_min_closing
        ttc = gap / vrel;
    else
        ttc = 99;
    end
    % deceleration the ego needs: lead keeps its speed -> relative stopping distance;
    % lead braking -> both brake to standstill (lead at its present deceleration)
    room = max(gap - C.fol.d0_safe, 0.5);
    dreq = max(vrel, 0)^2 / (2 * room);
    if al_eff < -0.1
        sl = vl^2 / (2 * -al_eff);
        dreq = max(dreq, v^2 / (2 * max(gap + sl - C.fol.d0_safe, 0.5)));
    end
else
    gap = 200; vl = v; al_eff = 0; ttc = 99; dreq = 0;
end
end
