function ac = acc_coast(v, C, gear, rpm)
%ACC_COAST Coasting acceleration (throttle 0, no retarder, no brake), m/s^2 (<= 0).
%   ac = acc_coast(v, C) uses rolling and air resistance plus the engine friction in a
%   typical gear (engine at 1,200 rpm in the gear the automatic would hold, about 0.085 m/s^2
%   at 70 km/h). With gear and rpm the engine friction of that gear is used (gear 0: none).
%   Check: 70 km/h gear 12 1,190 rpm -> -0.21 m/s^2 (docs/31 타력 감속, trailer: 0.21).
%#codegen
F = C.veh.c_air * v^2 + C.veh.crr * C.veh.m * C.veh.g * min(1, max(v, 0) * 10);
if nargin < 3
    % typical: the automatic keeps 1,000-1,300 rpm when coasting; friction torque about
    % 270 Nm at the wheels through the gear that gives 1,200 rpm
    if v > 1
        ratio = 1200 / (v * C.veh.fd / C.veh.r * 60 / (2*pi));
        ratio = min(ratio, C.veh.ratio(C.veh.launch_gear));
        F = F + (C.veh.fric0 + C.veh.fric1 * 1200) * ratio * C.veh.eta * C.veh.fd / C.veh.r;
    end
elseif gear >= 1 && gear <= numel(C.veh.ratio) && rpm > 800 && v > 3
    % engine friction through the engaged gear; near idle or at low speed the automatic opens
    % the clutch (neutral below about 600 rpm), so no engine drag is counted there, and the
    % low gears' drag is capped (the gear may be about to change)
    Ff = (C.veh.fric0 + C.veh.fric1 * rpm) * C.veh.ratio(gear) * C.veh.eta * C.veh.fd / C.veh.r;
    F = F + min(Ff, 0.35 * C.veh.m);
end
ac = -F / C.veh.m;
end
