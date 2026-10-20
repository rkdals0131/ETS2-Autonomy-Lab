function [scn_lead, scn_disc] = acc_scenario_signals(s)
%ACC_SCENARIO_SIGNALS Lead trajectory and driver signals of one scenario (acc_scenarios.m).
%   scn_lead = [t x_lead v_lead a_lead] on a 10 ms grid, scn_disc = [t lead_valid enable
%   set_speed time_gap resume] on a 50 ms grid (the closed loop holds these values).
dt = 0.01;
t = (0:dt:s.T).';
n = numel(t);
x = zeros(n, 1); v = zeros(n, 1); a = zeros(n, 1);
x(1) = s.lead.x0; v(1) = s.lead.v0;
for k = 1:n
    ak = 0; vt = v(k);
    for j = 1:size(s.lead.seg, 1)          % the latest segment that has started is active
        if t(k) >= s.lead.seg(j, 1)
            ak = s.lead.seg(j, 2);
            vt = s.lead.seg(j, 3);
        end
    end
    if (ak < 0 && v(k) <= vt) || (ak > 0 && v(k) >= vt), ak = 0; end
    a(k) = ak;
    if k < n
        vn = max(0, v(k) + ak*dt);
        if ak < 0, vn = max(vn, vt); elseif ak > 0, vn = min(vn, vt); end
        v(k+1) = vn;
        x(k+1) = x(k) + (v(k) + v(k+1))/2*dt;
    end
end
% shift: place the lead `gap` m ahead of where the ego would be at constant start speed
for j = 1:size(s.lead.shift, 1)
    k = find(t >= s.lead.shift(j, 1), 1);
    x = x + (s.v0*t(k) + s.lead.shift(j, 2) - x(k));
end
scn_lead = [t x v a];

td = (0:0.05:s.T).';
held = @(tab) tab(max(1, sum(td >= tab(:, 1).', 2)), 2);
valid = held(s.lead.valid);
vset = held(s.vset);
res = zeros(size(td));
for j = 1:size(s.resume, 1)
    res(td >= s.resume(j, 1) & td < s.resume(j, 2)) = 1;
end
scn_disc = [td valid ones(size(td)) vset s.time_gap*ones(size(td)) res];
end
