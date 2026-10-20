function k = acc_brake_scale(C, m)
%ACC_BRAKE_SCALE Service brake deceleration per unit input relative to the measured table.
%   k = acc_brake_scale(C, m): C.veh.m/m above the reference mass (the brake torques are fixed,
%   so a heavier vehicle decelerates less per unit); below it interpolated in C.veh.brk_light_*
%   down to 0.6 at 12 t (the truck alone: without the trailer's brakes it decelerates less per
%   unit than the combination, docs/31).
%#codegen
if m >= C.veh.m
    k = C.veh.m / m;
else
    mm = min(max(m, C.veh.brk_light_m(1)), C.veh.brk_light_m(end));
    k = interp1(C.veh.brk_light_m, C.veh.brk_light_k, mm);
end
end
