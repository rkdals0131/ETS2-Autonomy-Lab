function C = lcc_controller_params()
%LCC_CONTROLLER_PARAMS Tuning and vehicle data of LCC_Controller.slx (lateral MPC).
%   C = lcc_controller_params(). Vehicle numbers are the measured ones of lcc_plant_params.m
%   (the controller predicts with the same identification); tuning values are justified in
%   README.md ("튜닝").
C.Ts = 0.02;                 % s, drive_speed node rate (50 Hz)

% ---------------- MPC horizon ------------------------------------------------------------------
C.p = 75;                    % prediction steps (1.5 s): 42 m at 100 km/h, inside the 60 m preview
C.blocks = [2 3 5 5 10 10 15 25];   % move blocking: 8 free steering-rate moves over the horizon

% ---------------- contract ------------------------------------------------------------------------
C.n_preview = 61;            % curvature_preview points
C.preview_ds = 1.0;          % m, point i at s + i*ds from the reference point (drive axle)

% ---------------- steering (docs/31, virtual_eps.hpp) -----------------------------------------
C.angle_per_unit = 0.70;     % rad per applied unit (docs/31)
C.delta_max = 0.63;          % rad = 0.9 units (Ackermann measured to 0.9, docs/31)
% wheel-angle rate the inner loop can deliver: VirtualEps saturates at |u| = 1, so
% d(eff)/dt <= a(v) - c(v)*|eff| (units/s). Table of virtual_eps.hpp (4x2 LS, measured).
C.eps.kmh = [0 20 40 60 80 88.2 96.8];
C.eps.a = [0.650 0.570 0.489 0.401 0.320 0.284 0.261];
C.eps.c = [0 0.939 1.624 1.995 2.108 2.084 2.093];
C.rate_margin = 0.75;        % share of the deliverable rate the MPC may plan (keeps the P term alive)
% actuator seen by the MPC: wheel angle follows the planned wheel angle with a first-order lag
% tau_act (VirtualEps rate feedforward + P 6.5 rad/s, one game frame + one 20 ms sample of delay).
% Identified on the plant actuator (validate_lcc_plant.m, "actuator approximation": planned angle
% ramps sent as target = angle at the end of the step + rate, fit 95.5-99 %).
C.tau_kmh = [20 40 60 80 100];
C.tau_act = [0.01 0.059 0.071 0.059 0.073];   % s (20 km/h fit 0.001; 0.01 keeps the model proper)

% ---------------- vehicle (lcc_plant_params.m, identify_lcc_plant.m) ------------------------------
C.yaw.kmh = [0 25 50 60 80 90 100];
C.yaw.dL_trailer   = [0 -0.050 -0.082 -0.097 -0.119 -0.132 -0.141];
C.yaw.slip_trailer = [0  0.042  0.065  0.070  0.084  0.086  0.090];
C.yaw.dL_truck     = [0  0.008  0.026  0.029  0.038  0.040  0.046];
C.yaw.slip_truck   = [0 -0.027 -0.053 -0.065 -0.084 -0.092 -0.098];
C.trailer.a = 0.478;         % hitch behind the drive axle (fit; config 0.508)
C.trailer.Lt = 7.851;        % hitch -> effective trailer axle (fit; config middle axle 7.923)

% ---------------- costs ----------------------------------------------------------------------------
% outputs y = [ey, epsi, delta_cmd, ay_corr, e_trailer] (lcc_mpc_model.m)
C.w.y = [12 6 0 0 0];        % lateral error (m), heading error (rad)
C.w.trailer = 0;             % trailer axle lateral error (option, README "트레일러 쓸림")
C.w.u = 0.5;                 % steering rate (rad/s)
C.w.du = 4;                  % change of steering rate per 20 ms step
C.ay_corr_max = 3.0;         % m/s^2, soft limit of the lateral acceleration the controller adds
                             % on top of the lane's (v^2*kappa): comfort of corrections
C.ecr.delta = 0.1; C.ecr.ay = 1;

% ---------------- disturbance observer (curvature bias) ------------------------------------------
C.obs.T = 2.0;               % s, time constant of the yaw-rate bias estimate
C.obs.max = 1.5e-3;          % 1/m, |bias| limit
C.obs.v_min = 5;             % m/s, observer frozen below

% ---------------- fallback (status 2, 3): linearized pure pursuit of lateral_controller.hpp -----
C.fb.lookahead_m = 6; C.fb.lookahead_time_s = 0.7;
C.v_min = 2.0;               % m/s: below, the fallback law steers (status 3)

% ---------------- status codes ---------------------------------------------------------------------
C.status.off = 0; C.status.ok = 1; C.status.qp_fail = 2; C.status.low_speed = 3; C.status.bad_input = 4;
end
