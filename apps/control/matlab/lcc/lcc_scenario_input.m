function [in, lane, P, SCN] = lcc_scenario_input(s, config, ctrl, opts)
%LCC_SCENARIO_INPUT SimulationInput of lcc_closed_loop.slx for one lcc_scenarios() entry.
%   [in, lane, P, SCN] = lcc_scenario_input(s, "trailer" | "truck", 1 (baseline) | 2 (MPC)).
%   The lane starts at the origin with heading 0; the truck starts at its start, shifted by
%   -s.shift (lane left of the truck) and turned by -s.heading_deg, steering centred, trailer in
%   line, at the constant speed s.kmh (lane_test.py holds the speed with a pedal PI). The run
%   stops 5 m before the lane end (lane_test.py: remaining < 5).
%   Options: Kmh (override speed), Wheelbase (given to the controller, 3.158), Wander (amplitude
%   of the plant's slow yaw-rate disturbance P.dist.amp, rad/s per component, 0 = off; the
%   phases are drawn from the scenario index so the baseline and the MPC see the same one),
%   Window (curvature averaging window of the node's preview, m; 6 = curvature_window_m).
arguments
    s (1,1) struct
    config (1,1) string
    ctrl (1,1) double
    opts.Kmh (1,1) double = NaN
    opts.Wheelbase (1,1) double = 3.158
    opts.Wander (1,1) double = 0
    opts.Window (1,1) double = 6
end
lane = lcc_lane(s.segments);
P = lcc_plant_params(config);
kmh = s.kmh;
if isfinite(opts.Kmh), kmh = opts.Kmh; end
v = kmh / 3.6;
P.init.speed = v;
P.init.s = 0;
P.init.ey = -s.shift;
P.init.epsi = -deg2rad(s.heading_deg);
P.init.X = 0; P.init.Y = -s.shift; P.init.psi = -deg2rad(s.heading_deg);
P.init.eff = 0; P.init.hitch = 0;
P.dist.amp = opts.Wander;
P.lane.window = opts.Window;
st = RandStream("mt19937ar", "Seed", 100 + s.index);
P.dist.phase = 2 * pi * rand(st, 1, numel(P.dist.freq));
SCN = struct("speed", v, "wheelbase", opts.Wheelbase, "trailer", P.trailer_connected, "enable", true, ...
    "base", struct("lookahead_m", 6, "lookahead_time_s", 0.7, "window", opts.Window));
in = Simulink.SimulationInput("lcc_closed_loop");
in = in.setModelParameter("StopTime", num2str((lane.length - 5) / v));
in = in.setVariable("P", P, "Workspace", "lcc_plant");
in = in.setVariable("LANE", lane.tab, "Workspace", "lcc_plant");
in = in.setVariable("SCN", SCN, "Workspace", "lcc_closed_loop");
in = in.setVariable("LCC_CTRL", ctrl);
end
