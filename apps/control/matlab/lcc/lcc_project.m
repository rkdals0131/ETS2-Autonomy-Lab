function [e, sp] = lcc_project(lane, px, py, s_guess)
%LCC_PROJECT Signed lateral offset (left +) of points from the lane polyline of lcc_lane.
%   [e, sp] = lcc_project(lane, px, py, s_guess) searches the 0.25 m polyline segments within
%   +-20 m of s_guess (arc length estimate of each point) and returns the offset to the nearest
%   segment (LanePath::project) and the arc length of the projection.
n = numel(px);
e = zeros(n, 1); sp = zeros(n, 1);
x = lane.x; y = lane.y; s = lane.s;
for k = 1:n
    i0 = max(1, find(s >= s_guess(k) - 20, 1));
    i1 = min(numel(s) - 1, find(s <= s_guess(k) + 20, 1, "last"));
    i = (i0:i1).';
    ax = x(i); ay = y(i); dx = x(i + 1) - ax; dy = y(i + 1) - ay;
    t = ((px(k) - ax) .* dx + (py(k) - ay) .* dy) ./ (dx.^2 + dy.^2);
    t = min(max(t, 0), 1);
    d = hypot(px(k) - ax - t .* dx, py(k) - ay - t .* dy);
    [dm, j] = min(d);
    e(k) = sign(dx(j) * (py(k) - ay(j)) - dy(j) * (px(k) - ax(j))) * dm;
    sp(k) = s(i(j)) + t(j) * 0.25;
end
end
