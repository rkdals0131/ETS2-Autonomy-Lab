function [Ad, Bd, Cm, Dm, steady_slip, rack] = lcc_mpc_model(v, wheelbase, trailer, C)
%LCC_MPC_MODEL Prediction model of the lateral MPC at speed v (discrete, zero-order hold, C.Ts).
%   The move is the game's relative steering input u itself (the node's VirtualEps only inverts
%   its table while the MPC steers, lcc_mpc_post.m), so the actuator in the model is the measured
%   steering rack with the measured game delay instead of an approximation of a closed inner loop.
%   States  x = [ey; epsi; delta; hitch; m_1 .. m_nh]
%     ey        lateral error of the reference point (drive axle), left +
%     epsi      heading error
%     delta     actual front wheel angle = angle_per_unit * applied steering
%     hitch     psi_truck - psi_trailer (decoupled without trailer)
%     m_i       the move of i steps ago, u(k-i): the commands still on their way to the game
%   Inputs  [u; w]: u = steering input of this step (MV, normalized [-1, 1]), w = lane curvature
%           + bias (MD, 1/m)
%   Outputs y = [ey; epsi; delta; ay_corr; e_trailer; jerk]
%     ay_corr   v*d(epsi)/dt: lateral acceleration on top of the lane's v^2*w
%     e_trailer lateral error of the effective trailer axle (linearized, trailer only)
%     jerk      v*(v/L)*d(delta)/dt: the truck's lateral jerk (m/s^3) with the input acting in
%               this step; its weight makes the MPC smooth over short lane wiggles (README)
%   Rack (docs/31, virtual_eps.hpp table): d(delta)/dt = angle_per_unit*a(v)*u(t - D) - c(v)*delta,
%   c scaled by C.act.c_scale (closed-loop steady curves). D = C.act.delay_s from the state time;
%   D = (nd + f)*Ts: over a step the game applies u(k-nd-1) for f*Ts, then u(k-nd).
%   Kinematics (lcc_vehicle_deriv.m linearized): r = v*delta/L_eff, L_eff = wheelbase + dL(v),
%   reference-point side velocity -slip(v)*r, trailer (v*hitch - (slip + a)*r)/L_t.
%   steady_slip = slip(v): the heading error of a steady curve is slip*w (reference of y2).
%   rack = [angle_per_unit*a(v), c(v)] of the model (for the input conversion and the observer).
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
a = C.trailer.a; Lt = C.trailer.Lt;
ka = C.angle_per_unit * interp_clamped(C.eps.kmh, C.eps.a, kmh);
kc = C.act.c_scale * interp_clamped(C.eps.kmh, C.eps.c, kmh);
g = v / L;                               % yaw rate per wheel angle
% continuous part z = [ey; epsi; delta; hitch], inputs [u_applied, w]
Ac = [0 v -slip*g 0
      0 0 g 0
      0 0 -kc 0
      0 0 g*(1 + (a + slip)/Lt) -v/Lt];
Bu = [0; 0; ka; 0];
Bw = [0; -v; 0; 0];
Ts = C.Ts;
nd = C.act.nd; f = C.act.frac; nh = nd + 1;
% exact discretization of the split step (u(k-nd-1) for f*Ts, then u(k-nd))
Mn = expm([Ac Bu; zeros(1, 5)] * ((1 - f) * Ts));
Mo = expm([Ac Bu; zeros(1, 5)] * (f * Ts));
Mw = expm([Ac Bw; zeros(1, 5)] * Ts);
Phi = Mw(1:4, 1:4);
Gnew = Mn(1:4, 5);
Gold = Mn(1:4, 1:4) * Mo(1:4, 5);
Gw = Mw(1:4, 5);
n = 4 + nh;
Ad = zeros(n, n); Bd = zeros(n, 2);
Ad(1:4, 1:4) = Phi;
Ad(1:4, 4 + nh) = Gold;                  % m_{nd+1}
if nd == 0
    Bd(1:4, 1) = Gnew;                   % the move itself acts late in this step
else
    Ad(1:4, 4 + nd) = Ad(1:4, 4 + nd) + Gnew;
end
Bd(1:4, 2) = Gw;
Bd(5, 1) = 1;                            % m_1(k+1) = u(k)
for i = 2:nh
    Ad(4 + i, 3 + i) = 1;                % m_i(k+1) = m_{i-1}(k)
end
Cm = zeros(6, n); Dm = zeros(6, 2);
Cm(1, 1) = 1; Cm(2, 2) = 1; Cm(3, 3) = 1;
Cm(4, 3) = v * g; Dm(4, 2) = -v^2;
% jerk: v*g*(ka*u_acting - kc*delta), u_acting = (1 - f)*u(k-nd) + f*u(k-nd-1)
vg = v * g;
Cm(6, 3) = -vg * kc;
Cm(6, 4 + nh) = vg * ka * f;
if nd == 0
    Dm(6, 1) = vg * ka * (1 - f);
else
    Cm(6, 4 + nd) = Cm(6, 4 + nd) + vg * ka * (1 - f);
end
if trailer
    Cm(5, 1:4) = [1 -(a + Lt) 0 Lt];
    Dm(5, 2) = -(a + Lt)^2 / 2;
end
steady_slip = slip;
rack = [ka kc];
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
