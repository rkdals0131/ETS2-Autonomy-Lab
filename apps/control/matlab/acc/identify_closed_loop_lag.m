function R = identify_closed_loop_lag(names)
%IDENTIFY_CLOSED_LOOP_LAG Lag from the MPC request a_des to the plant acceleration.
%   R = identify_closed_loop_lag() reads results/scenario_<name>.json written by
%   run_acc_scenarios.m and fits a first-order + dead-time model (procest "P1D", System
%   Identification Toolbox) from a_des to the plant acceleration a in each run. This is the
%   lag the MPC actually sees through allocator + automatic gearbox + plant (+35 ms command
%   latency), to compare with the lags in its prediction model (C.mpc.tau1 = 0.5 s drive path,
%   C.mpc.tau2 = 0.33 s brake path). Gear-shift gaps are not removed (they show as misfit).
%   Writes results/closed_loop_lag.json.
arguments
    names (1,:) string = ["setspeed_60_80", "lead_brake_2", "lead_brake_4", "cut_in", "stopped_vehicle"]
end
here = fileparts(mfilename("fullpath"));
R = struct("name", {}, "K", {}, "Tp1_s", {}, "Td_s", {}, "fit_pct", {});
for n = names
    J = jsondecode(fileread(fullfile(here, "results", "scenario_" + n + ".json")));
    t = J.t; u = J.signals.a_des; y = J.signals.a;
    m = J.signals.v > 1 & J.signals.mode >= 1 & J.signals.mode ~= 4;
    k0 = find(m, 1); k1 = find(m, 1, "last");
    data = iddata(y(k0:k1), u(k0:k1), t(2) - t(1));
    sys = idproc("P1D");
    sys.Structure.Td.Maximum = 0.5;
    sys.Structure.Tp1.Maximum = 3;
    sys = procest(data, sys, procestOptions("Display", "off"));
    [~, fitPct] = compare(data, sys);
    R(end+1) = struct("name", n, "K", sys.Kp, "Tp1_s", sys.Tp1, "Td_s", sys.Td, "fit_pct", fitPct); %#ok<AGROW>
    fprintf("%-16s K %.2f  Tp1 %.2f s  Td %.2f s  fit %.0f %%\n", n, sys.Kp, sys.Tp1, sys.Td, fitPct);
end
fid = fopen(fullfile(here, "results", "closed_loop_lag.json"), "w");
fwrite(fid, jsonencode(R, PrettyPrint=true)); fclose(fid);
end
