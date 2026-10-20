function R = lcc_collect(out)
%LCC_COLLECT Signals of an lcc_closed_loop run, resampled at the 20 ms controller ticks.
%   R.t, ey, epsi, delta, hitch, preview0 (curvature_preview(1)), s, X, Y, psi, psi_t, r, ay,
%   eff, u, kappa, wheel_angle_cmd, wheel_rate_cmd, status, ctrl_in (n x 69).
names = ["ey", "epsi", "delta", "hitch", "preview", "kappa_back", "s", "X", "Y", "psi", "psi_t", ...
         "r", "ay", "eff", "u", "kappa", "wheel_angle_cmd", "wheel_rate_cmd", "status", "ctrl_in"];
y = out.yout;
T = y{1}.Values.Time(end);
R.t = (0:0.02:T).';
for i = 1:numel(names)
    ts = y{i}.Values;
    d = squeeze(ts.Data);
    tt = ts.Time;
    if size(d, 1) ~= numel(tt), d = d.'; end
    % value at each tick (last sample at or before the tick)
    idx = discretize(R.t + 1e-9, [tt; inf]);
    idx(isnan(idx)) = 1;
    v = double(d(idx, :));
    if names(i) == "preview"
        R.preview0 = v(:, 1);
    else
        R.(names(i)) = v;
    end
end
end
