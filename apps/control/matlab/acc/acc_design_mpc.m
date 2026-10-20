function [mpcComfort, mpcEmergency, M] = acc_design_mpc(C)
%ACC_DESIGN_MPC Linear MPC objects of the ACC upper level (MPC Toolbox).
%   [mpcComfort, mpcEmergency, M] = acc_design_mpc(acc_controller_params()) builds two
%   controllers on the same prediction model; the Multiple MPC Controllers block in
%   ACC_Controller.slx switches to the second one in EMERGENCY (faster brake jerk, lower
%   acceleration limit through the online bounds, less jerk weight).
%
%   Model (continuous, discretised with zero-order hold at C.Ts):
%     states  x = [gap d; relative speed vr = v_lead - v; ego speed v; a1; a2]
%             a1 = acceleration of the drive path (throttle, retarder 1-4), lag tau1
%             a2 = acceleration of the brake path (service brake), lag tau2
%     MVs     u1 = drive-path acceleration request, u2 = brake-path request (<= 0)
%     MD      a_lead (held constant over the horizon)
%     outputs y1 = d - th_nom*v   (gap term; reference and soft lower bound come online)
%             y2 = vr             (reference: shaped closing speed)
%             y3 = v              (reference: set speed; soft upper bound online)
%   The ego acceleration a = a1 + a2. The total request a_des = u1 + u2 goes to the allocator.
%   States are measured (custom state estimation): the controller passes x[k|k].
Ts = C.Ts;
t1 = C.mpc.tau1; t2 = C.mpc.tau2; thn = C.mpc.th_nom;
A = [0 1 0 0 0
     0 0 0 -1 -1
     0 0 0 1 1
     0 0 0 -1/t1 0
     0 0 0 0 -1/t2];
B = [0 0 0
     0 0 1
     0 0 0
     1/t1 0 0
     0 1/t2 0];
Cm = [1 0 -thn 0 0
      0 1 0 0 0
      0 0 1 0 0];
sys = ss(A, B, Cm, zeros(3, 3));
sys = setmpcsignals(sys, "MV", [1 2], "MD", 3);
sys.InputName = ["a1_req", "a2_req", "a_lead"];
sys.OutputName = ["gap_term", "rel_speed", "speed"];
sys.StateName = ["gap", "rel_speed", "speed", "a1", "a2"];
sysd = c2d(sys, Ts);
M.sysd = sysd;

mpcComfort = make(sysd, C, "comfort");
mpcEmergency = make(sysd, C, "emergency");
end

function obj = make(sysd, C, kind)
old = mpcverbosity("off");
cleanup = onCleanup(@() mpcverbosity(old));
obj = mpc(sysd, C.Ts, C.mpc.p, C.mpc.blocks);
setoutdist(obj, "model", ss(zeros(3, 1)));   % no output disturbance states: x is exactly the 5 measured states
setEstimator(obj, "custom");
Ts = C.Ts;
if kind == "comfort"
    j = C.mpc.jerk_comf; wdu = C.mpc.w_du; amin = C.mpc.a_min_comf;
else
    j = C.mpc.jerk_emerg; wdu = C.mpc.w_du_emerg; amin = C.mpc.a_min_emerg;
end
% MV bounds are finite here so that the online bounds of the block apply; the block
% overwrites them every step (drive path: no-brake deceleration .. a_max(v); brake path:
% a_min - drive minimum .. 0).
obj.MV(1).Min = -1.5; obj.MV(1).Max = 2.5;
obj.MV(2).Min = amin; obj.MV(2).Max = 0;
obj.MV(1).RateMin = -j(1) * Ts; obj.MV(1).RateMax = j(2) * Ts;
obj.MV(2).RateMin = -j(3) * Ts; obj.MV(2).RateMax = j(4) * Ts;
obj.MV(1).ScaleFactor = 2; obj.MV(2).ScaleFactor = 4;
obj.OV(1).ScaleFactor = C.mpc.scale_y(1);
obj.OV(2).ScaleFactor = C.mpc.scale_y(2);
obj.OV(3).ScaleFactor = C.mpc.scale_y(3);
% soft constraints, finite placeholders (online values from the prep block)
obj.OV(1).Min = -500; obj.OV(1).MinECR = C.mpc.ecr_gap;
obj.OV(3).Max = 60;   obj.OV(3).MaxECR = C.mpc.ecr_vset;
obj.DV(1).ScaleFactor = 4;
obj.Weights.MV = C.mpc.w_u;
obj.Weights.MVRate = wdu;
obj.Weights.OV = C.mpc.w_y_follow;     % overwritten online (FOLLOW / CRUISE)
obj.Weights.ECR = 1e5;
% the QP stays small: 6 blocked moves x 2 MVs + 1 slack
obj.Optimizer.SolverOptions.MaxIterations = 200;
obj.Optimizer.UseSuboptimalSolution = true;
end
