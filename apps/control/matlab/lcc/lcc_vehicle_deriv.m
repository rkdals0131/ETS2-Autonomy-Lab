function [dx, r, ay, vy] = lcc_vehicle_deriv(x, delta, v, wheelbase, kappa, P, yaw_dist)
%LCC_VEHICLE_DERIV Kinematic truck (+ trailer) in lane (Frenet) and world coordinates.
%   x = [s; ey; epsi; hitch; X; Y; psi] at the reference point (drive axle, tag raised):
%     s      arc length of the projection on the lane centre line
%     ey     lateral error, left of the lane positive (LanePath offset)
%     epsi   heading error psi - lane heading
%     hitch  psi_truck - psi_trailer
%     X, Y, psi  world pose (lane frame: lane starts at the origin with heading 0)
%   delta front wheel angle (rad), v speed (m/s), wheelbase (m, 3.158 tag raised),
%   kappa lane curvature at s. P = lcc_plant_params(). yaw_dist (rad/s, optional) is added to
%   the yaw rate (slow wander seen on the game's straights, lcc_plant_params P.dist).
%   Yaw rate r = v*tan(delta)/L_eff(v), L_eff = wheelbase + dL(v); the reference point moves
%   sideways at vy = -slip(v)*r; trailer yaw rate (v*sin(g) + (vy - a*r)*cos(g))/L_t.
%#codegen
if nargin < 7, yaw_dist = 0; end
kmh = abs(v) * 3.6;
Leff = wheelbase + interp_clamped(P.yaw.kmh, P.yaw.dL, kmh);
slip = interp_clamped(P.yaw.kmh, P.yaw.slip, kmh);
r = v * tan(delta) / Leff + yaw_dist;
vy = -slip * r;
ey = x(2); epsi = x(3); g = x(4); psi = x(7);
sdot = (v * cos(epsi) - vy * sin(epsi)) / (1 - kappa * ey);
dx = zeros(7, 1);
dx(1) = sdot;
dx(2) = v * sin(epsi) + vy * cos(epsi);
dx(3) = r - kappa * sdot;
rt = (v * sin(g) + (vy - P.trailer.a * r) * cos(g)) / P.trailer.Lt;
dx(4) = r - rt;
dx(5) = v * cos(psi) - vy * sin(psi);
dx(6) = v * sin(psi) + vy * cos(psi);
dx(7) = r;
ay = v * r;
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
