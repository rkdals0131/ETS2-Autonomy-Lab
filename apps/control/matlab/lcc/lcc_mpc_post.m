function [wheel_angle_cmd, wheel_rate_cmd, status, mem] = lcc_mpc_post(mv, qp_status, mode, fb_angle, fb_rate, ...
    x0, md0, obs, pred_rows, delta, conv, C)
%LCC_MPC_POST Contract outputs from the MPC move (MATLAB Function "MPC_Post").
%   status 0 off (enable false), 1 MPC, 2 QP failure (fallback law), 3 low speed (fallback law),
%   4 non-finite / out-of-range input (hold the measured wheel angle, rate 0).
%   MPC: the move mv is the game's steering input for this step. The node's VirtualEps sends
%   u = (rate/k + c*applied + bandwidth*(target/k - applied))/a (k = angle_per_unit, its own a, c
%   table = C.eps): with the target equal to the measured angle (applied = delta/k) its P term is
%   0 whatever its bandwidth, and the rate feedforward k*a*s - c*delta makes it send exactly s.
%   s = mv + ub: the move plus the rack input bias (obs(2), lcc_mpc_prep), so the game rack acts
%   as the model with mv. wheel_angle_cmd = delta (measured), wheel_rate_cmd = conv(1)*s -
%   conv(2)*delta. mem = [epsi_pred; kb; mpc_active; delta_pred; ub; u(k-1) .. u(k-nd-1)] for
%   the next step (model inputs mv); epsi_pred, delta_pred are the model's heading error and
%   wheel angle one step ahead (observers).
%#codegen
nh = C.act.nd + 1;
mem = zeros(C.mem_n, 1);
dmeas = 0;
if isfinite(delta), dmeas = delta; end
if mode == 0 || mode == 4
    wheel_angle_cmd = dmeas;
    wheel_rate_cmd = 0;
    status = uint8(mode);
elseif mode == 3 || qp_status < 0
    wheel_angle_cmd = min(max(fb_angle, -C.delta_max), C.delta_max);
    wheel_rate_cmd = fb_rate;
    if mode == 3
        status = uint8(C.status.low_speed);
    else
        status = uint8(C.status.qp_fail);
    end
else
    u = mv;
    s = min(max(mv + obs(2), -1), 1);
    wheel_angle_cmd = dmeas;
    wheel_rate_cmd = conv(1) * s - conv(2) * dmeas;
    status = uint8(C.status.ok);
    pred = pred_rows * [x0; u; md0];
    mem(1) = pred(1);
    mem(2) = obs(1);
    mem(3) = 1;
    mem(4) = pred(2);
    mem(5) = obs(2);
    mem(6) = u;
    mem(7:5 + nh) = x0(5:3 + nh);         % shift the inputs on their way
end
end
