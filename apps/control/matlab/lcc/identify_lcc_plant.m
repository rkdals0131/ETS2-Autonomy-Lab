function I = identify_lcc_plant()
%IDENTIFY_LCC_PLANT Lateral plant identification from the game recordings (heading-derivative yaw rate).
%   I = identify_lcc_plant() reads (lcc_load_recording, cached):
%     sysid-20261020-153130    trailer connected, tag up: sines 0.2/0.5/1.0 Hz and steady turns at 20-80 km/h
%     sysid-20261020-113045    truck alone, tag up: the same programme
%     lanetest-20261020-154003, -155105   trailer, closed-loop lane keeping (baseline controller)
%     lanetest-20261020-121048, -123506   truck alone, the same scenarios
%   and computes
%     1. effective wheelbase L_eff = v*delta/r, delta = 0.70*eff (wheel angle per unit, docs/31),
%        r = d(heading)/dt from the SDK placement. The SDK local angular velocity (frames "v_ang",
%        used by apps/sysid/analyze_sysid.py with a 2*pi scale) under-reads the heading rate by
%        4-11 % in the trailer runs (scale 6.5-7.0 instead of 2*pi), which is why docs/31 lists
%        L_eff 3.18 -> 3.30 m ("understeer"); the heading rate gives 3.13 -> 3.04 m.
%     2. sideslip of the reference point (drive axle) in steady curves: heading error / curvature.
%     3. kinematic trailer: hitch offset a (behind the drive axle) and length L_t fitted to the
%        trailer/truck yaw-rate frequency response r_t/r = (v - a*s)/(L_t*s + v).
%   Results: results/plant_identification.json (lcc_plant_params.m carries the numbers).
here = fileparts(mfilename("fullpath"));
I = struct();
runs = struct("name", ["sysid-20261020-153130", "sysid-20261020-113045"], "kind", ["trailer", "truck"]);
for i = 1:2
    D = lcc_load_recording(runs.name(i));
    [I.(runs.kind(i)).sines, I.(runs.kind(i)).turns] = sine_turns(D);
    I.(runs.kind(i)).run = runs.name(i);
    I.(runs.kind(i)).vang_scale = vang_scale(D);
end
% effective wheelbase vs speed: 0.2 Hz sines and steady turns
for kind = ["trailer", "truck"]
    S = I.(kind).sines; T = I.(kind).turns;
    s02 = S([S.hz] == 0.2);
    kmh = [s02.kmh];
    Le = ([s02.L_eff] + [T.L_eff]) / 2;
    I.(kind).Leff_table = struct("kmh", round(kmh), "L_eff_sine02", [s02.L_eff], "L_eff_turn", [T.L_eff], "L_eff_mean", Le);
    pfit = polyfit([0 kmh / 3.6], [3.158 Le], 1);
    I.(kind).Leff_line = struct("L0", pfit(2), "slope_per_mps", pfit(1));
end
% closed-loop lane tests
lt = struct("trailer", ["lanetest-20261020-154003", "lanetest-20261020-155105"], ...
            "truck", ["lanetest-20261020-121048", "lanetest-20261020-123506"]);
for kind = ["trailer", "truck"]
    C = struct([]);
    for run = lt.(kind)
        D = lcc_load_recording(run);
        C = [C, lane_curves(D)]; %#ok<AGROW>
    end
    I.(kind).lanetest_curves = C;
end
% kinematic trailer fit
S = I.trailer.sines;
v = [S.kmh] / 3.6; w = 2 * pi * [S.hz]; H = [S.trailer_gain] .* exp(1i * deg2rad([S.trailer_phase_deg]));
model = @(p) (v - p(1) * 1i * w) ./ (p(2) * 1i * w + v);
cost = @(p) sum(abs(model(p) - H).^2);
p = fminsearch(cost, [0.5 8.0]);
I.trailer.hitch_fit = struct("a_m", p(1), "Lt_m", p(2), "rms_complex_error", sqrt(cost(p) / numel(H)), ...
    "config_a_m", 2.0263 - 1.518, "config_Lt_middle_axle_m", 5.0658 + 2.8574);
Hc = model([2.0263 - 1.518, 5.0658 + 2.8574]);
for k = 1:numel(S)
    I.trailer.sines(k).model_gain_config = abs(Hc(k));
    I.trailer.sines(k).model_phase_config_deg = rad2deg(angle(Hc(k)));
    Hf = model(p);
    I.trailer.sines(k).model_gain_fit = abs(Hf(k));
    I.trailer.sines(k).model_phase_fit_deg = rad2deg(angle(Hf(k)));
end
fid = fopen(fullfile(here, "results", "plant_identification.json"), "w");
fwrite(fid, jsonencode(I, PrettyPrint=true)); fclose(fid);
print_summary(I);
end

% =====================================================================
function [S, T] = sine_turns(D)
r = gradient(D.psi, D.t);
rt = gradient(D.psi_t, D.t);
delta = 0.70 * D.eff;
S = struct([]); T = struct([]);
ph = unique(D.phase, "stable");
for i = 1:numel(ph)
    if startsWith(ph(i), "sine")
        k = D.phase == ph(i); t = D.t(k) - D.t(find(k, 1));
        p = split(ph(i), ":"); f = str2double(p(3));
        X = [sin(2*pi*f*t) cos(2*pi*f*t) ones(size(t)) t];
        cd = X \ delta(k); cr = X \ r(k); cv = X \ (D.vang(k) * 2 * pi);
        zd = complex(cd(1), cd(2)); zr = complex(cr(1), cr(2)); zv = complex(cv(1), cv(2));
        vm = mean(D.speed(k));
        s.phase = ph(i); s.kmh = round(vm * 3.6, 1); s.hz = f;
        s.L_eff = vm / abs(zr / zd);
        s.L_eff_vang = vm / abs(zv / zd);
        s.phase_deg = rad2deg(angle(zr / zd));
        s.trailer_gain = NaN; s.trailer_phase_deg = NaN;
        if all(isfinite(rt(k)))
            ct = X \ rt(k); zt = complex(ct(1), ct(2));
            s.trailer_gain = abs(zt / zr); s.trailer_phase_deg = rad2deg(angle(zt / zr));
        end
        if isempty(S), S = s; else, S(end+1) = s; end %#ok<AGROW>
    elseif startsWith(ph(i), "turn")
        k = find(D.phase == ph(i)); k = k(round(end/2):end);
        vm = mean(D.speed(k)); rm = mean(r(k));
        q.phase = ph(i); q.kmh = round(vm * 3.6, 1);
        q.L_eff = vm * mean(delta(k)) / rm;
        q.ay = vm * rm; q.radius_m = vm / rm;
        q.hitch_deg = NaN;
        if all(isfinite(D.psi_t(k))), q.hitch_deg = rad2deg(mean(D.psi(k) - D.psi_t(k))); end
        if isempty(T), T = q; else, T(end+1) = q; end %#ok<AGROW>
    end
end
end

function s = vang_scale(D)
r = gradient(D.psi, D.t);
sel = abs(r) > 0.05 & abs(D.vang) > 1e-4;
s = median(r(sel) ./ D.vang(sel));
end

function C = lane_curves(D)
% steady curve segments after 150 m (lane_test.summarize), heading rate from the placement
L = D.lat;
r = gradient(D.psi, D.t);
rr = interp1(D.t, r, L.t);
C = struct([]);
for sc = unique(L.scenario).'
    k = find(L.scenario == sc);
    travelled = L.remaining(k(1)) - L.remaining(k);
    j = k(travelled > 150 & abs(L.path_curvature(k)) > 1e-4);
    if numel(j) < 100, continue; end
    kap = L.path_curvature(j);
    if any(sign(kap) ~= sign(kap(1))), continue; end                 % one arc only (no S-curves)
    j = j(abs(kap - median(kap)) < 0.02 * abs(median(kap)));          % drop the curvature window ramps
    if numel(j) < 100, continue; end
    kap = L.path_curvature(j);
    c.run = D.run; c.scenario = sc;
    c.kmh = round(mean(L.speed(j)) * 3.6, 1);
    c.curvature = mean(kap);
    c.mean_offset_m = mean(L.offset(j));
    c.inward_offset_m = mean(L.offset(j)) * sign(c.curvature);
    c.L_eff = mean(L.speed(j) .* 0.70 .* L.applied(j)) / mean(rr(j));
    c.sideslip_point_m = mean(L.heading_error(j)) / c.curvature;
    if isempty(C), C = c; else, C(end+1) = c; end %#ok<AGROW>
end
end

function print_summary(I)
for kind = ["truck", "trailer"]
    fprintf("== %s (%s), heading rate / v_ang = %.3f (2*pi = 6.283)\n", kind, I.(kind).run, I.(kind).vang_scale);
    S = I.(kind).sines;
    for s = S
        fprintf("  %-20s L_eff %.3f (v_ang %.3f) phase %+.1f deg", s.phase, s.L_eff, s.L_eff_vang, s.phase_deg);
        if isfinite(s.trailer_gain), fprintf("  trailer/truck %.3f %+.1f deg", s.trailer_gain, s.trailer_phase_deg); end
        fprintf("\n");
    end
    for q = I.(kind).turns
        fprintf("  %-20s L_eff %.3f a_y %.2f R %.0f m hitch %.2f deg\n", q.phase, q.L_eff, q.ay, q.radius_m, q.hitch_deg);
    end
    fprintf("  L_eff line: %.3f %+.4f*v\n", I.(kind).Leff_line.L0, I.(kind).Leff_line.slope_per_mps);
    for c = I.(kind).lanetest_curves
        fprintf("  lane %s #%d %5.1f km/h k %.5f inward %+.1f mm L_eff %.3f slip point %.3f m\n", c.run, c.scenario, ...
            c.kmh, c.curvature, 1000 * c.inward_offset_m, c.L_eff, c.sideslip_point_m);
    end
end
h = I.trailer.hitch_fit;
fprintf("trailer fit: a %.3f m, L_t %.3f m (config %.3f / %.3f), rms %.3f\n", h.a_m, h.Lt_m, h.config_a_m, h.config_Lt_middle_axle_m, h.rms_complex_error);
end
