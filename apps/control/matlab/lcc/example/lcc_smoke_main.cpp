// Smoke test / integration example for the generated lane-centering MPC (ets2::LccController).
// Builds against apps/control/matlab/lcc/generated/ only (no MATLAB): compile_generated.m
// compiles it with MSVC; on Linux:
//   g++ -std=c++17 -O2 -I../generated lcc_smoke_main.cpp ../generated/LCC_Controller.cpp \
//       ../generated/LCC_Controller_data.cpp ../generated/rt*.cpp ../generated/coder_posix_time.c
// A crude kinematic truck (wheelbase 3.158 m, wheel angle following the command with a 0.06 s
// lag) starts 0.5 m right of a lane that turns into an R400 arc after 100 m, at 80 km/h with the
// trailer flag set. It prints the state every 5 s and the step time statistics at the
// end. Call step() every 20 ms. The controller object holds ~1.2 MB of solver work arrays, so
// it is allocated on the heap.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>
#include "LCC_Controller.h"

namespace {
double lane_curvature(double s) { return s > 100.0 ? 1.0 / 400.0 : 0.0; }
// mean curvature over [s - 3, s + 3] (the node's LanePath::curvature(s, 6))
double window_curvature(double s)
{
  const double a = s - 3.0, b = s + 3.0;
  const double len = std::fmax(0.0, b - std::fmax(a, 100.0));
  return len / 6.0 / 400.0;
}
}  // namespace

int main()
{
  auto lcc = std::make_unique<ets2::LccController>();
  lcc->initialize();
  ets2::LccController::ExtU_LCC_Controller_T in{};
  const double dt = 0.02, v = 80.0 / 3.6, L = 3.158;
  double s = 0.0, ey = -0.5, epsi = 0.0, delta = 0.0, hitch = 0.0;
  std::vector<double> times;
  for (int k = 0; k <= 60 * 50; ++k) {
    in.enable = true;
    in.speed_mps = v;
    in.lateral_error_m = ey;
    in.heading_error_rad = epsi;
    for (int i = 0; i < 61; ++i) {
      in.curvature_preview[i] = window_curvature(s + i);
    }
    in.wheel_angle_rad = delta;
    in.wheelbase_m = L;
    in.trailer_connected = true;
    in.hitch_angle_rad = hitch;
    lcc->setExternalInputs(&in);
    const auto t0 = std::chrono::steady_clock::now();
    lcc->step();
    const auto t1 = std::chrono::steady_clock::now();
    times.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
    const auto &out = lcc->getExternalOutputs();
    // crude plant, 10 sub-steps per tick
    for (int j = 0; j < 10; ++j) {
      const double h = dt / 10.0;
      delta += (out.wheel_angle_cmd_rad - delta) * h / 0.06;
      const double r = v * std::tan(delta) / L, kap = lane_curvature(s);
      const double sdot = v * std::cos(epsi) / (1.0 - kap * ey);
      ey += v * std::sin(epsi) * h;
      epsi += (r - kap * sdot) * h;
      hitch += (r - (v * std::sin(hitch) - 0.478 * r * std::cos(hitch)) / 7.851) * h;
      s += sdot * h;
    }
    if (k % 250 == 0) {
      std::printf("t %5.1f  s %6.1f  ey %+8.4f m  epsi %+7.4f  delta %+7.4f  rate %+7.4f  status %u\n",
                  k * dt, s, ey, epsi, out.wheel_angle_cmd_rad, out.wheel_rate_cmd_rad_s,
                  static_cast<unsigned>(out.status));
    }
  }
  double sum = 0.0, mx = 0.0;
  for (double t : times) { sum += t; mx = std::fmax(mx, t); }
  std::vector<double> sorted = times;
  std::sort(sorted.begin(), sorted.end());
  const double p99 = sorted[static_cast<std::size_t>(0.99 * (sorted.size() - 1))];
  std::printf("step time: mean %.1f us, p99 %.1f us, max %.1f us over %zu steps (period 20000 us)\n",
              sum / times.size(), p99, mx, times.size());
  ets2::LccController::terminate();
  return 0;
}
