function [wheel_angle, wheel_rate, curvature] = lcc_baseline(ey, epsi, v, wheelbase, preview, kappa_back, B)
%LCC_BASELINE Port of LateralController::update (lateral_controller.hpp), the baseline.
%   curvature = path_curvature(s) - 2*ey/d^2 - 2*sin(epsi)/d, d = lookahead_m + lookahead_time_s*v,
%   wheel_angle = atan(wheelbase*curvature), wheel_rate = wheelbase*curvature_rate/(1 + k^2) with
%   curvature_rate = v*(curvature(s + w/2) - curvature(s - w/2))/w (curvature_preview_s = 0).
%   preview(i+1) = LanePath::curvature(s + i, w) (1 m spacing, w = 6 m window), kappa_back =
%   curvature(s - w/2). B: lookahead_m (6), lookahead_time_s (0.7), window (6).
%#codegen
vs = max(0, v);
d = B.lookahead_m + vs * B.lookahead_time_s;
path_curvature = preview(1);
i = round(B.window / 2) + 1;                 % s + w/2 (1 m spacing)
curvature_rate = vs * (preview(i) - kappa_back) / B.window;
curvature = path_curvature - 2 * ey / d^2 - 2 * sin(epsi) / d;
k = wheelbase * curvature;
wheel_angle = atan(k);
wheel_rate = wheelbase * curvature_rate / (1 + k^2);
end
