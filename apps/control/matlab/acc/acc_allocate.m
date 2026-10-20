function [throttle, brake, ret_step, engine_brake, a_des, mv_app] = acc_allocate( ...
    mode, mv, a_seq_mean, v, a_f, gear, rpm, dreq, C)
%ACC_ALLOCATE Lower level: desired acceleration -> throttle, service brake, retarder step.
%   mode 0 OFF / 4 HOLD bypass the MPC. Otherwise a_des = mv(1) + mv(2) (drive path +
%   brake path of the MPC) and, from the measurements in acc_controller_params:
%   - above coasting (+ db_drive): throttle from the inverse engine map in the current gear
%     plus a PI on the acceleration error (frozen in neutral, while the clutch slips and for
%     1 s after a gear change, anti-windup at the throttle limits); retarder released at once.
%   - below coasting: throttle 0, retarder steps 1-4 quantised with hysteresis (step up when
%     the need passes 70 % of the next step, down when it falls 70 % into the step below) and a
%     dwell (0.4 s up, 1.0 s down, 2.5 s before reversing direction); the service brake covers\n%     what is left (inverse brake table
%     at this speed, retarder share counted at 75 % while braking) once the residual exceeds
%     0.12 m/s^2, or at once when urgent (EMERGENCY, or the MPC brake path below -0.3).
%   - retarder step 5 only when the MPC's predicted mean request over the 4 s horizon is below
%     -2.2 m/s^2, the request is below -2.0 for 2 s at more than 30 km/h and the kinematic need\n%     (dreq) is at most 2.5 m/s^2 (not closing on a lead), never in
%     EMERGENCY; it takes 3.1 s to act and does not add with the brake, so the brake bridges the
%     first 3 s and step 5 is dropped (back to 4 + brake) when more than step 5 + 0.6 is needed.
%   mv_app is the MPC request actually used (fed back to the MPC as the last move).
%#codegen
persistent s ret_t ret_dir integ brk_on r5 r5_t r5_on_t last_gear gear_t drv
if isempty(s)
    s = 0; ret_t = 0; integ = 0; brk_on = false; r5 = false; r5_t = 0; r5_on_t = 0;
    last_gear = 0; gear_t = 10; drv = false; ret_dir = 0;
end
Ts = C.Ts;
throttle = 0; brake = 0; ret_step = 0; engine_brake = false;
gear_d = double(gear);
if gear_d ~= last_gear
    gear_t = 0;
    last_gear = gear_d;
else
    gear_t = min(gear_t + Ts, 10);
end

if mode == 0 || mode == 4
    drv = false; ret_dir = 0;
    s = 0; ret_t = 0; integ = 0; brk_on = false; r5 = false; r5_t = 0; r5_on_t = 0;
    a_des = 0;
    mv_app = [min(max(a_f, -1.5), 1.0); 0];
    if mode == 4
        brake = C.alc.hold_brake;
    end
    return
end

a_des = mv(1) + mv(2);
mv_app = mv;
urgent = mode == 6 || mv(2) < -0.3;
ac = acc_coast(v, C, gear_d, rpm);

% drive / coast decision with hysteresis: enter above coasting + db_drive (with the retarder
% still on only above coasting + db_drive_fast, otherwise it steps down first), leave below
% coasting + db_drive_off
if drv
    drv = a_des >= ac + C.alc.db_drive_off && mode ~= 6;
else
    drv = mode ~= 6 && ((s == 0 && ~r5 && a_des > ac + C.alc.db_drive) || a_des > ac + C.alc.db_drive_fast);
end
if drv
    % ---------------- drive ----------------
    s = 0; ret_t = C.alc.ret_dwell_dn; r5 = false; r5_t = 0; r5_on_t = 0; brk_on = false;
    e = a_des - max(a_f, ac);                          % ignore the decay of retarder/brake deceleration
    slipping = false;
    if gear_d >= 1 && gear_d <= numel(C.veh.ratio)
        rpm_road = v * C.veh.fd / C.veh.r * 60 / (2*pi) * C.veh.ratio(gear_d);
        slipping = rpm > 1.15 * rpm_road + 100;
    end
    freeze = gear_d < 1 || slipping || gear_t < 1.0;
    a_req = a_des + C.alc.kp * e + integ;
    throttle = acc_inverse_throttle(a_req, v, gear_d, rpm, C);
    if ~freeze && ~(throttle >= 1 && e > 0) && ~(throttle <= 0 && e < 0)
        integ = min(max(integ + C.alc.ki * e * Ts, -C.alc.i_max), C.alc.i_max);
    end
else
    % ---------------- coast / retarder / brake ----------------
    integ = integ * 0.95;
    need = ac - a_des;                                   % deceleration beyond coasting (> 0)
    ret_t = ret_t + Ts;
    d = zeros(1, 5);                                     % extra deceleration of steps 0..4
    for k = 1:4, d(k + 1) = acc_retarder_decel(k, v, C); end

    % retarder step 5 (sustained, predicted long deceleration only)
    r5_eff = 0;
    cond_on = (mode == 1 || mode == 2 || mode == 3) && v > C.alc.ret5_min_v && ...
        a_seq_mean <= C.alc.ret5_on_mean && a_des <= C.alc.ret5_on_now && dreq <= C.alc.ret5_max_dreq;
    if cond_on, r5_on_t = r5_on_t + Ts; else, r5_on_t = 0; end
    if ~r5 && r5_on_t >= C.alc.ret5_sustain
        r5 = true; r5_t = 0;
    end
    if r5
        r5_t = r5_t + Ts;
        r5_eff = C.veh.ret5_decel * min(1, r5_t / C.veh.ret5_delay);
        if a_seq_mean > C.alc.ret5_off_mean || v < C.alc.ret5_min_v - 1 || mode == 6 || ...
                (r5_t > C.veh.ret5_delay && need > r5_eff + 0.6)
            r5 = false; s = 4; ret_t = 0;
        end
    end

    if r5
        ret_step = 5;
        resid = need - r5_eff;
        if r5_t <= C.veh.ret5_delay && resid > C.alc.brk_on
            brake = acc_inverse_brake(resid, v, C);
        end
        brk_on = brake > 0;
    else
        if v < C.alc.ret_min_v
            s = 0;
        else
            % a change against the direction of the last one waits ret_dwell_rev (no hunting)
            up_ok = ret_t >= C.alc.ret_dwell_up && (ret_dir >= 0 || ret_t >= C.alc.ret_dwell_rev || urgent);
            dn_ok = ret_t >= C.alc.ret_dwell_dn && (ret_dir <= 0 || ret_t >= C.alc.ret_dwell_rev);
            if s < 4 && up_ok && need >= d(s + 1) + C.alc.ret_up_frac * (d(s + 2) - d(s + 1))
                s = s + 1; ret_t = 0; ret_dir = 1;
            elseif s > 0 && dn_ok && need <= d(s + 1) - C.alc.ret_dn_frac * (d(s + 1) - d(s))
                s = s - 1; ret_t = 0; ret_dir = -1;
            end
        end
        ret_step = s;
        k_ret = 1;
        if brk_on, k_ret = C.veh.ret_blend; end
        resid = need - k_ret * d(s + 1);
        if urgent, on_thr = C.alc.brk_on_urgent; else, on_thr = C.alc.brk_on; end
        if brk_on
            if resid < C.alc.brk_off && ~urgent, brk_on = false; end
        elseif resid > on_thr
            brk_on = true;
        end
        if brk_on
            brake = acc_inverse_brake(max(resid, 0), v, C);
        end
    end
    if mode == 3 && v < C.alc.stop_brake_v
        brake = max(brake, C.alc.stop_brake_min);
    end
end
brake = min(max(brake, 0), C.alc.brk_max);
throttle = min(max(throttle, 0), 1);
if C.alc.use_engine_brake && ret_step == 5
    ret_step = 4;
    engine_brake = true;
end
end
