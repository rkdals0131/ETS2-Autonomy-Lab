function S = replay_collect(S, out)
%REPLAY_COLLECT Resample the acc_plant outputs of a replay onto the recorded frames.
y = out.yout;
tr = S.t - S.t(1);
get = @(i, method) interp1(y{i}.Values.Time, squeeze(y{i}.Values.Data), tr, method, "extrap");
S.v_sim = get(1, "linear");
S.a_sim = get(2, "linear");
S.rpm_sim = get(3, "linear");
S.gear_sim = get(4, "previous");
S.clutch_sim = get(6, "previous");
S.err = S.v_sim - S.v_rec;
S.rms = sqrt(mean(S.err.^2, "omitnan"));
end
