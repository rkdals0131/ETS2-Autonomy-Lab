function P = lcc_plant_params(config)
%LCC_PLANT_PARAMS Parameters of lcc_plant.slx (Scania S 6x2 tag lift, lateral).
%   P = lcc_plant_params("trailer") or lcc_plant_params("truck") (truck alone). Every value
%   has its source next to it: docs/31_plant_parameters.md, docs/29_reference_truck_dynamics.md,
%   the SDK configuration of the runs (truck_config.json / configs.json), code
%   (ros2/src/ets2_autonomy/src/control/virtual_eps.hpp), or identify_lcc_plant.m
%   (results/plant_identification.json, game recordings re-analysed with the heading-derivative
%   yaw rate). "assumed" = no measurement.
arguments
    config (1,1) string {mustBeMember(config, ["trailer", "truck"])} = "trailer"
end
P.trailer_connected = config == "trailer";   % (logical; no strings in P: it feeds MATLAB Function blocks)

% ---------------- solver ----------------------------------------------------------------
P.sim.step = 0.005;          % s, ode4. 4 steps per 20 ms controller sample
P.sim.ctrl_dt = 0.02;        % s, node / controller rate (drive_speed 50 Hz)

% ---------------- geometry (docs/31 기하, SDK wheel positions; z grows rearwards) --------
P.geo.front_z = -1.640;      % steering axle
P.geo.drive_z = 1.518;       % drive axle = reference point (tag raised)
P.geo.L = 3.158;             % front -> drive axle, tag raised (docs/31: effective = geometric)
P.geo.L_tag_down = 3.68;     % effective, tag lowered (docs/31); only via the wheelbase input
P.geo.wheel_angle_per_unit = 0.70;   % rad front wheel angle per applied steering unit (docs/31, 20-80 km/h)
P.geo.eff_max = 0.94;        % assumed: applied steering limit used by lateral_sim_test.cpp
                             % (Ackermann measured up to eff 0.9, docs/31)

% ---------------- relative steering plant d(eff)/dt = a(v) u(t - delay) - c(v) eff --------
% docs/31 조향 계통, 6x2 LS fits. 0 km/h: steer-id-20261020-105120 (a 0.650, no centering).
% 88.2 / 96.8 km/h: 4x2 values (assumed equal; 6x2 measured to 80 km/h only, the 4x2 and 6x2
% tables agree within 0.01 at 20-80 km/h).
P.steer.kmh = [0 20 40 60 80 88.2 96.8];
if P.trailer_connected
    P.steer.a = [0.650 0.569 0.487 0.404 0.317 0.284 0.261];   % trailer connected (sysid-20261020-153130)
    P.steer.c = [0     0.923 1.619 2.010 2.100 2.084 2.093];
else
    P.steer.a = [0.650 0.569 0.488 0.404 0.315 0.284 0.261];   % truck alone, tag raised (sysid-20261020-113045)
    P.steer.c = [0     0.933 1.622 2.009 2.095 2.084 2.093];
end
% Steady state in closed loop: in the steady curves of the lane tests (lanetest-20261020-121048,
% -154003, -155105, 40-100 km/h, 10 curves) the command/applied ratio u/eff is 0.974 (0.966-0.989)
% of the LS c/a: the self-centering holds 2.6 % less than the LS fit. The plant uses c*c_scale.
P.steer.c_scale = 0.974;
P.steer.delay = 1/60;        % one game frame (docs/31 "지연 약 1 프레임"); with the 20 ms sample hold of
                             % the node this is the ~35 ms loop delay of virtual_eps.hpp. Used by the
                             % plant validation (sysid-era recordings).
P.steer.delay_game = 3 / 60; % drive_speed in the game (bag 20261021-163908, assist vs drive state):
                             % a command acts 3 frames (2-4) after the frame whose state it used, the
                             % applied value read is from that frame. Scenarios use this delay.

% ---------------- node inner loop (virtual_eps.hpp, code) ------------------------------------
% VirtualEps runs in the node at 50 Hz with the 4x2 table (virtual_eps.hpp :31-32); it is part of
% the actuator seen by the lateral controller (wheel angle target + rate -> applied steering).
P.eps.kmh = [0 20 40 60 80 88.2 96.8];
P.eps.a = [0.650 0.570 0.489 0.401 0.320 0.284 0.261];
P.eps.c = [0 0.939 1.624 1.995 2.108 2.084 2.093];
P.eps.bandwidth = 6.5;       % rad/s (DrivingParameters::steering_bandwidth)
P.eps.rate_limit = 0;        % units/s, 0 = off (default)

% ---------------- yaw response: kinematic bicycle with a speed-dependent wheelbase ----------
% r = v*tan(delta)/L_eff(v); the reference point (drive axle) moves sideways at -slip(v)*r
% (it lies slip(v) behind the no-slip point). identify_lcc_plant.m, heading-derivative yaw rate:
%   truck alone: L_eff 3.15-3.20 m (sines 0.2 Hz, steady turns, lanetest-121048 curves),
%                slip -0.03 (25 km/h) .. -0.10 m (99 km/h) from the steady heading error in curves
%   trailer:     L_eff 3.13 (20) -> 3.04 (80) -> 3.02 m (97 km/h) (sysid-153130 sines/turns,
%                lanetest-154003/-155105 curves agree within 0.01 m),
%                slip 0.04 (25) .. 0.09 m (97 km/h)
% L_eff - 3.158 is added to the wheelbase (3.158 tag raised; tag lowered + trailer: assumed same shift).
P.yaw.kmh = [0 25 50 60 80 90 100];
if P.trailer_connected
    P.yaw.dL   = [0 -0.050 -0.082 -0.097 -0.119 -0.132 -0.141];
    P.yaw.slip = [0  0.042  0.065  0.070  0.084  0.086  0.090];
else
    P.yaw.dL   = [0  0.008  0.026  0.029  0.038  0.040  0.046];
    P.yaw.slip = [0 -0.027 -0.053 -0.065 -0.084 -0.092 -0.098];
end

% ---------------- kinematic trailer (docs/31 트레일러, SDK configs.json) --------------------
% r_t = (v*sin(g) + (v_y - a*r)*cos(g)) / L_t, hitch angle g = psi_truck - psi_trailer.
% Fitted to the trailer/truck yaw-rate response of the 12 sines (identify_lcc_plant.m):
% a 0.478 m, L_t 7.851 m (config: truck hook z 2.026 -> a = 0.508 m behind the drive axle,
% trailer hook z -5.066 -> middle axle 7.923 m).
P.trailer.a = 0.478;
P.trailer.Lt = 7.851;
P.trailer.L_rear_axle = 5.0658 + 4.1614;   % hitch -> last trailer axle, 9.227 m (config)
P.trailer.width = 2.55;      % assumed (box trailer), for the swept-path note only

% ---------------- slow yaw-rate wander (game straights) --------------------------------------
% The game's lane tests wander +-2-3 cm with periods of 20-40 s on straights (baseline RMS
% 7-19 mm after 150 m), which the noise-free plant does not (< 1 mm). A yaw-rate disturbance
% of three slow sines reproduces that size (amplitude fitted to the baseline's straight RMS in
% validate_lcc_plant.m; the cause in the game is not known). Off unless a run sets amp > 0.
P.dist.amp = 0;              % rad/s per component
P.dist.amp_game = 0.0013;    % fit: baseline straight RMS 30/80/100 km/h ~ game (validate_lcc_plant.m)
P.dist.freq = [0.017 0.029 0.047];   % Hz
P.dist.phase = [0 2.1 4.2];  % rad (scenarios draw them)

% ---------------- lane sensor (node: LanePath::curvature) -----------------------------------
P.lane.preview_n = 61;       % curvature_preview points (contract)
P.lane.preview_ds = 1.0;     % m spacing, point 0 at the reference point
P.lane.window = 6;           % m, centered mean curvature (DrivingParameters::curvature_window_m)
P.lane.width = 3.5;          % lane_test.py lanes

% ---------------- initial state (scenario sets it) --------------------------------------------
P.init.ey = 0; P.init.epsi = 0; P.init.eff = 0; P.init.hitch = 0;
P.init.X = 0; P.init.Y = 0; P.init.psi = 0; P.init.s = 0;
% The lane itself is the model-workspace variable LANE = lcc_lane(segments).tab (fixed size).
end
