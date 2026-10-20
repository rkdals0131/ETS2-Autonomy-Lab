function E = extract_shift_events(D)
%EXTRACT_SHIFT_EVENTS Gear changes of the game's automatic gearbox in a recording.
%   E = extract_shift_events(acc_load_recording("acc-20261020-192243")) returns a
%   table with one row per change of the SDK engine gear between two driving gears
%   (launches from neutral and stops to neutral are listed with from/to = 0).
%   Columns: t_start (clutch starts to open, s), t_change (SDK gear changes),
%   t_closed (clutch back below 0.05), t_thr (applied throttle back to 90 % of the
%   command, NaN if the command is below 0.05), from, to, kmh, rpm_from (engine
%   speed the old gear gives at the speed when the clutch starts to open),
%   rpm_to (the same in the new gear at t_change), thr (command), brk (applied),
%   ret (retarder step), phase.
R = [20.81 16.16 12.57 9.76 7.56 5.87 4.55 3.53 2.77 2.15 1.66 1.29 1.0 0.78];
k2w = 2.59 / 0.506 * 60 / (2 * pi);           % rpm per (m/s) per unit ratio
g = D.gear; n = numel(g);
ch = find(diff(g) ~= 0) + 1;
rows = cell(0, 16);
for k = ch.'
    from = g(k - 1); to = g(k);
    % back to the frame where the clutch started to open (at most 2 s)
    j = k - 1;
    while j > 1 && D.clutch(j) > 0.05 && D.t(k) - D.t(j) < 2.0, j = j - 1; end
    j = j + 1;
    % forward to the clutch closed again
    m = k;
    while m < n && D.clutch(m) > 0.05 && D.t(m) - D.t(k) < 5.0, m = m + 1; end
    tthr = NaN;
    if D.cmd_thr(j) > 0.05
        q = m;
        while q < n && D.eff_thr(q) < 0.9 * D.cmd_thr(q) && D.t(q) - D.t(k) < 5.0, q = q + 1; end
        tthr = D.t(q);
    end
    rf = NaN; rt = NaN;
    if from > 0, rf = D.speed(j) * k2w * R(from); end
    if to > 0, rt = D.speed(k) * k2w * R(to); end
    rows(end + 1, :) = {D.t(j), D.t(k), D.t(m), tthr, from, to, D.speed(j) * 3.6, rf, rt, ...
        D.cmd_thr(j), D.eff_brk(j), D.ret_step(j), D.phase(k), D.rpm(j), D.eb(j), D.eff_thr(k - 1)}; %#ok<AGROW>
end
E = cell2table(rows, "VariableNames", ["t_start", "t_change", "t_closed", "t_thr", "from", "to", ...
    "kmh", "rpm_from", "rpm_to", "thr", "brk", "ret", "phase", "rpm_sdk", "eb", "thr_blip"]);
end
