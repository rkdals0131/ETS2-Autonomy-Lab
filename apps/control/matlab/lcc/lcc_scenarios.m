function S = lcc_scenarios()
%LCC_SCENARIOS The 17 lane-keeping scenarios of apps/sysid/lane_test.py (SCENARIOS, same order).
%   S(i).name      short id;  S(i).title  lane_test.py name;  S(i).index  lane_test index (0-16)
%   S(i).kmh       target speed;  S(i).segments  [length curvature] after the 60 m lead-in
%   S(i).shift     lane start left of the truck (m);  S(i).heading_deg  lane heading - truck heading
%   Curvature is positive to the left; lane_test.py mirrors a lane to fit the test pad, so its
%   game runs mix left and right curves (compare inward/outward, not signs).
S = struct("name", {}, "title", {}, "index", {}, "kmh", {}, "segments", {}, "shift", {}, "heading_deg", {});
S = add(S, "straight_1m_30", "straight, lane 1 m left, 30 km/h", 30, [500 0], 1.0);
S = add(S, "straight_1m_80", "straight, lane 1 m left, 80 km/h", 80, [1400 0], 1.0);
S = add(S, "R400_60", "arc R400 180 deg, 60 km/h", 60, [400*pi 1/400; 150 0], 0);
S = add(S, "R400_80", "arc R400 180 deg, 80 km/h", 80, [400*pi 1/400; 150 0], 0);
S = add(S, "R200_50", "arc R200 180 deg, 50 km/h", 50, [200*pi 1/200; 120 0], 0);
s3 = [300*pi/2 1/300; 300*pi/2 -1/300; 300*pi/2 1/300; 150 0];
S = add(S, "S300_60", "S-curve R300 3x90 deg, 60 km/h", 60, s3, 0);
S = add(S, "S300_80", "S-curve R300 3x90 deg, 80 km/h", 80, s3, 0);
S = add(S, "shift120_80", "3.5 m lane shift over 120 m, 80 km/h", 80, [300 0; lane_shift(3.5, 120); 400 0], 0);
S = add(S, "straight_1m_100", "straight, lane 1 m left, 100 km/h", 100, [1800 0], 1.0);
S = add(S, "R700_90", "arc R700 120 deg, 90 km/h", 90, [700*2*pi/3 1/700; 200 0], 0);
S = add(S, "R700_100", "arc R700 120 deg, 100 km/h", 100, [700*2*pi/3 1/700; 200 0], 0);
S = add(S, "S700_100", "S-curve R700 2x60 deg, 100 km/h", 100, [700*pi/3 1/700; 700*pi/3 -1/700; 200 0], 0);
S = add(S, "shift200_100", "3.5 m lane shift over 200 m, 100 km/h", 100, [300 0; lane_shift(3.5, 200); 500 0], 0);
S = add(S, "offset3m_60", "lane 3 m right of truck, 60 km/h", 60, [700 0], -3.0);
S = add(S, "heading8_60", "lane heading +8 deg from truck, 60 km/h", 60, [700 0], 0, 8);
S = add(S, "R40_15", "arc R40 90 deg, 15 km/h", 15, [30 0; 40*pi/2 1/40; 80 0], 0);
S = add(S, "R60_25", "arc R60 180 deg, 25 km/h", 25, [40 0; 60*pi 1/60; 80 0], 0);
end

function S = add(S, name, title, kmh, segments, shift, heading_deg)
if nargin < 7, heading_deg = 0; end
S(end+1) = struct("name", name, "title", title, "index", numel(S), "kmh", kmh, "segments", segments, ...
    "shift", shift, "heading_deg", heading_deg);
end

function seg = lane_shift(width, length)
% lane_test.py lane_shift: two opposite arcs shifting the lane by width over length
a = 2 * atan(width / length);
R = length / (2 * sin(a));
seg = [R*a 1/R; R*a -1/R];
end
