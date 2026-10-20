function [theta, kappa] = lcc_lane_lookup(tab, s)
%LCC_LANE_LOOKUP Lane heading and curvature at arc length s from the uniform lcc_lane table.
%   theta is interpolated linearly; kappa is the slope of that piecewise-linear heading in the
%   cell (not the interpolated kappa column), so integrating kappa along s reproduces theta
%   exactly (the lane-frame errors and the world pose of the plant stay consistent). Clamped
%   to the table ends. Code-generation friendly.
%#codegen
n = numel(tab.theta);
x = (s - tab.s0) / tab.ds;
x = min(max(x, 0), n - 1.000001);
i = floor(x);
f = x - i;
theta = tab.theta(i + 1) * (1 - f) + tab.theta(i + 2) * f;
kappa = (tab.theta(i + 2) - tab.theta(i + 1)) / tab.ds;
end
