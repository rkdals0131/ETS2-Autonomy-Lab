function b = acc_inverse_brake(dec, v, C)
%ACC_INVERSE_BRAKE Service brake input for a net deceleration dec (m/s^2, >= 0) at speed v.
%   Inverts the measured table C.veh.brk_a(speed, input) at this speed (linear in both).
%   The result is continuous (not quantised): near 70 km/h 0.01 of input is about 0.23 m/s^2.
%#codegen
vv = min(max(v, C.veh.brk_v(1)), C.veh.brk_v(end));
nb = numel(C.veh.brk_b);
row = zeros(1, nb);
for j = 1:nb
    row(j) = interp1(C.veh.brk_v, C.veh.brk_a(:, j).', vv);
end
dd = min(max(dec, 0), row(end));
b = interp1(row, C.veh.brk_b, dd);
end
