function [preview, kappa_back, kappa_here] = lcc_lane_sensor(s, tab, L)
%LCC_LANE_SENSOR What the node reads from its lane polyline at the reference point.
%   preview(i+1) = mean lane curvature over [s_i - w/2, s_i + w/2], s_i = s + i*ds, i = 0..n-1
%   (LanePath::curvature(s_i, w), w = L.window), kappa_back = the same at s - w/2 (the
%   baseline's curvature rate uses curvature(s +- w/2)), kappa_here = curvature at s.
%   L = P.lane (preview_ds, window; preview_n must be 61). tab = lcc_lane(...).tab.
%#codegen
n = 61;                      % contract size (P.lane.preview_n); constant for code generation
preview = zeros(n, 1);
w = L.window;
for i = 1:n
    si = s + (i - 1) * L.preview_ds;
    preview(i) = mean_curvature(tab, si, w);
end
kappa_back = mean_curvature(tab, s - w / 2, w);
[~, kappa_here] = lcc_lane_lookup(tab, s);
end

function k = mean_curvature(tab, s, w)
t1 = lcc_lane_lookup(tab, s + w / 2);
t0 = lcc_lane_lookup(tab, s - w / 2);
k = (t1 - t0) / w;
end
