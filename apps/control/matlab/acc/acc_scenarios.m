function S = acc_scenarios()
%ACC_SCENARIOS Closed-loop test scenarios of the ACC (definitions only).
%   S = acc_scenarios() returns a struct array; acc_scenario_signals(S(i)) turns one into the
%   scn_lead / scn_disc matrices that acc_closed_loop.slx reads. Fields:
%     name, title, T (s), v0 (ego start speed m/s), vset ([t value] rows, m/s),
%     lead: x0 (m ahead of the ego front bumper at t = 0, rear bumper), v0, seg (rows
%       [t_start accel v_target]: from t_start accelerate at accel until v_target),
%       valid ([t value] rows), shift ([t gap] rows: put the lead gap m ahead of where the ego
%       truck would be at constant start speed at time t, for cut-ins / late detection)
%     resume ([t_on t_off] rows, driver resume pulses), time_gap (s).
kmh = 1/3.6;
th = 2.0; d0 = 5.0;
base = struct("name", "", "title", "", "T", 60, "v0", 0, "vset", [0 0], ...
    "lead", struct("x0", 500, "v0", 0, "seg", zeros(0, 3), "valid", [0 0], "shift", zeros(0, 2)), ...
    "resume", zeros(0, 2), "time_gap", th);
S = repmat(base, 1, 0);

s = base; s.name = "setspeed_60_80"; s.title = "Set speed 60 -> 80 km/h, no lead";
s.T = 45; s.v0 = 60*kmh; s.vset = [0 60*kmh; 5 80*kmh];
s.lead = struct("x0", 2000, "v0", 0, "seg", zeros(0, 3), "valid", [0 0], "shift", zeros(0, 2));
S(end+1) = s;

v = 80*kmh;
s = base; s.name = "lead_brake_2"; s.title = "Lead at 80 km/h brakes at -2 m/s^2 to 40 km/h";
s.T = 45; s.v0 = v; s.vset = [0 90*kmh];
s.lead = struct("x0", d0 + th*v, "v0", v, "seg", [10 -2 40*kmh], "valid", [0 1], "shift", zeros(0, 2));
S(end+1) = s;

s.name = "lead_brake_4"; s.title = "Lead at 80 km/h brakes at -4 m/s^2 to 40 km/h";
s.lead.seg = [10 -4 40*kmh];
S(end+1) = s;

% beyond the comfort range: the lead makes an emergency stop (EMERGENCY mode, down to -6)
s.name = "lead_emergency_stop"; s.title = "Lead at 80 km/h brakes at -6 m/s^2 to a stop (EMERGENCY)";
s.T = 30;
s.lead.seg = [10 -6 0];
S(end+1) = s;

s = base; s.name = "cut_in"; s.title = "Cut-in at 30 m, lead 10 km/h slower (80 -> 70 km/h)";
s.T = 60; s.v0 = v; s.vset = [0 v];
s.lead = struct("x0", 0, "v0", 70*kmh, "seg", zeros(0, 3), "valid", [0 0; 5 1], "shift", [5 30]);
S(end+1) = s;

v = 50*kmh;
s = base; s.name = "stop_and_go"; s.title = "Stop and go: lead stops from 50 km/h (-2 m/s^2), waits 5 s, restarts at 1 m/s^2";
s.T = 60; s.v0 = v; s.vset = [0 60*kmh];
tStop = 5 + v/2;
s.lead = struct("x0", d0 + th*v, "v0", v, "seg", [5 -2 0; tStop + 5 1 v], "valid", [0 1], "shift", zeros(0, 2));
s.resume = [tStop + 5 + 2, tStop + 5 + 2.3];     % driver presses resume 2 s after the lead goes
S(end+1) = s;

% the same with a long wait: the truck reaches standstill (HOLD); the lead leaves after the
% 3 s auto-resume window, so the driver's resume starts GO
s.name = "stop_and_go_hold"; s.title = "Stop and go, lead waits 15 s (HOLD, resume by the driver)";
s.T = 70;
s.lead.seg = [5 -2 0; tStop + 15 1 v];
s.resume = [tStop + 15 + 2, tStop + 15 + 2.3];
S(end+1) = s;

% lead leaves shortly after the truck has stopped: automatic GO without the resume input
s.name = "stop_and_go_auto"; s.title = "Stop and go, lead leaves right after the truck stops (automatic GO)";
s.T = 60;
s.lead.seg = [5 -2 0; tStop + 8.5 1 v];
s.resume = zeros(0, 2);
S(end+1) = s;

v = 80*kmh;
s = base; s.name = "stopped_vehicle"; s.title = "Approach a stopped vehicle from 80 km/h, first seen at 150 m";
s.T = 40; s.v0 = v; s.vset = [0 v];
s.lead = struct("x0", 0, "v0", 0, "seg", zeros(0, 3), "valid", [0 0; 2 1], "shift", [2 150]);
S(end+1) = s;

v = 60*kmh;
s = base; s.name = "lead_accel_away"; s.title = "Lead at 60 km/h accelerates away at 1 m/s^2 (set speed 80 km/h)";
s.T = 45; s.v0 = v; s.vset = [0 80*kmh];
s.lead = struct("x0", d0 + th*v, "v0", v, "seg", [5 1 100*kmh], "valid", [0 1], "shift", zeros(0, 2));
S(end+1) = s;
end
