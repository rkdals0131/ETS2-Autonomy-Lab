function S = replay_segment(D, t0, t1, opts)
%REPLAY_SEGMENT Replay the recorded commands of D between t0 and t1 on acc_plant.slx.
%   S = replay_segment(D, t0, t1) starts the plant from the recorded speed, gear,
%   engine speed and torque at t0 and feeds the recorded commands (cmd_thr, cmd_brk,
%   SDK retarder step, engine brake switch, brake temperature). The automatic gearbox
%   chooses gears and drives the clutch by itself. S has t (recording time), v_rec,
%   v_sim, gear_rec, gear_sim, rpm_rec, rpm_sim, clutch_sim, phase.
%   With opts.Run = false only the Simulink.SimulationInput is returned in S.in.
arguments
    D struct
    t0 (1,1) double
    t1 (1,1) double
    opts.Run (1,1) logical = true
    opts.P struct = struct()
end
model = 'acc_plant';
if ~bdIsLoaded(model), load_system(model); end
if isempty(fieldnames(opts.P)), P = acc_plant_params(); else, P = opts.P; end
k = find(D.t >= t0 & D.t <= t1);
t = D.t(k) - D.t(k(1));
ret = D.ret_step(k); ret(isnan(ret)) = 0;
eb = D.eb(k); eb(isnan(eb)) = 0;
bt = D.brake_temp(k); bt = fillmissing(bt, "nearest"); bt(isnan(bt)) = P.acc.brake_temp;
u = [t, D.cmd_thr(k), D.cmd_brk(k), ret, eb, bt, zeros(size(t))];
u(isnan(u)) = 0;
k0 = k(1);
P.init.v0 = max(0, D.speed(k0));
g0 = D.gear(k0);
if isnan(D.rpm(k0)), P.init.we0 = P.eng.idle_rpm*pi/30; else, P.init.we0 = D.rpm(k0)*pi/30; end
if ~isnan(D.Te(k0)), P.init.Te0 = D.Te(k0); end
in = Simulink.SimulationInput(model);
in = in.setExternalInput(u);
in = in.setModelParameter("StopTime", num2str(t(end)));
in = in.setVariable("P", P, "Workspace", model);
in = in.setVariable("AT_init_gear", double(max(g0, 0)), "Workspace", model);
f = fieldnames(P.at);           % the chart reads AT_* (the workspace code derives them from P)
for i = 1:numel(f), in = in.setVariable("AT_" + f{i}, P.at.(f{i}), "Workspace", model); end
S.in = in; S.k = k; S.t = D.t(k);
S.v_rec = D.speed(k); S.gear_rec = D.gear(k); S.rpm_rec = D.rpm(k); S.phase = D.phase(k);
S.ret = ret; S.brk = D.cmd_brk(k); S.thr = D.cmd_thr(k);
if opts.Run
    S = replay_collect(S, sim(in));
end
end
