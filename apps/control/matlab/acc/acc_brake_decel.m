function d = acc_brake_decel(b, v, C)
%ACC_BRAKE_DECEL Net service-brake deceleration of the measured table (m/s^2, >= 0).
%   d = acc_brake_decel(b, v, C): table C.veh.brk_a (reference mass C.veh.m) at input b and
%   speed v, linear in both (the forward of acc_inverse_brake without the effectiveness factor).
%#codegen
vv = min(max(v, C.veh.brk_v(1)), C.veh.brk_v(end));
nb = numel(C.veh.brk_b);
row = zeros(1, nb);
for j = 1:nb
    row(j) = interp1(C.veh.brk_v, C.veh.brk_a(:, j).', vv);
end
d = interp1(C.veh.brk_b, row, min(max(b, 0), C.veh.brk_b(end)));
end
