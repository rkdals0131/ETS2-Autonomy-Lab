function lane = lcc_lane(segments, opts)
%LCC_LANE Lane tables for lcc_plant.slx from straight/arc segments (lane_test.py build_lane).
%   lane = lcc_lane([length curvature; ...]) prepends the 60 m lead-in straight of lane_test.py
%   (LEAD_IN) and returns
%     lane.tab   fixed-size uniform table for the plant (s0, ds, theta, kappa), N = 9001 points
%                at 0.5 m from s = -50 m: theta(s) is the lane heading (integral of the
%                curvature), kappa(s) the curvature; straight before 0 and after the end
%                (LanePath continues the last segment past the end).
%     lane.length  lane length (m)
%     lane.s, lane.x, lane.y, lane.theta   0.25 m polyline in the lane frame (start at the
%                origin, heading 0), for projections in the metrics
%   Options: LeadIn (60 m).
arguments
    segments (:,2) double
    opts.LeadIn (1,1) double = 60
end
seg = [opts.LeadIn 0; segments];
edges = [0; cumsum(seg(:, 1))];
lane.length = edges(end);
N = 9001; ds = 0.5; s0 = -50;
s = s0 + (0:N-1).' * ds;
assert(s(end) > lane.length + 100, "lane longer than the plant table");
[theta, kappa] = profile(s, seg, edges);
lane.tab = struct("s0", s0, "ds", ds, "theta", theta, "kappa", kappa);
% fine polyline for projections (integrate the exact arcs)
sp = (-50:0.25:lane.length + 100).';
[th, ~] = profile(sp, seg, edges);
x = zeros(size(sp)); y = zeros(size(sp));
i0 = find(sp == 0);
for k = i0+1:numel(sp)
    x(k) = x(k-1) + chord(th(k-1), th(k), 0.25, 1);
    y(k) = y(k-1) + chord(th(k-1), th(k), 0.25, 2);
end
for k = i0-1:-1:1
    x(k) = x(k+1) - chord(th(k), th(k+1), 0.25, 1);
    y(k) = y(k+1) - chord(th(k), th(k+1), 0.25, 2);
end
lane.s = sp; lane.x = x; lane.y = y; lane.theta = th;
lane.segments = seg;
end

% =====================================================================
function [theta, kappa] = profile(s, seg, edges)
theta = zeros(size(s)); kappa = zeros(size(s));
th0 = [0; cumsum(seg(:, 1) .* seg(:, 2))];
for k = 1:numel(s)
    if s(k) <= 0
        theta(k) = 0; kappa(k) = 0;
    elseif s(k) >= edges(end)
        theta(k) = th0(end); kappa(k) = 0;
    else
        j = find(s(k) >= edges, 1, "last");
        kappa(k) = seg(j, 2);
        theta(k) = th0(j) + seg(j, 2) * (s(k) - edges(j));
    end
end
end

function d = chord(t0, t1, h, comp)
% exact displacement along an arc from heading t0 to t1 over length h
if abs(t1 - t0) < 1e-12
    v = [cos(t0) sin(t0)] * h;
else
    k = (t1 - t0) / h;
    v = [(sin(t1) - sin(t0)) / k, -(cos(t1) - cos(t0)) / k];
end
d = v(comp);
end
