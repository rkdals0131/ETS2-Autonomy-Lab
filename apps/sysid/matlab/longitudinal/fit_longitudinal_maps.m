function T = fit_longitudinal_maps(opts)
%FIT_LONGITUDINAL_MAPS Fit the data-driven tables of the longitudinal plant.
%   T = fit_longitudinal_maps() reads the training recordings (fixed-gear map run
%   and brake step run, both with the empty trailer), fits
%     - engine friction line (throttle 0, no engine brake),
%     - engine brake torque vs rpm,
%     - part-load throttle map f(throttle, rpm) as a fraction between friction
%       and the def full-load curve,
%     - engine torque rise-rate limit vs (torque, rpm),
%     - gearbox efficiency and the inertia inside the reported output torque
%       (J_report), free engine inertia (clutch open),
%     - locked engine-side inertia J_locked together with mass from the speed
%       (engine-torque force balance on the fixed-gear run),
%     - retarder torque slope per level (with the inertia term),
%     - truck service brake gain vs (brake temperature, wheel speed),
%     - trailer brake force vs brake input (force balance residual),
%     - vehicle mass and rolling coefficient per run (with J_locked fixed),
%   and writes them to longitudinal_fitted_tables.m (generated, tracked) and
%   results/fitted_maps.json. The validation runs (sysid-*) are not used here
%   except for the per-run mass/rolling fit, which every run needs.
arguments
    opts.TrainRuns (1,:) string = ["gear-20261020-151650", "brake-20261020-152718"]
    opts.AllRuns (1,:) string = ["gear-20261020-151650", "brake-20261020-152718", ...
        "sysid-20261020-153130", "sysid-20261020-144933"]
    opts.Write (1,1) logical = true
end
here = fileparts(mfilename("fullpath"));
fd = 2.59; r = 0.506; g = 9.81; cAir = 3.0;
rpmC = [300 440 1000 1300 1600 1900 2300 2500];
frC = [.1 .4 1 1 .82 .72 .4 .1];
tFull = @(rpm) 2700 * interp1(rpmC, frC, min(max(rpm, 300), 2500), "linear");

% ---- collect training samples -------------------------------------------
S = collect(opts.TrainRuns);

% ---- engine friction (throttle 0, engine brake off, clutch locked) ----------
q = S.locked & S.thr0 & S.eb == 0 & S.ret == 0 & S.brk == 0 & S.settled & abs(S.dTe) < 50 & S.rpm > 850;
pf = polyfit(S.rpm(q), S.Te(q), 1);
T.eng.fric0 = pf(2); T.eng.fric1 = pf(1);
T.eng.fric_rms = rms(S.Te(q) - polyval(pf, S.rpm(q)));
T.eng.fric_n = nnz(q);
tFric = @(rpm) T.eng.fric0 + T.eng.fric1 * rpm;

% ---- engine brake vs rpm ----------------------------------------------------
q = S.locked & S.thr0 & S.eb > 0 & abs(S.dTe) < 300;
bp = [600 900 950 1000 1050 1100 1150 1200 1300 1450 1600 1750 1900 2050 2200 2350 2600];
[T.eng.eb_rpm, T.eng.eb_nm, T.eng.eb_n] = binned(S.rpm(q), S.Te(q), bp, 5);
% hold the end values outside the measured range
T.eng.eb_rpm = [500, T.eng.eb_rpm, 2700];
T.eng.eb_nm = [T.eng.eb_nm(1) * 0.8, T.eng.eb_nm, T.eng.eb_nm(end)];

% ---- part-load throttle map ---------------------------------------------------
q = S.locked & S.eb == 0 & S.ret == 0 & S.brk == 0 & S.rpm > 800 & abs(S.dTe) < 300;
thr = S.thr(q); rpm = S.rpm(q);
frac = (S.Te(q) - tFric(rpm)) ./ (tFull(rpm) - tFric(rpm));
% one-parameter shape f = 1 - (1 - thr)^n as the fill-in for empty cells
nfit = fminbnd(@(n) sum((frac - (1 - (1 - thr).^n)).^2), 0.5, 3);
T.eng.thr_shape_n = nfit;
T.eng.thr_shape_rms = rms(frac - (1 - (1 - thr).^nfit));
T.eng.thr_bp = [0 0.05 0.1 0.15 0.2 0.25 0.3 0.4 0.5 0.6 0.7 0.8 0.9 1.0];
T.eng.rpm_bp = [600 900 1200 1500 1800 2100 2400];
F = 1 - (1 - T.eng.thr_bp(:)).^nfit * ones(1, numel(T.eng.rpm_bp));
N = zeros(size(F));
for i = 2:numel(T.eng.thr_bp) - 1
    for j = 1:numel(T.eng.rpm_bp)
        w = (abs(thr - T.eng.thr_bp(i)) < 0.025) & (abs(rpm - T.eng.rpm_bp(j)) < 150);
        if nnz(w) >= 30
            % move the shape by the median residual near this cell
            F(i, j) = F(i, j) + median(frac(w) - (1 - (1 - thr(w)).^nfit));
            N(i, j) = nnz(w);
        end
    end
end
F(1, :) = 0; F(end, :) = 1;
F = min(max(cummax(F, 1), 0), 1);
T.eng.frac_table = F;
T.eng.frac_n = N;

% ---- torque rise-rate limit (clutch locked) --------------------------------------
tdes = tFric(S.rpm) + interp2(T.eng.rpm_bp, T.eng.thr_bp, F, ...
    min(max(S.rpm, 600), 2400), S.thr, "linear") .* (tFull(S.rpm) - tFric(S.rpm));
gap = tdes - S.Te;
q = S.locked & S.eb == 0 & S.ret == 0 & S.rpm > 750 & gap > 150 & S.fwd_ok;
T.eng.rate_te_bp = [0 400 800 1200 1600 2000 2400 2800];
T.eng.rate_rpm_bp = [800 950 1100 1250 1400 1600 2000];
R = nan(numel(T.eng.rate_te_bp), numel(T.eng.rate_rpm_bp));
for i = 1:numel(T.eng.rate_te_bp)
    for j = 1:numel(T.eng.rate_rpm_bp)
        w = q & abs(S.Te - T.eng.rate_te_bp(i)) < 200 & abs(S.rpm - T.eng.rate_rpm_bp(j)) < 75;
        if nnz(w) >= 8, R(i, j) = median(S.dTe_fwd(w)); end
    end
end
T.eng.rate_cells_measured = nnz(~isnan(R));
% fill the empty cells with the nearest measured cell (first along rpm, then torque)
R = fillmissing(R, "nearest", 2);
R = fillmissing(R, "nearest", 1);
T.eng.rate_up_table = max(R, 500);
T.eng.rate_down = 30000;   % torque drops within one 60 Hz frame (Nm/s)

% ---- gearbox: efficiency and the inertia in the reported output torque -------------
% The Tier 1 output torque follows To = eta*ratio*(Te - J_report*d(w_e)/dt) very closely
% (gears 9-14). J_report is only the inertia inside that channel: the vehicle itself
% accelerates as if the locked engine-side inertia were J_locked (fitted below from
% the speed), so the model keeps the two apart.
q = S.locked & S.ratio < 2.8 & S.ret == 0 & S.speed > 1 & ~isnan(S.dwo) & S.settled;
X = [S.ratio(q) .* S.Te(q), -S.ratio(q).^2 .* S.dwo(q)];
b = X \ S.To(q);
T.gb.eta = b(1); T.gb.J_report = b(2) / b(1);
T.gb.fit_rms = rms(S.To(q) - X * b); T.gb.fit_n = nnz(q);
T.gb.fit_r2 = 1 - sum((S.To(q) - X * b).^2) / sum((S.To(q) - mean(S.To(q))).^2);
q = S.clutch >= 0.99 & ~isnan(S.dwe) & S.rpm > 650 & abs(S.Te) > 100;
T.eng.J_free = S.dwe(q) \ S.Te(q);
T.eng.J_free_n = nnz(q);

% ---- retarder slope per level (output shaft Nm per rad/s) ------------------------
Tret = S.To - T.gb.eta * S.ratio .* (S.Te - T.gb.J_report * S.ratio .* S.dwo);
levels = [0.2 0.4 0.6];
T.ret.slope_measured = nan(size(levels)); T.ret.slope_n = zeros(size(levels));
for i = 1:numel(levels)
    q = abs(S.ret - levels(i)) < 0.01 & S.locked & S.eb == 0 & S.thr0 & S.wo > 20 & ~isnan(S.dwo);
    T.ret.slope_measured(i) = S.wo(q) \ Tret(q);
    T.ret.slope_n(i) = nnz(q);
end
% Levels 0.8 and 1.0 are not in the training data. docs/30 "브레이크와 리타더 다시
% 보기" gives step 4 = -23.2 Nm/(rad/s) at 100 rad/s and step 5 about -2661 Nm at
% 60 rad/s (both with eta 0.933 / J 5.75); scale them by the step-3 ratio
% (this fit / docs) so the five levels are on the same basis.
docsStep3 = -16.2;
scale = T.ret.slope_measured(3) / docsStep3;
T.ret.level_bp = [0 0.2 0.4 0.6 0.8 1.0];
T.ret.slope = [0, T.ret.slope_measured, -23.2 * scale, -2661 / 60 * scale];
T.ret.w_bp = [0 40 80 120 160 200];
T.ret.table = T.ret.slope(:) * T.ret.w_bp;     % rows: level, cols: output shaft rad/s

% ---- service brake gain (truck wheels on the ground) ---------------------------------
q = S.brk > 0.03 & ~isnan(S.temp) & S.speed > 0.3;
kb = S.Tb(q) ./ S.brk(q);
A = [ones(nnz(q), 1), S.temp(q), S.speed(q) / r];
cb = A \ kb;
T.brk.gain_coef = cb.';     % Nm per unit input = c0 + c1*temp + c2*wheel_rad_s
T.brk.gain_rel_rms = rms((kb - A * cb) ./ kb);
T.brk.temp_bp = [0 50 100 150 200 250];
T.brk.w_bp = [0 10 20 30 40 60];
[TT, WW] = ndgrid(T.brk.temp_bp, T.brk.w_bp);
T.brk.gain_table = max(cb(1) + cb(2) * TT + cb(3) * WW, 20000);

% ---- vehicle mass, rolling coefficient and locked inertia from the speed ----------------
% Force balance with the engine torque (not the reported output torque):
%   eta*ratio*Te*fd/r - c_air*v^2 = m*a + crr*m*g + eta*J*(ratio*fd/r)^2*a
% The fixed-gear run has four gears, which separates m from J; the other runs use
% that J and fit m and crr only.
Sg = collect(opts.TrainRuns(1));
[mg, crrg, Jg, ng, rmsg] = mass_fit(Sg, eta_of(T), NaN, fd, r, g, cAir);
T.gb.J_locked = Jg;
T.gb.J_locked_run = opts.TrainRuns(1);
T.mass = struct();
for run = opts.AllRuns
    Sr = collect(run);
    key = matlab.lang.makeValidName(run);
    [m1, c1, ~, n1, r1] = mass_fit(Sr, T.gb.eta, Jg, fd, r, g, cAir);
    T.mass.(key) = struct("m", m1, "crr", c1, "n", n1, "rms_N", r1);
    % reported-output-torque version, for comparison with docs/29-30
    q = Sr.locked & Sr.brk == 0 & Sr.Tb == 0 & Sr.ratio < 2.8 & Sr.speed > 2 & ...
        Sr.settled & ~isnan(Sr.a) & Sr.straight;
    bm = [Sr.a(q), g * ones(nnz(q), 1)] \ (Sr.To(q) * fd / r - cAir * Sr.speed(q).^2);
    T.mass.(key).m_from_To = bm(1);
end
T.mass_fit_gear_run = struct("m", mg, "crr", crrg, "J", Jg, "n", ng, "rms_N", rmsg);

% ---- trailer brake force vs input (brake run residual) --------------------------------
Sb = collect("brake-20261020-152718");
mb = T.mass.brake_20261020_152718;
w2 = (Sb.ratio * fd / r);
Feng = T.gb.eta * w2 .* (Sb.Te - T.gb.J_locked * w2 .* Sb.a);
Feng(~Sb.locked) = 0;
Fneed = Feng - cAir * Sb.speed.^2 - mb.crr * mb.m * g - mb.m * Sb.a;
Fres = Fneed - Sb.Tb / r;
q = Sb.brk > 0.03 & Sb.speed > 2 & Sb.brake_age > 0.6 & ~isnan(Sb.a) & (Sb.locked | Sb.clutch >= 0.99);
bpB = [0.1 0.3 0.5 0.7 1.0];
T.brk.trailer_bp = 0; T.brk.trailer_force = 0; T.brk.trailer_n = 0;
for x = bpB
    w = q & abs(Sb.brk - x) < 0.03;
    if nnz(w) >= 10
        T.brk.trailer_bp(end+1) = x;
        T.brk.trailer_force(end+1) = median(Fres(w));
        T.brk.trailer_n(end+1) = nnz(w);
    end
end
T.brk.trailer_force = max(T.brk.trailer_force, 0);
if T.brk.trailer_bp(end) < 1
    T.brk.trailer_bp(end+1) = 1; T.brk.trailer_force(end+1) = T.brk.trailer_force(end);
end

T.train_runs = opts.TrainRuns;
T.created = string(datetime("now", "Format", "yyyy-MM-dd HH:mm"));
if opts.Write
    write_tables(T, fullfile(here, "longitudinal_fitted_tables.m"));
    if ~isfolder(fullfile(here, "results")), mkdir(fullfile(here, "results")); end
    fid = fopen(fullfile(here, "results", "fitted_maps.json"), "w");
    fwrite(fid, jsonencode(T, "PrettyPrint", true));
    fclose(fid);
end
end

% =====================================================================
function S = collect(runs)
% Concatenate the per-frame samples of the given runs with derived columns.
S = struct();
for run = runs
    D = load_recording(run);
    n = numel(D.t);
    s.t = D.t; s.speed = D.speed; s.thr = D.eff_thr; s.thr(s.thr < 1e-3) = 0;
    s.thr0 = s.thr == 0;
    s.brk = D.eff_brk; s.brk(s.brk < 1e-3) = 0;
    s.rpm = D.we * 60 / (2 * pi); s.we = D.we; s.Te = D.Te; s.To = D.To; s.wo = D.wo;
    s.ratio = D.ratio; s.clutch = D.clutch; s.ret = D.ret_level; s.eb = D.eb;
    s.temp = D.brake_temp;
    s.Tb = sum(D.wbt(:, 1:4), 2) + D.on_ground4 .* D.wbt(:, 5) + D.on_ground5 .* D.wbt(:, 6);
    s.locked = D.clutch == 0 & D.ratio > 0;
    s.dTe = sg_derivative(D.Te, D.t);
    s.dTe_fwd = [diff(D.Te) ./ diff(D.t); NaN];
    s.fwd_ok = [diff(D.t) < 0.03; false];
    s.dwo = sg_derivative(D.wo, D.t);
    s.dwe = sg_derivative(D.we, D.t, 5);
    s.a = sg_derivative(D.speed, D.t, 31);
    % frames within 1 s of a gear, clutch, retarder, engine brake or brake change
    ch = find(diff(D.ratio) ~= 0 | diff(D.clutch) ~= 0 | diff(D.ret_level) ~= 0 | ...
        diff(D.eb) ~= 0 | diff(D.eff_brk > 0) ~= 0);
    near = false(n, 1);
    for c = ch(:).'
        near(abs(D.t - D.t(c)) < 1.0) = true;
    end
    s.settled = ~near;
    kind = extractBefore(D.phase + ":", ":");
    s.straight = ~startsWith(kind, "uturn") & ~ismember(kind, ["turn", "sine"]);
    % time since the brake input was last zero
    age = zeros(n, 1);
    for i = 2:n
        if D.eff_brk(i) > 0.03, age(i) = age(i-1) + (D.t(i) - D.t(i-1)); end
    end
    s.brake_age = age;
    fn = fieldnames(s);
    for i = 1:numel(fn)
        if isfield(S, fn{i}), S.(fn{i}) = [S.(fn{i}); s.(fn{i})]; else, S.(fn{i}) = s.(fn{i}); end
    end
end
end

function e = eta_of(T)
e = T.gb.eta;
end

function [m, crr, J, n, res] = mass_fit(S, eta, J, fd, r, g, cAir)
% Least squares on frames with the clutch locked, no brake, away from changes,
% driving straight. J = NaN fits J too.
q = S.locked & S.brk == 0 & S.Tb == 0 & S.speed > 2 & S.settled & ~isnan(S.a) & ...
    S.straight & S.ret == 0;
w2 = S.ratio(q) * fd / r;
y = eta * w2 .* S.Te(q) - cAir * S.speed(q).^2;
if isnan(J)
    X = [S.a(q), g * ones(nnz(q), 1), eta * w2.^2 .* S.a(q)];
    b = X \ y;
    J = b(3);
else
    y = y - eta * J * w2.^2 .* S.a(q);
    X = [S.a(q), g * ones(nnz(q), 1)];
    b = X \ y;
end
m = b(1); crr = b(2) / b(1); n = nnz(q); res = rms(y - X * b);
end

function [x, y, n] = binned(xs, ys, edges, minN)
x = []; y = []; n = [];
for k = 1:numel(edges) - 1
    w = xs >= edges(k) & xs < edges(k+1);
    if nnz(w) >= minN
        x(end+1) = median(xs(w)); y(end+1) = median(ys(w)); n(end+1) = nnz(w); %#ok<AGROW>
    end
end
end

function write_tables(T, file)
% Write the fitted tables as a MATLAB function with literal values.
fid = fopen(file, "w");
c = onCleanup(@() fclose(fid));
fprintf(fid, "function T = longitudinal_fitted_tables()\n");
fprintf(fid, "%%LONGITUDINAL_FITTED_TABLES Tables fitted by fit_longitudinal_maps.m (generated, do not edit).\n");
fprintf(fid, "%%   Created %s from %s.\n", T.created, strjoin(T.train_runs, ", "));
emit(fid, "T", rmfield(T, ["mass", "created", "train_runs"]));
names = fieldnames(T.mass);
for i = 1:numel(names)
    m = T.mass.(names{i});
    fprintf(fid, "T.mass.%s = struct('m', %.1f, 'crr', %.5f, 'n', %d);\n", names{i}, m.m, m.crr, m.n);
end
fprintf(fid, "end\n");
end

function emit(fid, prefix, s)
fn = fieldnames(s);
for i = 1:numel(fn)
    v = s.(fn{i});
    name = prefix + "." + fn{i};
    if isstruct(v)
        emit(fid, name, v);
    elseif isnumeric(v) || islogical(v)
        fprintf(fid, "%s = %s;\n", name, mat2str(double(v), 6));
    end
end
end
