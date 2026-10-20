function D = lcc_load_recording(runName, opts)
%LCC_LOAD_RECORDING Read the lateral signals of bridge/recordings/<runName> for the LCC work.
%   D = lcc_load_recording("sysid-20261020-153130") returns one row per game frame
%   (paused frames and repeated sim times dropped), cached in cache/<runName>.mat.
%
%   Frame fields (frames.jsonl): t (s from the first kept frame), sim_us, phase, speed (m/s),
%   eff (SDK truck.effective.steering), cmd (our relative steering command u), w0, w1
%   (front wheel steering, turns), vang (SDK local angular velocity, y component, raw),
%   psi (truck heading, rad, unwrapped; SCS heading is in turns), X, Z (SCS position, m),
%   tag_up (lift axle raised), trailer (connected), psi_t (trailer heading, rad, unwrapped),
%   Xt, Zt (trailer position).
%   For lanetest-* runs, D.lat holds lateral.jsonl (one row per controller call): t (s on the
%   frame clock), scenario, speed, applied, cmd, target, rate, wheel, offset, heading_error,
%   distance, remaining, path_curvature, curvature.
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
src = fullfile(repo, "bridge", "recordings", runName);
R = read_jsonl(fullfile(src, "frames.jsonl"));
n = numel(R);
D.sim_us = nan(n, 1); D.phase = strings(n, 1); paused = false(n, 1);
f = ["speed", "eff", "cmd", "w0", "w1", "vang", "head", "X", "Z", "tag_up", "trailer", "head_t", "Xt", "Zt"];
for k = f, D.(k) = nan(n, 1); end
for k = 1:n
    r = R{k};
    D.sim_us(k) = double(r.sim_us);
    D.phase(k) = string(r.phase);
    if isfield(r, "paused") && ~isempty(r.paused), paused(k) = logical(r.paused); end
    D.speed(k) = num(r, "speed"); D.eff(k) = num(r, "eff_steer"); D.cmd(k) = num(r, "cmd_steer");
    D.w0(k) = num(r, "w0"); D.w1(k) = num(r, "w1");
    if isfield(r, "v_ang") && numel(r.v_ang) == 3, D.vang(k) = r.v_ang(2); end
    if isfield(r, "rot") && numel(r.rot) == 3, D.head(k) = r.rot(1); end
    if isfield(r, "pos") && numel(r.pos) == 3, D.X(k) = r.pos(1); D.Z(k) = r.pos(3); end
    if isfield(r, "x")
        x = r.x;
        if isfield(x, "truck_lift_axle"), D.tag_up(k) = double(x.truck_lift_axle); end
        if isfield(x, "trailer_0_connected"), D.trailer(k) = double(x.trailer_0_connected); end
        if isfield(x, "trailer_0_world_placement") && isstruct(x.trailer_0_world_placement)
            p = x.trailer_0_world_placement;
            D.head_t(k) = p.euler_rotations(1);
            D.Xt(k) = p.position_m(1); D.Zt(k) = p.position_m(3);
        end
    end
end
keep = ~paused & ~isnan(D.sim_us);
D = rows(D, keep);
keep = [true; diff(D.sim_us) > 0];
D = rows(D, keep);
D.t = (D.sim_us - D.sim_us(1)) / 1e6;
D.psi = unwrap(D.head * 2 * pi);          % SCS heading: turns, counterclockwise
ok = ~isnan(D.head_t);
D.psi_t = nan(size(D.t));
if any(ok), D.psi_t(ok) = unwrap(D.head_t(ok) * 2 * pi); end
D = rmfield(D, ["head", "head_t"]);
D.run = runName;
latFile = fullfile(src, "lateral.jsonl");
if isfile(latFile)
    L = read_jsonl(latFile);
    m = numel(L);
    g = ["scenario", "speed", "applied", "cmd", "target", "rate", "wheel", "offset", "heading_error", ...
         "distance", "remaining", "path_curvature", "curvature"];
    lat.t = nan(m, 1);
    for k = g, lat.(k) = nan(m, 1); end
    for k = 1:m
        r = L{k};
        lat.t(k) = (double(r.t) - D.sim_us(1)) / 1e6;
        for j = g, lat.(j)(k) = num(r, j); end
    end
    D.lat = lat;
end
save(cacheFile, "D");
end

% =====================================================================
function R = read_jsonl(file)
txt = fileread(file);
lines = splitlines(string(txt));
lines = lines(strlength(lines) > 0);
R = jsondecode("[" + strjoin(lines, ",") + "]");
if isstruct(R), R = num2cell(R); end
end

function v = num(r, f)
v = NaN;
if isfield(r, f) && ~isempty(r.(f)) && isnumeric(r.(f)), v = double(r.(f)); end
end

function D = rows(D, keep)
fn = fieldnames(D);
for i = 1:numel(fn)
    x = D.(fn{i});
    if size(x, 1) == numel(keep), D.(fn{i}) = x(keep, :); end
end
end
