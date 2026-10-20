function [wheel_angle_cmd, wheel_rate_cmd, status, mem] = lcc_mpc_post(mv, qp_status, mode, fb_angle, fb_rate, ...
    x0, md0, kb, epsi_row, delta, C)
%LCC_MPC_POST Contract outputs from the MPC move (MATLAB Function "MPC_Post").
%   status 0 off (enable false), 1 MPC, 2 QP failure (fallback law), 3 low speed (fallback law),
%   4 non-finite / out-of-range input (hold the measured wheel angle, rate 0).
%   MPC: the planned wheel angle integrates the steering-rate move over one step,
%   wheel_angle_cmd = delta_cmd + Ts*mv (clamped to C.delta_max), wheel_rate_cmd = mv (the
%   VirtualEps rate feedforward). mem = [delta_cmd; mv; epsi_pred; kb; mpc_active] for the next
%   step; epsi_pred is the model's heading error one step ahead (bias observer).
%#codegen
mem = zeros(5, 1);
dmeas = 0;
if isfinite(delta), dmeas = delta; end
if mode == 0 || mode == 4
    wheel_angle_cmd = dmeas;
    wheel_rate_cmd = 0;
    status = uint8(mode);
    mem(1) = dmeas;
elseif mode == 3 || qp_status < 0
    wheel_angle_cmd = min(max(fb_angle, -C.delta_max), C.delta_max);
    wheel_rate_cmd = fb_rate;
    if mode == 3
        status = uint8(C.status.low_speed);
    else
        status = uint8(C.status.qp_fail);
    end
    mem(1) = wheel_angle_cmd; mem(2) = fb_rate;
else
    wheel_angle_cmd = min(max(x0(4) + C.Ts * mv, -C.delta_max), C.delta_max);
    wheel_rate_cmd = mv;
    status = uint8(C.status.ok);
    mem(1) = wheel_angle_cmd;
    mem(2) = mv;
    mem(3) = epsi_row * [x0; mv; md0];
    mem(4) = kb;
    mem(5) = 1;
end
end
