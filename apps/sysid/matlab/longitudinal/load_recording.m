function D = load_recording(runName, opts)
%LOAD_RECORDING Parse bridge/recordings/<runName>/frames.jsonl into column vectors.
%   D = load_recording("gear-20261020-151650") returns a struct with one row per
%   frame that carries the Tier 1 drivetrain log ("dt"). Frames without "dt" and
%   paused frames are dropped. The result is cached in cache/<runName>.mat; pass
%   opts.Refresh = true to parse again.
%
%   Fields (SI units): t (s, from sim_us, starts at 0), phase (string), attempt,
%   focus, speed (m/s), gear (SDK gear), rpm, cmd_thr, cmd_brk, eff_thr, eff_brk,
%   y (world height, m), we (engine rad/s), Te (engine torque Nm), To (output
%   torque Nm), wo (output shaft rad/s), ratio, clutch (0 engaged .. 1 open),
%   ret_level (0..1), eb (0/1), wbt (n x 6 wheel brake torque Nm), on_ground4/5,
%   ret_step (SDK truck.brake.retarder, NaN if not logged), motor_flag,
%   brake_temp (deg C), trailer (0/1), file (source path).
arguments
    runName (1,1) string
    opts.Refresh (1,1) logical = false
end
here = fileparts(mfilename("fullpath"));
repo = fullfile(here, "..", "..", "..", "..");
cacheFile = fullfile(here, "cache", runName + ".mat");
if ~opts.Refresh && isfile(cacheFile)
    S = load(cacheFile, "D");
    D = S.D;
    return
end
src = fullfile(repo, "bridge", "recordings", runName, "frames.jsonl");
txt = fileread(src);
lines = splitlines(string(txt));
lines = lines(strlength(lines) > 0);
n = numel(lines);

D.t = nan(n,1); D.phase = strings(n,1); D.attempt = zeros(n,1); D.focus = true(n,1);
D.paused = false(n,1);
names = ["speed","gear","rpm","cmd_thr","cmd_brk","eff_thr","eff_brk"];
for f = names, D.(f) = nan(n,1); end
D.y = nan(n,1);
dtn = ["we","Te","To","wo","ratio","clutch","ret_level","eb"];
dtk = ["engine_speed_radps","engine_torque_nm","output_torque_nm", ...
       "output_shaft_speed_radps","gear_ratio","clutch","retarder_level","engine_brake"];
for f = dtn, D.(f) = nan(n,1); end
D.wbt = nan(n,6);
D.on_ground4 = nan(n,1); D.on_ground5 = nan(n,1);
D.ret_step = nan(n,1); D.motor_flag = nan(n,1); D.brake_temp = nan(n,1); D.trailer = zeros(n,1);
hasDt = false(n,1);

for k = 1:n
    r = jsondecode(lines(k));
    if ~isfield(r, "dt") || isempty(r.dt), continue; end
    hasDt(k) = true;
    D.t(k) = double(r.sim_us) / 1e6;
    D.phase(k) = string(r.phase);
    if isfield(r, "attempt"), D.attempt(k) = r.attempt; end
    if isfield(r, "focus"), D.focus(k) = logical(r.focus); end
    if isfield(r, "paused"), D.paused(k) = logical(r.paused); end
    for f = names
        if isfield(r, f) && ~isempty(r.(f)), D.(f)(k) = r.(f); end
    end
    if isfield(r, "pos") && numel(r.pos) >= 2, D.y(k) = r.pos(2); end
    for j = 1:numel(dtn)
        if isfield(r.dt, dtk(j)) && ~isempty(r.dt.(dtk(j))), D.(dtn(j))(k) = r.dt.(dtk(j)); end
    end
    if isfield(r.dt, "wheel_brake_torque_nm")
        w = r.dt.wheel_brake_torque_nm(:).';
        D.wbt(k, 1:min(6, numel(w))) = w(1:min(6, numel(w)));
    end
    if isfield(r, "x")
        x = r.x;
        D.on_ground4(k) = getx(x, "truck_wheel_on_ground_4_");
        D.on_ground5(k) = getx(x, "truck_wheel_on_ground_5_");
        D.ret_step(k) = getx(x, "truck_brake_retarder");
        D.motor_flag(k) = getx(x, "truck_brake_motor");
        D.brake_temp(k) = getx(x, "truck_brake_temperature");
        tc = getx(x, "trailer_0_connected");
        D.trailer(k) = ~isnan(tc) && tc;
    end
end

% Keep frames with the drivetrain log, drop paused frames and repeated sim times.
keep = hasDt & ~D.paused;
fn = fieldnames(D);
for i = 1:numel(fn)
    D.(fn{i}) = D.(fn{i})(keep, :);
end
[~, iu] = unique(D.t, "stable");
dup = true(numel(D.t), 1); dup(iu) = false;
dup = dup | [false; diff(D.t) <= 0];
for i = 1:numel(fn)
    D.(fn{i}) = D.(fn{i})(~dup, :);
end
D.t0 = D.t(1);
D.t = D.t - D.t(1);
D.run = runName;
D.file = src;
% A lost on_ground channel means the tag axle state is unknown; assume lifted.
D.on_ground4(isnan(D.on_ground4)) = 0;
D.on_ground5(isnan(D.on_ground5)) = 0;
save(cacheFile, "D");
end

function v = getx(x, name)
if isfield(x, name) && ~isempty(x.(name))
    val = x.(name);
    if islogical(val) || isnumeric(val)
        v = double(val(1));
    else
        v = NaN;
    end
else
    v = NaN;
end
end
