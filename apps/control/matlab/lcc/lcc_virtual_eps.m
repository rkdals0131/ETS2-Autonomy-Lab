function u = lcc_virtual_eps(wheel_angle, wheel_rate, eff, speed, E, angle_per_unit)
%LCC_VIRTUAL_EPS Port of VirtualEps::command (virtual_eps.hpp) without the optional slew limit.
%   u = lcc_virtual_eps(wheel_angle, wheel_rate, eff, speed, E, angle_per_unit): the node's
%   inner loop from the front wheel angle target (rad) and its rate (rad/s) to the relative
%   steering input u in [-1, 1]. As lateral_capi.cpp, the target is clamped to [-1, 1] units.
%   E = P.eps (table a(v), c(v) of virtual_eps.hpp, bandwidth). eff = applied steering (units).
%   u = (rate + c*eff + bandwidth*(target - eff)) / a, so d(eff)/dt = rate - bandwidth*error on
%   the measured plant.
%#codegen
target = min(max(wheel_angle / angle_per_unit, -1), 1);
rate = wheel_rate / angle_per_unit;
kmh = abs(speed) * 3.6;
n = numel(E.kmh);
if kmh >= E.kmh(n)
    a = E.a(n); c = E.c(n);
else
    i = 1;
    while kmh > E.kmh(i + 1), i = i + 1; end
    f = (kmh - E.kmh(i)) / (E.kmh(i + 1) - E.kmh(i));
    a = E.a(i) + f * (E.a(i + 1) - E.a(i));
    c = E.c(i) + f * (E.c(i + 1) - E.c(i));
end
u = min(max((rate + c * eff + E.bandwidth * (target - eff)) / a, -1), 1);
end
