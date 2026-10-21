function mpcLcc = lcc_design_mpc(C)
%LCC_DESIGN_MPC MPC object of the lateral controller (Adaptive MPC block in LCC_Controller.slx).
%   mpcLcc = lcc_design_mpc(lcc_controller_params()) builds the controller on the nominal model
%   (60 km/h, trailer connected, wheelbase 3.158); the Adaptive MPC block replaces the model
%   every step with lcc_mpc_model at the current speed, wheelbase and trailer state.
%   MV u = the game's relative steering input (normalized), MD w = lane curvature (+ bias,
%   previewed over the horizon), outputs [ey; epsi; delta; ay_corr; e_trailer; jerk] (all computed from
%   the state the prep function predicts: custom state estimation, no disturbance model).
[Ad, Bd, Cm, Dm] = lcc_mpc_model(60/3.6, 3.158, true, C);
sys = ss(Ad, Bd, Cm, Dm, C.Ts);
sys = setmpcsignals(sys, "MV", 1, "MD", 2);
sys.InputName = ["steering_input", "curvature"];
sys.OutputName = ["ey", "epsi", "delta", "ay_corr", "e_trailer", "jerk"];
sys.StateName = ["ey", "epsi", "delta", "hitch", "u_k-" + string(1:C.act.nd + 1)];
old = mpcverbosity("off");
cleanup = onCleanup(@() mpcverbosity(old));
mpcLcc = mpc(sys, C.Ts, C.p, C.blocks);
setoutdist(mpcLcc, "model", ss(zeros(6, 1)));    % no output disturbance states
setEstimator(mpcLcc, "custom");
% steering input: the block also gets the bounds online (umin/umax inports)
mpcLcc.MV(1).Min = -C.u_max; mpcLcc.MV(1).Max = C.u_max;
mpcLcc.MV(1).ScaleFactor = 0.2;
mpcLcc.OV(1).ScaleFactor = 1;
mpcLcc.OV(2).ScaleFactor = 0.05;
mpcLcc.OV(3).ScaleFactor = 0.2;
mpcLcc.OV(4).ScaleFactor = 1;
mpcLcc.OV(5).ScaleFactor = 1;
mpcLcc.OV(6).ScaleFactor = 1;
mpcLcc.DV(1).ScaleFactor = 0.01;
% wheel angle within the steering range, correction lateral acceleration (comfort)
mpcLcc.OV(3).Min = -C.delta_max; mpcLcc.OV(3).Max = C.delta_max;
mpcLcc.OV(3).MinECR = C.ecr.delta; mpcLcc.OV(3).MaxECR = C.ecr.delta;
mpcLcc.OV(4).Min = -C.ay_corr_max; mpcLcc.OV(4).Max = C.ay_corr_max;
mpcLcc.OV(4).MinECR = C.ecr.ay; mpcLcc.OV(4).MaxECR = C.ecr.ay;
% weights are on scaled signals (signal / ScaleFactor): convert the physical weights
w = [C.w.y C.w.jerk]; w(5) = C.w.trailer;
sf = [1 0.05 0.2 1 1 1];
mpcLcc.Weights.OV = w .* sf;
mpcLcc.Weights.MV = C.w.u * 0.2;
mpcLcc.Weights.MVRate = C.w.du * 0.2;
mpcLcc.Weights.ECR = 1e5;
mpcLcc.Optimizer.SolverOptions.MaxIterations = 100;
mpcLcc.Optimizer.UseSuboptimalSolution = true;
end
