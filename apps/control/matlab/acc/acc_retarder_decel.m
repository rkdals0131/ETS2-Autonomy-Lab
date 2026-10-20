function d = acc_retarder_decel(s, v, C)
%ACC_RETARDER_DECEL Extra deceleration of retarder step s (1..4) at speed v, m/s^2 (>= 0).
%   Retarder torque at the output shaft = slope(s) * output shaft speed, output shaft speed
%   = v/r*fd, force = torque*fd/r: d = slope*v*(fd/r)^2/m. Step 0 -> 0.
%#codegen
if s < 1
    d = 0;
else
    k = min(s, numel(C.veh.ret_slope));
    d = C.veh.ret_slope(k) * max(v, 0) * (C.veh.fd / C.veh.r)^2 / C.veh.m;
end
end
