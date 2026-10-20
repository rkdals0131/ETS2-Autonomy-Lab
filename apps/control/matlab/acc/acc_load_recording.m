function D = acc_load_recording(runName, opts)
%ACC_LOAD_RECORDING Read bridge/recordings/<runName>/frames.jsonl for the ACC work.
%   D = acc_load_recording("acc-20261020-192243") returns one row per frame
%   (paused frames and repeated sim times dropped), cached in cache/<runName>.mat.
%   It reads more than apps/sysid/matlab/longitudinal/load_recording.m (gear_shown,
%   the automatic clutch, the trailer wheel brake torques) and keeps frames
%   without the drivetrain log (dt fields NaN there).
%
%   Fields: t (s from the first kept frame), phase, speed (m/s), gear (SDK engine
%   gear), gear_shown, rpm, cmd_thr, cmd_brk, eff_thr, eff_brk, ret_step (SDK
%   truck.brake.retarder), motor_flag, clutch_eff (truck.effective.clutch),
%   brake_temp, we, Te, To, wo, ratio, clutch, ret_level, eb, wbt (n x 6),
%   twbt (n x 6 trailer).
arguments
    runName (1,1) string
    opts.Refresh (1,1) logical = false
end
here = fileparts(mfilename("fullpath"));
repo = fullfile(here, "..", "..", "..", "..");
if ~isfolder(fullfile(here, "cache")), mkdir(fullfile(here, "cache")); end
cacheFile = fullfile(here, "cache", runName + ".mat");
if ~opts.Refresh && isfile(cacheFile)
    S = load(cacheFile, "D");
    D = S.D;
    return
end
src = fullfile(repo, "bridge", "recordings", runName, "frames.jsonl");
lines = splitlines(string(fileread(src)));
lines = lines(strlength(lines) > 0);
n = numel(lines);

top = ["speed", "gear", "gear_shown", "rpm", "cmd_thr", "cmd_brk", "eff_thr", "eff_brk"];
xk = ["truck_brake_retarder", "truck_brake_motor", "truck_effective_clutch", "truck_brake_temperature"];
xn = ["ret_step", "motor_flag", "clutch_eff", "brake_temp"];
dtk = ["engine_speed_radps", "engine_torque_nm", "output_torque_nm", ...
       "output_shaft_speed_radps", "gear_ratio", "clutch", "retarder_level", "engine_brake"];
dtn = ["we", "Te", "To", "wo", "ratio", "clutch", "ret_level", "eb"];
D.t = nan(n, 1); D.phase = strings(n, 1); D.paused = false(n, 1);
for f = [top xn dtn], D.(f) = nan(n, 1); end
D.wbt = nan(n, 6); D.twbt = nan(n, 6);
for k = 1:n
    r = jsondecode(lines(k));
    D.t(k) = double(r.sim_us) / 1e6;
    D.phase(k) = string(r.phase);
    if isfield(r, "paused"), D.paused(k) = logical(r.paused); end
    for f = top
        if isfield(r, f) && ~isempty(r.(f)), D.(f)(k) = double(r.(f)); end
    end
    if isfield(r, "x")
        for j = 1:numel(xk)
            if isfield(r.x, xk(j)) && ~isempty(r.x.(xk(j))), D.(xn(j))(k) = double(r.x.(xk(j))); end
        end
    end
    if isfield(r, "dt") && ~isempty(r.dt)
        for j = 1:numel(dtk)
            if isfield(r.dt, dtk(j)) && ~isempty(r.dt.(dtk(j))), D.(dtn(j))(k) = r.dt.(dtk(j)); end
        end
        if isfield(r.dt, "wheel_brake_torque_nm")
            w = r.dt.wheel_brake_torque_nm(:).'; D.wbt(k, 1:min(6, numel(w))) = w(1:min(6, numel(w)));
        end
        if isfield(r.dt, "trailer_wheel_brake_torque_nm")
            w = r.dt.trailer_wheel_brake_torque_nm(:).'; D.twbt(k, 1:min(6, numel(w))) = w(1:min(6, numel(w)));
        end
    end
end
keep = ~D.paused & ~isnan(D.t);
keep = keep & [true; diff(D.t) > 0];
fn = fieldnames(D);
for i = 1:numel(fn), D.(fn{i}) = D.(fn{i})(keep, :); end
keep = [true; diff(D.t) > 0];      % again after removing paused frames
for i = 1:numel(fn), D.(fn{i}) = D.(fn{i})(keep, :); end
D.t0 = D.t(1);
D.t = D.t - D.t(1);
D.run = runName;
save(cacheFile, "D");
end
