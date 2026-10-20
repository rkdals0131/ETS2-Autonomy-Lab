function [Ad, Bd, Cm, Dm, steady_slip] = lcc_mpc_model(v, wheelbase, trailer, C)
%LCC_MPC_MODEL Prediction model of the lateral MPC at speed v (discrete, zero-order hold, C.Ts).
%   States  x = [ey; epsi; delta; delta_cmd; hitch]
%     ey        lateral error of the reference point (drive axle), left +
%     epsi      heading error
%     delta     actual front wheel angle; follows delta_cmd with the lag C.tau_act(v)
%     delta_cmd planned wheel angle (the controller's own command, integral of the MV)
%     hitch     psi_truck - psi_trailer (decoupled without trailer)
%   Inputs  [u; w]: u = d(delta_cmd)/dt (MV, rad/s), w = lane curvature + bias (MD, 1/m)
%   Outputs y = [ey; epsi; delta_cmd; ay_corr; e_trailer]
%     ay_corr   v*d(epsi)/dt: lateral acceleration on top of the lane's v^2*w
%     e_trailer lateral error of the effective trailer axle (linearized, trailer only)
%   Kinematics (lcc_vehicle_deriv.m linearized): r = v*delta/L_eff, L_eff = wheelbase + dL(v),
%   reference-point side velocity -slip(v)*r, trailer (v*hitch - (slip + a)*r)/L_t.
%   steady_slip = slip(v): the heading error of a steady curve is slip*w (reference of y2).
%#codegen
kmh = abs(v) * 3.6;
if trailer
    dL = interp_clamped(C.yaw.kmh, C.yaw.dL_trailer, kmh);
    slip = interp_clamped(C.yaw.kmh, C.yaw.slip_trailer, kmh);
else
    dL = interp_clamped(C.yaw.kmh, C.yaw.dL_truck, kmh);
    slip = interp_clamped(C.yaw.kmh, C.yaw.slip_truck, kmh);
end
L = wheelbase + dL;
a = C.trailer.a; Lt = C.trailer.Lt; tau = interp_clamped(C.tau_kmh, C.tau_act, kmh);
g = v / L;                               % yaw rate per wheel angle
A = [0 v -slip*g 0 0
     0 0 g 0 0
     0 0 -1/tau 1/tau 0
     0 0 0 0 0
     0 0 g*(1 + (a + slip)/Lt) 0 -v/Lt];
B = [0 0
     0 -v
     0 0
     1 0
     0 0];
Cm = [1 0 0 0 0
      0 1 0 0 0
      0 0 0 1 0
      0 0 v*g 0 0
      0 0 0 0 0];
Dm = [0 0
      0 0
      0 0
      0 -v^2
      0 0];
if trailer
    Cm(5, :) = [1 -(a + Lt) 0 0 Lt];
    Dm(5, 2) = -(a + Lt)^2 / 2;
end
% zero-order hold
M = expm([A B; zeros(2, 7)] * C.Ts);
Ad = M(1:5, 1:5);
Bd = M(1:5, 6:7);
steady_slip = slip;
end

function y = interp_clamped(xs, ys, x)
n = numel(xs);
if x <= xs(1)
    y = ys(1);
elseif x >= xs(n)
    y = ys(n);
else
    i = 1;
    while x > xs(i + 1), i = i + 1; end
    f = (x - xs(i)) / (xs(i + 1) - xs(i));
    y = ys(i) + f * (ys(i + 1) - ys(i));
end
end
