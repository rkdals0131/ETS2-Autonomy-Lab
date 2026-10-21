function [pred_ey, pred_epsi, pred_dt] = lcc_mpc_pred(xseq, status, C)
%LCC_MPC_PRED Predicted trajectory of the MPC plan (MATLAB Function "MPC_Pred").
%   xseq is the Adaptive MPC block's optimal state sequence x.seq ((p+1) x nx, row j+1 =
%   x(k+j|k), row 1 = the state x0 of lcc_mpc_prep). pred_ey, pred_epsi (C.pred.n x 1) are its
%   lateral error (reference point, left +, m) and heading error (rad) every C.pred.every steps:
%   sample i (0-based) at i*pred_dt s after the state's time, sample 0 = the measured inputs.
%   Both are relative to the reference the controller tracks (the inputs lateral_error_m,
%   heading_error_rad: the lane, or the node's merge reference). Valid in status 1 only; 0
%   otherwise. pred_dt = C.pred.every * C.Ts. Outputs only: the move does not depend on this.
%#codegen
pred_ey = zeros(C.pred.n, 1);
pred_epsi = zeros(C.pred.n, 1);
pred_dt = C.pred.every * C.Ts;
if status == C.status.ok
    idx = 1 + C.pred.every * (0:C.pred.n - 1);
    pred_ey(:) = xseq(idx, 1);
    pred_epsi(:) = xseq(idx, 2);
end
end
