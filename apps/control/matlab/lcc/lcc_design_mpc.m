function mpcLcc = lcc_design_mpc(C)
%LCC_DESIGN_MPC MPC object of the lateral controller (Adaptive MPC block in LCC_Controller.slx).
%   mpcLcc = lcc_design_mpc(lcc_controller_params()) builds the controller on the nominal model
%   (60 km/h, trailer connected, wheelbase 3.158); the Adaptive MPC block replaces the model
%   every step with lcc_mpc_model at the current speed, wheelbase and trailer state.
%   MV u = steering-rate of the planned wheel angle (rad/s), MD w = lane curvature (+ bias,
%   previewed over the horizon), outputs [ey; epsi; delta_cmd; ay_corr; e_trailer] (all
%   computed from the measured state: custom state estimation, no disturbance model).
[Ad, Bd, Cm, Dm] = lcc_mpc_model(60/3.6, 3.158, true, C);
sys = ss(Ad, Bd, Cm, Dm, C.Ts);
sys = setmpcsignals(sys, "MV", 1, "MD", 2);
sys.InputName = ["delta_rate", "curvature"];
sys.OutputName = ["ey", "epsi", "delta_cmd", "ay_corr", "e_trailer"];
sys.StateName = ["ey", "epsi", "delta", "delta_cmd", "hitch"];
old = mpcverbosity("off");
cleanup = onCleanup(@() mpcverbosity(old));
mpcLcc = mpc(sys, C.Ts, C.p, C.blocks);
setoutdist(mpcLcc, "model", ss(zeros(5, 1)));    % no output disturbance states
setEstimator(mpcLcc, "custom");
% steering rate: finite placeholders, the block overwrites them online (umin/umax inports)
mpcLcc.MV(1).Min = -0.4; mpcLcc.MV(1).Max = 0.4;
mpcLcc.MV(1).ScaleFactor = 0.2;
mpcLcc.OV(1).ScaleFactor = 1;
mpcLcc.OV(2).ScaleFactor = 0.05;
mpcLcc.OV(3).ScaleFactor = 0.2;
mpcLcc.OV(4).ScaleFactor = 1;
mpcLcc.OV(5).ScaleFactor = 1;
mpcLcc.DV(1).ScaleFactor = 0.01;
% planned wheel angle within the steering range, correction lateral acceleration (comfort)
mpcLcc.OV(3).Min = -C.delta_max; mpcLcc.OV(3).Max = C.delta_max;
mpcLcc.OV(3).MinECR = C.ecr.delta; mpcLcc.OV(3).MaxECR = C.ecr.delta;
mpcLcc.OV(4).Min = -C.ay_corr_max; mpcLcc.OV(4).Max = C.ay_corr_max;
mpcLcc.OV(4).MinECR = C.ecr.ay; mpcLcc.OV(4).MaxECR = C.ecr.ay;
% weights are on scaled signals (signal / ScaleFactor): convert the physical weights
w = C.w.y; w(5) = C.w.trailer;
sf = [1 0.05 0.2 1 1];
mpcLcc.Weights.OV = w .* sf;
mpcLcc.Weights.MV = C.w.u * 0.2;
mpcLcc.Weights.MVRate = C.w.du * 0.2;
mpcLcc.Weights.ECR = 1e5;
mpcLcc.Optimizer.SolverOptions.MaxIterations = 100;
mpcLcc.Optimizer.UseSuboptimalSolution = true;
end
