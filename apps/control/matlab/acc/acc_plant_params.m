function P = acc_plant_params()
%ACC_PLANT_PARAMS Parameters of acc_plant.slx: the fitted longitudinal plant plus the
%   game's real automatic gearbox (Scania S 6x2 + empty box trailer, G33CM-R, ~20.3 t).
%
%   P = acc_plant_params() starts from longitudinal_params("trailer") in
%   apps/sysid/matlab/longitudinal (read-only, put on the path here) and adds
%     P.at   automatic shift model (AutoGearbox chart), fitted to the shift events of
%            acc-20261020-192243 (extract_shift_events.m, fit_auto_shift.m prints them)
%     P.acc  plant-side settings for the ACC work (brake temperature, overrides)
%   Every value below has its source next to it. Values marked "fit" were read off
%   the shift table that fit_auto_shift.m prints; "assumed" means no measurement.
here = fileparts(mfilename("fullpath"));
lon = fullfile(here, "..", "..", "..", "sysid", "matlab", "longitudinal");
if ~contains(path, "longitudinal"), addpath(lon); end
P = longitudinal_params("trailer");

% ---------------- solver ---------------------------------------------------------
% 5 ms so that the 20 ms gearbox chart and the 50 ms controller are integer multiples
% (the longitudinal model uses 4 ms).
P.sim.step = 0.005;

% ---------------- automatic gearbox (SDK gear numbers 1..14) ---------------------------
at.ratio = P.gb.ratios;                 % def G33CM-R, SDK n = ratios_forward[n-1] (docs/31 변속기)
at.kv = P.veh.fd / P.veh.r * 60 / (2*pi);   % engine rpm per (m/s) per unit gear ratio
at.dt = 0.02;                           % chart sample time s
at.top_gear = 14;
at.min_gear = 2;                        % lowest driving gear seen (launch 0->2, 4->2 at 5.8 km/h)
% Upshift when the engine speed of the current gear exceeds
% up_rpm(thr) - up_slope(thr)*(gear - 4).
% fit, acc-20261020-192243, engine speed from road speed at the frame the clutch starts
% to open (gear 4/6/7/8/9): thr 0.2 -> 1,496/1,474/1,462/1,452; 0.4 -> 1,571/1,546/-/1,524/1,514
% (2->4 at 1,596); 0.7 -> 1,713/1,701/-/1,685; 1.0 -> 1,900-1,919/1,897-1,907/-/1,898-1,901
% (10->12 at 1,897); 0.5 -> 1,624-1,667 (6->8); thr 0 after retarder 5: 7->8 at 1,490-1,504.
at.up_thr = [0 0.2 0.4 0.5 0.7 1.0];
at.up_rpm = [1530 1496 1571 1650 1713 1910];
at.up_slope = [11 11 12 10 7 1.5];
% Target = the highest gear whose engine speed right after the shift (from the threshold
% speed above) stays at or above up_min(target gear): fit, 66 of the 69 upshifts of the run are
% reproduced (the other 3: 7->8 right after retarder 5 is released, model 7->9) with 899 rpm
% for targets <= 6, 910 for 7-9 and 935 from 10 (target 6 accepted
% at 899.6, target 9 rejected at 896.6-899.9 and target 8 accepted at 929.6, target 10
% rejected at 928 and accepted at 1,026). The game skips gears this way (4->6->8->10 at full
% throttle, 4->6->7->8->9 at 0.2).
at.up_min = [899 899 899 899 899 899 910 910 910 935 935 935 935 935];
% Downshift when the predicted engine speed (speed + min(a,0)*t_pred) drops below
% dn_rpm(thr, gear) = dn0 + thr^3*dn0_thr - (dn_slope + thr^3*dn_slope_thr)*(12 - gear).
% (thr^3 and the steeper throttle slope 30 keep the game's partial-throttle landings: after
% 8->10 at 0.7 it stays in 10 at 1,026 rpm, after 8->10 at 1.0 in 10 at 1,159 rpm.)
% fit: coasting/braking 12->11 at 952-954, 11->10 at 924-929, 10->9 at 899-904,
% 9->7 at 882, 4->2 at 768 rpm (= 955 - 24*(12-g)); kickdown at full throttle 12->11 at
% 1,252 and 9->8 at 1,126 rpm (= 1,252 - 42*(12-g)).
at.dn0 = 955; at.dn0_thr = 297; at.dn_slope = 24; at.dn_slope_thr = 30;
at.t_pred = 0.75;                       % s, shift duration used for the prediction (assumed = open + close)
at.dn_target = 1000;                    % fit: target = highest lower gear with predicted rpm >= this
                                        % (12->11, 11->10, 10->9 at brake 0.05, 10->8 at 0.1)
% Retarder step 5 or engine brake: the gearbox raises the engine speed (docs/31 변속기).
% fit: 12->9, 11->8, 9->7, 8->6, 6->4 start at 1,003-1,501 rpm; the new gear is the
% lowest one whose engine speed at the start stays <= 2,500 rpm (2,154-2,496 seen).
at.eb_trig = 1505;
at.eb_max = 2500;
% Neutral (clutch opens, SDK gear 0): fit, with the throttle closed the clutch opens at
% 546-681 rpm under light braking and at 778-940 rpm under brake >= 0.5.
at.neutral_rpm = 600;
at.neutral_brk = 0.5;
at.neutral_brk_rpm = 900;
% Shift timing, fit: every shift of the run has 0.50 s from clutch opening to the SDK gear
% change and 0.233 s from there to the clutch closed; the throttle comes back 0.80-0.82 s
% after the clutch started to open. The clutch opens in about 0.13 s. Downshifts blip the
% applied throttle to 0.2 while the clutch is open (rev matching, eff_thr 0.12-0.2).
at.t_open = 0.50;
at.t_ramp_open = 0.13;
at.t_close = 0.233;
at.t_thr = 0.08;
at.blip = 0.2;
at.dwell = 0.2;                          % s in gear before the next shift (fit: first upshift 0.45 s after the launch clutch closed)
% Launch from neutral: fit, the clutch is fully open 0.1 s after the throttle, the SDK gear
% goes 0 -> 4 at 0.2 s (0 -> 2 in 6 of 17 launches, which then shift 2->4 at 7 km/h; 4 is
% used here), then the clutch slips while the engine is held near a throttle-dependent speed
% (1.0: 1,050-1,150; 0.7: 860-970; 0.4: 730-800; 0.2: 735-770 rpm) until the gearbox side
% catches up, then closes within about 0.25 s. Slip time 1.15-1.37 s (thr 0.4-1.0), 4.1 s (0.2).
at.launch_gear = 4;
at.launch_thr = [0 0.2 0.4 0.7 1.0];
at.launch_t_engage = 0.2;
at.launch_t_close = 0.25;
at.launch_sync = 0.92;                   % close when gearbox-side speed >= this * engine speed (assumed)
% Clutch capacity while slipping (engine side, Nm) ramps linearly to cap_max in cap_t s.
% The plant's clutch law fed with the game's clutch values transmits far more than the game
% does (the game holds the engine at the launch speed by itself), so the chart outputs the
% clutch value that reproduces the measured launch speed. fit (fit_auto_shift.m, fminsearch
% on the replayed speed of the first 4-8 s, rms 0.17 / 0.09 / 0.14 / 0.14 m/s for 0.2 / 0.4 /
% 0.7 / 1.0). Throttle 0 is not measured (assumed).
at.launch_cap_max = [200 651 647 1557 2113];
at.launch_cap_t = [2.31 2.31 0.44 0.69 0.50];
at.C_clutch = P.gb.C_clutch;             % same clutch law as the plant (e = (1-c)^exp)
at.clutch_exp = P.gb.clutch_exp;
at.creep_thr = 0.02;                     % throttle command that starts a launch (assumed)
P.at = at;

% ---------------- plant-side settings for the ACC work ----------------------------------
P.acc.brake_temp = 80;     % deg C, constant for closed-loop runs (acc-20261020-192243 median 87,
                           % blend phases 52-120)
% Engine brake (retarder step 5, B) at the wheels: the engine torque channel reads -3,000 Nm
% but the deceleration it gives is smaller than eta*ratio*Te says. fit (validate_acc_plant.m,
% blend:5:0.0 in gear 9 at 63-40 km/h: game 2.45, model 2.62 m/s^2 unscaled, 2.43 with 0.82;
% docs/31 남은 불일치: engine brake in gear 8 about 12 % weaker than the model). The friction
% torque while coasting is not scaled (the retarder 0/3 blends match within 0.03 m/s^2).
P.acc.eb_scale = 0.82;
P.eng.eb_nm = P.acc.eb_scale * P.eng.eb_nm;
% Service brake strength vs input and speed. The fitted brake model reproduces the 70 km/h
% blend table (game 0.02/0.05/0.1 -> 0.69/1.38/2.50 m/s^2 total; replay 0.69/1.33/2.37) and the brake-to-stop
% blocks of acc-20261020-192243 (brake 0.3-1.0 from 50 km/h, about 10 m/s^2 at 1.0, cool brakes),
% but it is too strong for light braking at low speed: game 0.1 -> 1.81 and 0.2 -> 3.35 m/s^2
% at 20 km/h (docs/31 ACC용 측정; acc-20261020-185529 bins 0.1 -> 1.78-1.85 at 5-10 km/h) against
% 2.44 / 3.87 in the model. brk_target is the measured net deceleration (minus coasting); the
% plant multiplies its brake force by brk_scale = target / model for those cells and by 1 where
% the target is NaN (model kept). The 0.3-1.0 stops of acc-20261020-185529 (3.9-9 m/s^2) are
% not used: they ran with brakes at 94-188 deg C (fade about 2 m/s^2 per 100 deg C, docs/31).
% Assumed: 0.02/0.05 at low speed scale like 0.1; the change-over speed (20 -> 43 km/h).
P.acc.brk_b = [0 0.02 0.05 0.1 0.2 0.3 0.4 0.5 0.75 1.0];
P.acc.brk_v = [0 5.6 12 30];
lo = [0 0.36 0.85 1.70 3.25 NaN NaN NaN NaN NaN];
hi = [0 0.48 1.17 2.29 NaN NaN NaN NaN NaN NaN];
P.acc.brk_target = [lo; lo; hi; hi];
P.acc.brk_scale = brake_scale(P);
% Idle creep of the real automatic at low speed with the throttle closed (assumed model, one
% game observation 2026-10-21: truck alone, after a driver override the truck kept rolling at
% 1.08 m/s with brake 0.085 and the throttle closed, gear engaged; brake 0.2 holds it, and at
% standstill the clutch opens so it does not creep away, docs/31 정차 유지). Constant force up
% to creep_v_full, falling to 0 at creep_v_zero, while rolling in gear. 14 kN puts the plant's
% truck-alone equilibrium with brake 0.085 at 1.08 m/s (brake 10.8 kN + rolling 0.85 kN); at
% the engine about 175 Nm through SDK gear 2. Brake 0.2 gives about 32 kN there.
P.acc.creep_force = 14000;    % N
P.acc.creep_v_full = 0.8;     % m/s
P.acc.creep_v_zero = 2.5;     % m/s
P.acc.creep_on_v = 0.3;       % m/s in gear: creep armed
P.acc.creep_off_v = 0.05;     % m/s: standstill, clutch open
% Retarder step 5 + service brake: the game's 65-35 km/h medians (5: 2.37, 5 + 0.05: 2.47,
% 5 + 0.1: 3.07 m/s^2) look non-additive, but within one gear they add (gear 9, 47-42 km/h:
% 2.5 alone, 3.4-3.7 with 0.05). The medians are low because the faster deceleration puts two
% 0.73 s downshift gaps (no engine brake) inside the window. The shift model reproduces part of
% it (replay 2.90 / 3.51 against 2.47 / 3.07); no extra blending rule.
end

function K = brake_scale(P)
% Ratio of the measured net brake deceleration to the fitted model's static brake force / m
% (truck gain at P.acc.brake_temp and the wheel speed, plus the trailer table, friction limit).
K = ones(numel(P.acc.brk_v), numel(P.acc.brk_b));
for i = 1:numel(P.acc.brk_v)
    w = P.acc.brk_v(i) / P.veh.r;
    gain = interp2(P.brk.w_bp, P.brk.temp_bp, P.brk.gain_table, min(w, P.brk.w_bp(end)), P.acc.brake_temp);
    for j = 2:numel(P.acc.brk_b)
        b = P.acc.brk_b(j);
        F = b * gain / P.veh.r + interp1(P.brk.trailer_bp, P.brk.trailer_force, b);
        F = min(F, P.brk.mu_max * P.veh.m * P.veh.g);
        if ~isnan(P.acc.brk_target(i, j))
            K(i, j) = P.acc.brk_target(i, j) / (F / P.veh.m);
        end
    end
end
K(:, 1) = K(:, 2);
end
