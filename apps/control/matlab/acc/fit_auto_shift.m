function R = fit_auto_shift(opts)
%FIT_AUTO_SHIFT Shift events of the game's automatic gearbox and the launch clutch fit.
%   R = fit_auto_shift() reads acc-20261020-192243, prints every gear change with the
%   engine speed the old and new gear give (the numbers behind P.at in
%   acc_plant_params.m), then fits the launch clutch capacity ramp (cap_max, cap_t per
%   throttle) by replaying the launch segments on acc_plant.slx (fminsearch on the speed
%   RMS of the first seconds). Results go to results/auto_shift_fit.json; copy the fitted
%   numbers into acc_plant_params.m by hand (they are not read automatically).
arguments
    opts.Fit (1,1) logical = true
end
here = fileparts(mfilename("fullpath"));
D = acc_load_recording("acc-20261020-192243");
E = extract_shift_events(D);
E.open_s = E.t_change - E.t_start;
E.close_s = E.t_closed - E.t_change;
E.thr_back_s = E.t_thr - E.t_start;
disp(E(:, ["t_start", "from", "to", "kmh", "rpm_from", "rpm_to", "thr", "brk", "ret", ...
    "open_s", "close_s", "thr_back_s", "phase"]));
up = E(E.from > 0 & E.to > E.from, :);
dn = E(E.from > 0 & E.to > 0 & E.to < E.from, :);
fprintf("upshifts %d: open %.3f-%.3f s, close %.3f-%.3f s, throttle back %.2f-%.2f s\n", height(up), ...
    min(up.open_s), max(up.open_s), min(up.close_s), max(up.close_s), min(up.thr_back_s), max(up.thr_back_s));
fprintf("downshifts %d: open %.3f-%.3f s, close %.3f-%.3f s\n", height(dn), ...
    min(dn.open_s), max(dn.open_s), min(dn.close_s), max(dn.close_s));
R.events = E;

% launch clutch fit: one launch per throttle level (gear 4 starts where possible)
L = [0.2 44.3 52.0; 0.4 89.0 94.5; 0.7 131.3 135.5; 1.0 164.3 168.0];
P0 = acc_plant_params();
R.launch = struct([]);
if opts.Fit
    load_system("acc_plant");
    for i = 1:size(L, 1)
        thr = L(i, 1);
        j = find(abs(P0.at.launch_thr - thr) < 1e-9);
        x0 = [P0.at.launch_cap_max(j), P0.at.launch_cap_t(j)];
        cost = @(x) launch_cost(D, L(i, 2), L(i, 3), P0, j, x);
        o = optimset("TolX", 1, "TolFun", 1e-3, "MaxFunEvals", 40, "Display", "off");
        [x, f] = fminsearch(cost, x0, o);
        fprintf("throttle %.1f: cap_max %.0f Nm, cap_t %.2f s, speed rms %.3f m/s (start %.3f)\n", ...
            thr, x(1), x(2), f, cost(x0));
        R.launch(i).thr = thr; R.launch(i).cap_max = x(1); R.launch(i).cap_t = x(2); R.launch(i).rms = f;
    end
end
J.events = table2struct(E);
J.launch = R.launch;
fid = fopen(fullfile(here, "results", "auto_shift_fit.json"), "w");
fwrite(fid, jsonencode(J, PrettyPrint=true)); fclose(fid);
end

function c = launch_cost(D, t0, t1, P, j, x)
P.at.launch_cap_max(j) = max(10, x(1));
P.at.launch_cap_t(j) = max(0.05, x(2));
S = replay_segment(D, t0, t1, P=P);
c = S.rms;
end
