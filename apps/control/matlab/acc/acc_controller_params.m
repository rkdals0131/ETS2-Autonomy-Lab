function C = acc_controller_params()
%ACC_CONTROLLER_PARAMS Tuning and vehicle maps of ACC_Controller.slx.
%   C = acc_controller_params() returns everything the controller needs. It does not
%   read the plant model; vehicle numbers are copied here from the measurements
%   (docs/31_plant_parameters.md, results of identify_actuator_lag.m) with their source.
%   The model workspace of ACC_Controller.slx runs this function (C) and
%   acc_design_mpc(C) (mpcComfort, mpcEmergency); the chart reads the MODE_* scalars.

C.Ts = 0.05;                       % s, controller sample time

% ---------------- vehicle (Scania S 6x2 + empty box trailer) ---------------------------
C.veh.m = 20300;                   % kg, docs/31 질량 트레일러 연결 19.9-20.6 t
C.veh.r = 0.506;                   % m wheel radius
C.veh.fd = 2.59;                   % final drive
C.veh.g = 9.81;
C.veh.crr = 0.0072;                % rolling resistance (docs/31: 0.0073-0.0082 with trailer; plant fit 0.0072)
C.veh.c_air = 3.0;                 % N/(m/s)^2
C.veh.eta = 0.972;                 % gearbox efficiency (Simulink longitudinal fit)
C.veh.ratio = [20.81 16.16 12.57 9.76 7.56 5.87 4.55 3.53 2.77 2.15 1.66 1.29 1.0 0.78];  % SDK gear 1..14
C.veh.launch_gear = 4;             % gear the automatic starts in (acc-20261020-192243, 11 of 17 launches)
C.veh.fric0 = 215.1;               % engine friction torque = fric0 + fric1*rpm (Nm), docs/31 엔진
C.veh.fric1 = 0.0512;
C.veh.curve_rpm = [300 440 1000 1300 1600 1900 2300 2500];   % full load = 2700*frac (def, verified)
C.veh.curve_frac = [0.1 0.4 1 1 0.82 0.72 0.4 0.1];
C.veh.T_max = 2700;
C.veh.thr_shape = 1.33;            % part load: (Te - fric)/(full - fric) = 1 - (1 - thr)^1.33 (docs/31)
C.veh.idle_rpm = 550;
C.veh.launch_rpm = 1100;           % engine speed while the launch clutch slips at full throttle
% Retarder steps 1-4: torque at the output shaft = slope * output shaft speed (Nm per rad/s).
% 1-3 fitted on the gear run (Simulink longitudinal fit), 4 from docs/30 on the same basis.
% Check against acc-20261020-192243 ladder (total incl. coasting): step 2 0.42 @69, 3 0.52 @63,
% 4 0.60 @56 km/h -> this table + coasting gives 0.43 / 0.53 / 0.61.
C.veh.ret_slope = [4.33 9.04 14.69 21.03];
% Retarder step 5 (+ engine brake, the gearbox downshifts): 2.37-2.46 m/s^2 total at 47-65 km/h,
% full effect after about 3.1 s (docs/31 ACC용 측정). Used as one block, not interpolated.
C.veh.ret5_decel = 2.3;
C.veh.ret5_delay = 3.1;
% Retarder 1-4 + service brake nearly add: retarder 3 + brake 0.05 / 0.1 at 70 km/h gives
% 1.61 / 2.79 m/s^2 against 1.70 / 2.82 from the parts (coasting counted once). The retarder's
% share counts 0.75 while the service brake is on (worst case of the two points).
C.veh.ret_blend = 0.75;
% Service brake: measured net deceleration (minus coasting) by input (columns) and speed (rows,
% m/s). 70 km/h: docs/31 blend table 0.02/0.05/0.1 -> 0.48/1.17/2.29; 20 km/h (docs/31): 0.1 -> 1.70,
% 0.2 -> 3.25, 0.4 -> 4.25. Larger inputs from acc-20261020-185529 stops (0.3 -> 3.95 at 94 deg C;
% 0.5 / 0.75 / 1.0 measured at 128 / 157 / 188 deg C, corrected to about 90 deg C with the fade of
% about 2 m/s^2 per 100 deg C, docs/31) and docs/31 1.0 -> 9.6 steady at 80 km/h. Not measured:
% 0.02/0.05 at low speed (scaled like 0.1), 0.2 at high speed, the change-over speed (20 -> 43 km/h).
% Underestimating the brake strength at large inputs errs on the safe side (more braking).
C.veh.brk_b = [0 0.02 0.05 0.1 0.2 0.3 0.4 0.5 0.75 1.0];
C.veh.brk_v = [0 5.6 12 30];
lo = [0 0.36 0.85 1.70 3.25 3.95 4.25 5.45 7.20 8.50];
hi = [0 0.48 1.17 2.29 3.30 4.15 4.90 6.00 8.50 9.60];
C.veh.brk_a = [lo; lo; hi; hi];
% Full-throttle acceleration in gear (shift gaps excluded), acc-20261020-192243 launch:1.0 and
% accel phases: 20-25 km/h 2.37, 25-40 1.66-1.86, 40-55 1.15-1.28, 55-65 0.97-1.04, 65-75
% 0.69-0.74 m/s^2. 0-10 km/h (clutch slip) about 2; above 75 km/h not measured (extrapolated).
C.veh.afull_v = [0 2.8 5.6 7 11 12.5 15.3 18 20.8 25];
C.veh.afull_a = [2.0 2.4 2.4 1.85 1.7 1.25 1.1 0.95 0.72 0.45];

% ---------------- MPC (upper level) ---------------------------------------------------
% Lags of the two acceleration paths (identify_actuator_lag.m, game recordings): brake
% Tp1 0.31 s + Td 0.02 s; throttle Tp1 0.33 s + Td 0.24 s (turbo); retarder ~0.18 s.
% The drive path (throttle + retarder 1-4) uses 0.5 s, the brake path 0.33 s. The lag from a_des
% to the plant acceleration through the allocator (identify_closed_loop_lag.m on the scenario
% runs) is in results/closed_loop_lag.json.
C.mpc.tau1 = 0.5;
C.mpc.tau2 = 0.33;
C.mpc.p = 80;                       % 4 s prediction
C.mpc.blocks = [2 3 5 10 20 40];    % move blocking (6 moves per MV)
C.mpc.th_nom = 2.0;                 % time gap built into output 1 (y1 = gap - th_nom*v)
% weights (dimensionless after the scale factors below)
C.mpc.scale_y = [10 2 5];           % gap term m, relative speed m/s, speed m/s
C.mpc.w_y_follow = [0.05 0.5 0];    % gap, relative speed (shaped reference), speed
C.mpc.w_y_cruise = [0 0 0.6];
C.mpc.w_u = [0.05 1.0];             % drive path, brake path: braking costs 20x more (brake-sparing)
C.mpc.w_du = [2.0 3.0];             % jerk penalty per path (normal mode)
C.mpc.w_du_emerg = [0.2 0.05];
C.mpc.jerk_comf = [1.0 1.5 2.0 2.5];   % m/s^3: drive path down/up, brake path apply/release (sum <= 3 when braking)
C.mpc.jerk_emerg = [3 3 12 6];
C.mpc.a_min_comf = -3.0;            % m/s^2, total a_des lower limit in normal following
C.mpc.a_min_emerg = -6.0;           % only in EMERGENCY
C.mpc.a_max_comf = 0.8;             % m/s^2 comfort acceleration (also <= 0.85*full throttle)
C.mpc.a_max_follow = 0.4;           % m/s^2 while catching up a lead that is not accelerating
C.mpc.ecr_gap = 1;                  % soft minimum gap, equal concern
C.mpc.ecr_vset = 2;                 % soft set-speed upper bound

% ---------------- following policy -------------------------------------------------------
C.fol.d0 = 5.0;                     % m standstill gap (bumper to bumper)
C.fol.th_default = 2.0;             % s time gap for a truck with trailer
C.fol.th_min = 1.0;  C.fol.th_max = 3.0;
C.fol.safe_frac = 0.5;              % soft minimum gap = d0_safe + safe_frac*th*v
C.fol.d0_safe = 3.0;                % m
C.fol.a_approach = 2.0;             % m/s^2, most deceleration the closing-speed profile asks for
C.fol.k_gap = 0.4;                  % 1/s, slope of the profile near the target gap (2.5 s)
C.fol.vr_catchup = 1.5;             % m/s closing speed allowed when catching up a lead
C.fol.vr_ref_max = 25;              % m/s
C.fol.k_stop = 0.8;                 % 1/s profile slope in STOP (lead stopped: settle at d0 within ~1.3 s)
C.fol.k_open = 0.05;                % 1/s opening speed per m when closer than the target
C.fol.vr_open_max = 0.5;            % m/s
C.fol.vset_margin = 0.3;            % m/s allowed above the set speed (soft)
C.fol.a_lead_floor_T = 2.0;         % s: predicted lead deceleration limited to v_lead/2 s (the lead stops at most once in the prediction)
C.fol.a_filter_tau = 0.1;           % s, first-order filter on the measured ego acceleration

% ---------------- mode logic (Stateflow) ------------------------------------------------------
C.mode.follow_range_extra = 20;     % m beyond the distance needed to settle at a_approach
C.mode.follow_hyst = 30;            % m
C.mode.lead_lost_s = 0.5;           % s without lead before FOLLOW -> CRUISE
C.mode.v_lead_stopped = 0.5;        % m/s lead counts as stopped (STOP)
C.mode.v_lead_moving = 1.0;         % m/s lead moving again
C.mode.v_standstill = 0.15;         % m/s ego stopped (STOP -> HOLD)
C.mode.go_gap = 2.0;                % m the lead must pull away for an automatic GO
C.mode.go_window = 3.0;             % s after stopping in which GO is automatic
C.mode.go_done_v = 2.0;             % m/s GO -> FOLLOW / CRUISE
C.mode.ttc_on = 2.0;                % s EMERGENCY when TTC (closing faster than ttc_min_closing) below this ...
C.mode.dreq_on = 3.2;               % ... or the kinematic deceleration needed exceeds this (m/s^2)
C.fol.ttc_min_closing = 3.0;        % m/s: TTC for the mode logic only above this closing speed
C.mode.ttc_off = 4.0;               % s and required deceleration below dreq_off for emerg_hold s
C.mode.dreq_off = 2.0;
C.mode.emerg_hold = 1.0;

% ---------------- allocator (lower level) ---------------------------------------------------
C.alc.db_drive = 0.08;              % m/s^2 above coasting before the throttle is used
C.alc.db_drive_fast = 0.25;         % ... while the retarder is still on (otherwise it steps down first)
C.alc.db_drive_off = 0.0;           % m/s^2 above coasting below which the throttle is released
C.alc.kp = 0.2;                     % throttle PI on the acceleration error (m/s^2 per m/s^2; error against max(a, coasting))
C.alc.ki = 0.6;                     % 1/s
C.alc.i_max = 0.6;                  % m/s^2 integrator limit
C.alc.ret_up_frac = 0.7;            % step up when the need passes 70 % of the next step
C.alc.ret_dn_frac = 0.7;            % step down when it falls 70 % into the step below
C.alc.ret_dwell_up = 0.4;           % s between step increases (retarder 1-4 act in the same frame)
C.alc.ret_dwell_dn = 1.0;           % s between step decreases
C.alc.ret_dwell_rev = 2.5;          % s before a change against the direction of the last one
C.alc.ret_min_v = 3.0;              % m/s below this the retarder is released (weak, and the clutch opens)
C.alc.brk_on = 0.12;                % m/s^2 residual before the service brake comes in (normal)
C.alc.brk_on_urgent = 0.02;         % residual threshold when urgent
C.alc.brk_off = 0.03;               % m/s^2 residual where it is released again
C.alc.brk_ki = 0.0;                 % no integral on the brake (MPC closes the loop)
C.alc.brk_max = 1.0;
C.alc.hold_brake = 0.2;             % HOLD at standstill (docs/31: no creep, brake 0.2 holds)
C.alc.stop_brake_v = 0.5;           % m/s: in STOP below this speed at least stop_brake_min
C.alc.stop_brake_min = 0.05;
C.alc.ret5_on_mean = -2.2;          % m/s^2 predicted mean a_des over ret5_window for retarder 5
C.alc.ret5_on_now = -2.0;
C.alc.ret5_off_mean = -1.4;
C.alc.ret5_sustain = 2.0;           % s the condition must hold
C.alc.ret5_max_dreq = 2.5;          % m/s^2: not while the kinematic need is larger (closing on a lead)
C.alc.ret5_window = 4.0;            % s of the MPC prediction averaged (whole horizon)
C.alc.ret5_min_v = 8.3;             % m/s (30 km/h)
C.alc.use_engine_brake = false;     % true: use the B engine brake instead of retarder step 5
end
