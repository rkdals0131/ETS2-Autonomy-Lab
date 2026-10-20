function P = longitudinal_params(config, variant)
%LONGITUDINAL_PARAMS Parameters of longitudinal_plant.slx with their sources.
%   P = longitudinal_params("trailer")  Scania S 6x2 + empty box trailer (default)
%   P = longitudinal_params("truck")    the same truck without a trailer
%   P = longitudinal_params(config, "baseline") uses only the values written in
%   docs/29 and docs/30 before this model was fitted (eta 0.933, J 5.75, constant
%   2,500 Nm/s rise rate, linear part-load map, 0.2 s brake lag, no trailer brake,
%   no chassis force lag). It exists to show what the fitted structure changed.
%
%   Tables marked "fit" come from fit_longitudinal_maps.m through the generated
%   longitudinal_fitted_tables.m (training runs gear-20261020-151650 and
%   brake-20261020-152718). The model reads everything from the struct P.
%   P.init holds the initial state; validate_longitudinal.m sets it per segment.
arguments
    config (1,1) string {mustBeMember(config, ["trailer", "truck"])} = "trailer"
    variant (1,1) string {mustBeMember(variant, ["fitted", "baseline"])} = "fitted"
end
F = longitudinal_fitted_tables();

% ---------------- vehicle ----------------------------------------------------
P.veh.fd = 2.59;          % final drive, def (docs/29 현재 레퍼런스, docs/30 정적 사양)
P.veh.r = 0.506;          % wheel radius m, SDK wheel config (docs/29)
P.veh.c_air = 3.0;        % air resistance F = c_air*v^2 N, RTP 9.2 global physics (docs/30 정적 사양)
P.veh.g = 9.81;
if config == "trailer"
    % effective mass incl. rotating parts downstream of the gearbox output.
    % fit: output-torque force balance, mean of the three trailer runs
    % (gear 18,763 / brake 18,774 / sysid 18,497 kg). docs/29 트레일러 연결 says 19.9-20.6 t
    % with a different sample selection; validate_longitudinal.m uses the per-run value.
    % fit: engine-torque force balance (eta*ratio*Te with J_locked), three trailer runs
    % gear 19,925 / brake 20,420 / sysid 20,606 kg; docs/29 트레일러 연결: 19.9-20.6 t.
    % (The same regression on the reported output torque gives 18.5-18.8 t because that
    % channel subtracts the larger J_report.) validate_longitudinal.m uses the per-run value.
    P.veh.m = 20300;
    P.veh.crr = 0.0072;   % fit 0.0069-0.0077; docs/29: 0.0073-0.0082
    P.veh.trailer = 1;    % adds the trailer brake force table
else
    P.veh.m = 12000;      % docs/30 물리 검증 (11,967 kg, truck alone, can-probe run)
    P.veh.crr = 0.0072;   % docs/30 물리 검증
    P.veh.trailer = 0;
end
P.veh.tau_body = 0.2;     % s, lag between the wheel forces and the body acceleration.
                          % Measured: decel after a brake step and accel decay after the
                          % clutch opens both follow a first-order lag of about 0.2 s while
                          % the wheel brake torque / output torque change within one frame
                          % (sysid-20261020-144933, sysid-20261020-153130); value tuned in
                          % validate_longitudinal.m sweep (0.15-0.25 s
                          % within 0.015 m/s; 0.2 best).

% ---------------- engine (DC16 520) -----------------------------------------
P.eng.T_max = 2700;                                   % def engine torque (docs/30 정적 사양)
P.eng.curve_rpm = [300 440 1000 1300 1600 1900 2300 2500];   % def torque_curve
P.eng.curve_frac = [0.1 0.4 1 1 0.82 0.72 0.4 0.1];        % verified within 1 % (docs/30 기어 고정 맵)
P.eng.fric0 = F.eng.fric0;    % fit -215.1 Nm  (docs/30: about -217)
P.eng.fric1 = F.eng.fric1;    % fit -0.0512 Nm/rpm (docs/30: about -0.05), residual 0.0 Nm
P.eng.eb_rpm = F.eng.eb_rpm;  % fit, engine brake torque vs rpm (docs/30 table: -2,060 @950, -3,000 flat)
P.eng.eb_nm = F.eng.eb_nm;
P.eng.thr_bp = F.eng.thr_bp;  % fit, part-load map: Te = fric + frac(thr,rpm)*(full - fric)
P.eng.rpm_bp = F.eng.rpm_bp;  %   shape 1-(1-thr)^1.33 plus measured cell corrections
P.eng.frac_table = F.eng.frac_table;
P.eng.rate_te_bp = F.eng.rate_te_bp;      % fit, torque rise-rate limit (Nm/s) vs (Te, rpm)
P.eng.rate_rpm_bp = F.eng.rate_rpm_bp;    %   with the clutch locked (turbo lag)
P.eng.rate_up_table = F.eng.rate_up_table;
P.eng.rate_open = 40000;      % Nm/s with the clutch open: torque jumps within 2-3 frames
                              % (launch in sysid-20261020-144933)
P.eng.rate_down = F.eng.rate_down;   % Nm/s, torque drops within one frame
P.eng.tau = 0.02;             % s, tracking time constant inside the rate limits
P.eng.idle_rpm = 550;         % SDK rpm at idle (all runs)
P.eng.gov_gain = 200;         % Nm per rad/s idle governor (holds idle in neutral; assumed)
P.eng.J_free = F.eng.J_free;  % kg m^2, fit: free engine decay with the clutch open (5.85)
P.eng.max_rpm = 2600;
P.eng.eb_thr_max = 0.05;      % the engine brake works while the applied throttle is at most this
                              % (gear run: -2,991 Nm at eff_thr 0.02, -158 Nm at 0.08 as it decays)
P.eng.auto_eb_brake = 0.5;    % service brake input at which the game adds the engine brake by itself
                              % (profile g_motor_brake_auto 1): engine torque about -3,000 Nm from the
                              % first frame of every 0.5 / 1.0 step with the clutch closed and the
                              % engine brake flag still 0; never at 0.1-0.3 (brake/sysid runs).
                              % Set above 1 to disable.

% ---------------- gearbox + clutch (G33CM-R) ----------------------------------
P.gb.ratios = [20.81 16.16 12.57 9.76 7.56 5.87 4.55 3.53 2.77 2.15 1.66 1.29 1.0 0.78];  % def, reference only (input is dt.gear_ratio)
P.gb.eta = F.gb.eta;          % fit 0.972 from the Tier 1 output torque (gears 9-14, R^2 0.9996,
                              % rms 33 Nm); docs/30: 0.933 (all four gears, with low-gear noise)
P.gb.eta_coast = P.gb.eta;     % efficiency used for the drive force when the engine torque is negative
                              % (set from the sweep in validate_longitudinal; see below)
P.gb.J_report = F.gb.J_report;% fit 16.0 kg m^2: inertia inside the reported output torque channel
                              % To = eta*ratio*(Te - J_report*dw_e/dt). Used only for the To output.
P.gb.J_locked = F.gb.J_locked;% fit 4.6 kg m^2: engine-side inertia that the vehicle speed shows
                              % (gear run force balance with Te, m and J together); docs/30: 5.75
P.gb.clutch_lock = 0.5;       % clutch value below which engine speed = ratio*output speed
P.gb.C_clutch = 230000;       % Nm engine side: clutch torque capacity = C_clutch*e while slipping.
                              % Launches at c 0.95-0.97 need about 900-1,700 Nm engine side (force
                              % balance). C_clutch, K_slip and clutch_exp come from speed-error sweeps
                              % on launch/accel/throttle_step/pre_brake segments (validate_longitudinal):
                              % rms 0.33 m/s with e = 1-c, 0.28-0.30 m/s with e = (1-c)^1.5.
P.gb.K_slip = 3200;           % Nm per rad/s of slip per unit e below the capacity (same sweep)
P.gb.clutch_exp = 1.5;        % engagement e = (1-c)^clutch_exp scales both clutch terms
P.gb.track_tau = 0.02;        % s, free-engine state tracking while locked (numerical)
P.gb.launch_rpm = 1100;       % engine speed the game holds while slipping at full throttle
                              % (sysid-20261020-144933 launch: 1,050-1,145 rpm)
P.gb.slip_gain = 200;         % Nm per rad/s pulling the slipping engine to that speed (assumed)

% ---------------- retarder ----------------------------------------------------
P.ret.level_bp = F.ret.level_bp;  % retarder_level (def auxiliary_brakes 0.2 .. 1.0)
P.ret.w_bp = F.ret.w_bp;          % output shaft rad/s
P.ret.table = F.ret.table;        % Nm at the output shaft = slope(level)*w
                                  % levels 0.2/0.4/0.6 fit (gear run, inertia term with eta/J above):
                                  % -4.3 / -9.0 / -14.7 Nm/(rad/s); 0.8 and 1.0 from docs/30
                                  % "브레이크와 리타더 다시 보기" scaled to this basis (not measured here)
P.ret.eb_level = 0.99;            % level 1.0 (step 5) also applies the engine brake (docs/29)
P.ret.tau = 0.05;                 % s, torque follows a step change within ~0.1 s (gear run)

% ---------------- service brake -------------------------------------------------
P.brk.temp_bp = F.brk.temp_bp;    % deg C (SDK truck.brake.temperature, recorded input)
P.brk.w_bp = F.brk.w_bp;          % wheel rad/s (v/r)
P.brk.gain_table = F.brk.gain_table;  % fit: truck wheel brake torque sum per unit input (Nm),
                                      % = 119,080 - 274*temp - 507*w, rel. rms 2.7 %
P.brk.tau = 0;                    % s, pressure lag of the wheel brake torque. The Tier 1 wheel
                                  % torque reaches its value in the next frame (docs/30); the body
                                  % lag is P.veh.tau_body. Kept as a parameter (being measured).
P.brk.trailer_bp = F.brk.trailer_bp;        % fit: trailer brake force (N) vs input, force balance
P.brk.trailer_force = F.brk.trailer_force;  %   residual in brake-20261020-152718 (not measured directly)
                                            %   1.0 has no settled samples; it repeats 0.5
P.brk.mu_max = 1.1;           % total service brake force limit / (m*g): decel at brake 1.0 levels off
                              % at about 10.5-11 m/s^2 (sysid-20261020-144933 truck alone, sysid-
                              % 20261020-153130 trailer; docs/29: 1.0 is the tyre friction limit,
                              % 7.5-12.3 m/s^2 between runs)

% ---------------- initial state -----------------------------------------------
P.init.v0 = 0;
P.init.a0 = 0;
P.init.Te0 = P.eng.fric0 + P.eng.fric1 * P.eng.idle_rpm;
P.init.we0 = P.eng.idle_rpm * 2 * pi / 60;
P.init.ret0 = 0;

% ---------------- solver ----------------------------------------------------------
P.sim.step = 0.004;

if variant == "baseline"
    P.gb.eta = 0.933; P.gb.J_locked = 5.75; P.gb.J_report = 5.75;   % docs/30 기어 고정 맵
    P.gb.eta_coast = 0.933;
    P.eng.fric0 = -217; P.eng.fric1 = -0.05;                % docs/30
    % docs/30 table, resampled on the fitted breakpoints (same table size for fast restart)
    P.eng.eb_nm = interp1([950 1150 1450 1750 2050 2250], [-2060 -2972 -2989 -3005 -3020 -2954], ...
        P.eng.eb_rpm, "linear");
    P.eng.eb_nm = fillmissing(P.eng.eb_nm, "nearest");
    P.eng.frac_table = P.eng.thr_bp(:) * ones(1, numel(P.eng.rpm_bp));  % linear in throttle
    P.eng.rate_up_table = 2500 * ones(size(P.eng.rate_up_table));       % docs/30: about 2,500 Nm/s
    P.eng.rate_open = 2500;
    P.ret.table = [0; -3.5; -8.3; -13.7; -18.5; -23] * P.ret.w_bp;      % docs/30 물리 검증 table, no inertia term
    P.brk.gain_table = 90000 * ones(size(P.brk.gain_table));            % docs/30: 0.1 -> 9.0 kNm
    P.brk.tau = 0.2;                                                    % placeholder lag
    P.brk.trailer_force = 0 * P.brk.trailer_force;                      % trailer brake unknown
    P.veh.tau_body = 0.001;
    P.eng.auto_eb_brake = 2;                                            % not known before
    P.eng.eb_thr_max = 1e-3;
    P.brk.mu_max = 5;                                                   % no friction limit
    if config == "trailer", P.veh.m = 20300; P.veh.crr = 0.0077; end    % docs/29 트레일러 연결
end
end
