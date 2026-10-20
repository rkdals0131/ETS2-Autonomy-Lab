function in = acc_scenario_input(s, opts)
%ACC_SCENARIO_INPUT Simulink.SimulationInput of acc_closed_loop.slx for one scenario.
%   in = acc_scenario_input(s) uses the scenario s (acc_scenarios.m) with the reference truck.
%   Options:
%     PlantMass      plant mass P.veh.m (kg, default 20,300; the force-based actuators, brake
%                    torques, retarder and engine, stay as measured, so deceleration per unit
%                    scales 1/m in the plant)
%     MassPrior      mass_prior_kg input of the controller (0 = unknown -> 20.3 t default)
%     Adaptive       false: the controller uses its fixed 20.3 t maps (C.est.adaptive = false,
%                    the estimator still runs and is logged)
%     PitchKnown     true: pitch_rad = asin(grade) reaches the controller, false: 0
%     ActuationValid true: the estimator gets the plant's applied throttle (shift cuts) and the
%                    delayed brake / retarder commands, false: its own previous commands
%     Grade          [t grade] rows, road grade sin(theta) (default flat)
%     Trailer        trailer brakes in the plant (default: PlantMass >= 15 t; below that the
%                    plant is the truck alone, P.veh.trailer = 0, as in the game at 12 t)
%   The plant starts in the gear the automatic would hold at the start speed, at the steady
%   torque for that speed and mass.
arguments
    s (1,1) struct
    opts.PlantMass (1,1) double = 20300
    opts.MassPrior (1,1) double = 0
    opts.Adaptive (1,1) logical = true
    opts.PitchKnown (1,1) logical = true
    opts.ActuationValid (1,1) logical = true
    opts.Grade (:,2) double = [0 0]
    opts.Trailer (1,1) double = NaN
end
cl = 'acc_closed_loop';
[scn_lead, scn_disc] = acc_scenario_signals(s);
P = acc_plant_params();
P.veh.m = opts.PlantMass;
if isnan(opts.Trailer), P.veh.trailer = double(opts.PlantMass >= 15000); else, P.veh.trailer = opts.Trailer; end
g0 = 0;
if s.v0 > 1
    for k = P.at.top_gear:-1:P.at.min_gear       % the gear the automatic would hold (>= 1,100 rpm)
        if s.v0 * P.at.kv * P.at.ratio(k) >= 1100, g0 = k; break; end
    end
end
P.init.v0 = s.v0;
gr0 = opts.Grade(1, 2);
if g0 > 0
    P.init.we0 = s.v0 * P.at.kv * P.at.ratio(g0) * pi/30;
    Fres = P.veh.c_air*s.v0^2 + P.veh.crr*P.veh.m*P.veh.g + P.veh.m*P.veh.g*gr0;
    P.init.Te0 = Fres * P.veh.r / (P.veh.fd * P.at.ratio(g0) * P.gb.eta);
end
grade = opts.Grade;
if size(grade, 1) == 1, grade = [0 grade(2); s.T grade(2)]; end
in = Simulink.SimulationInput(cl);
in = in.setModelParameter("StopTime", num2str(s.T));
in = in.setVariable("scn_lead", scn_lead);
in = in.setVariable("scn_disc", scn_disc);
in = in.setVariable("scn_grade", grade);
in = in.setVariable("scn_mass_prior", opts.MassPrior);
in = in.setVariable("scn_pitch_known", double(opts.PitchKnown));
in = in.setVariable("scn_act_valid", double(opts.ActuationValid));
in = in.setVariable("P", P, "Workspace", "acc_plant");
in = in.setVariable("AT_init_gear", g0, "Workspace", "acc_plant");
if ~opts.Adaptive
    C = acc_controller_params();
    C.est.adaptive = false;
    in = in.setVariable("C", C, "Workspace", "ACC_Controller");
end
end
