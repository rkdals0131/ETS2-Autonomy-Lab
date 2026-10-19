function result = fit_steering_plant(framesFile, outFile)
%FIT_STEERING_PLANT Estimate eff/u = a/(s + c) * exp(-s*Td) per speed from a sysid run.
%   result = fit_steering_plant(framesFile, outFile) reads frames.jsonl written by
%   apps/sysid/sysid_drive.py, groups the 20/40/60/80 km/h blocks (rate pulses,
%   holds, sines, recoveries), resamples each block to 60 Hz and fits a one-pole
%   transfer function with input delay (System Identification Toolbox tfest).
%   The results go to outFile as JSON for comparison with docs/29.
arguments
    framesFile (1,1) string
    outFile (1,1) string = ""
end
lines = splitlines(fileread(framesFile));
lines = lines(strlength(lines) > 0);
n = numel(lines);
t = nan(n,1); u = nan(n,1); eff = nan(n,1); v = nan(n,1); phase = strings(n,1);
for k = 1:n
    r = jsondecode(lines{k});
    if isempty(r.eff_steer) || isempty(r.speed), continue; end
    t(k) = double(r.sim_us) / 1e6; u(k) = r.cmd_steer; eff(k) = r.eff_steer; v(k) = r.speed;
    phase(k) = string(r.phase);
end
ok = ~isnan(t); t = t(ok); u = u(ok); eff = eff(ok); v = v(ok); phase = phase(ok);
speeds = [20 40 60 80];
Ts = 1/60;
result = struct('kmh', {}, 'a', {}, 'c', {}, 'delay_s', {}, 'fit_percent', {}, 'samples', {});
for kmh = speeds
    tag = ":" + kmh;
    in = (startsWith(phase, "rate_pulse" + tag) | startsWith(phase, "rate_hold" + tag) | ...
          startsWith(phase, "sine" + tag) | startsWith(phase, "recover" + tag));
    idx = find(in);
    if numel(idx) < 200, continue; end
    % Contiguous runs of this speed's blocks become separate experiments.
    breaks = [0; find(diff(idx) > 1); numel(idx)];
    exps = {};
    for b = 1:numel(breaks)-1
        seg = idx(breaks(b)+1:breaks(b+1));
        if numel(seg) < 60, continue; end
        tt = t(seg) - t(seg(1));
        [tt, keep] = unique(tt);
        grid = (0:Ts:tt(end)).';
        uu = interp1(tt, u(seg(keep)), grid, 'previous');
        yy = interp1(tt, eff(seg(keep)), grid, 'linear');
        exps{end+1} = iddata(yy, uu, Ts); %#ok<AGROW>
    end
    data = merge(exps{:});
    opt = tfestOptions('InitialCondition', 'estimate', 'EnforceStability', true);
    sys = tfest(data, 1, 0, NaN, opt);   % one pole, no zero, delay estimated
    a = sys.Numerator(end); c = sys.Denominator(end);
    fitp = mean(sys.Report.Fit.FitPercent);
    result(end+1) = struct('kmh', kmh, 'a', a, 'c', c, 'delay_s', sys.IODelay, ...
                           'fit_percent', fitp, 'samples', sum(cellfun(@(e) size(e,1), exps))); %#ok<AGROW>
    fprintf('%3d km/h: a = %.3f /s, c = %.3f /s, delay = %.3f s, fit = %.1f %%\n', kmh, a, c, sys.IODelay, fitp);
end
if strlength(outFile) > 0
    fid = fopen(outFile, 'w'); fprintf(fid, '%s', jsonencode(result, 'PrettyPrint', true)); fclose(fid);
end
end
