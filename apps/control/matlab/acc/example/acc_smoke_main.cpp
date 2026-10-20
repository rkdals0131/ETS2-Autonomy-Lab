// Smoke test / integration example for the generated ACC controller (ets2::AccController).
// Builds against apps/control/matlab/acc/generated/ only (no MATLAB): compile_generated.m
// compiles it with MSVC; on Linux:
//   g++ -std=c++17 -O2 -I../generated acc_smoke_main.cpp ../generated/ACC_Controller.cpp \
//       ../generated/ACC_Controller_data.cpp ../generated/rt*.cpp ../generated/coder_posix_time.c
// It drives the controller with a crude point-mass truck behind a lead that brakes from
// 80 to 40 km/h at -2 m/s^2 and prints the outputs once per second. Call step() every 50 ms.
#include <cstdio>
#include "ACC_Controller.h"

int main()
{
  ets2::AccController acc;
  acc.initialize();

  ets2::AccController::ExtU_ACC_Controller_T in{};
  double v = 80.0 / 3.6, a = 0.0, xe = 0.0;          // ego
  double vl = v, xl = 5.0 + 2.0 * v;                 // lead, rear bumper
  const double dt = 0.05;
  for (int k = 0; k <= 40 * 20; ++k) {
    const double t = k * dt;
    const double al = (t >= 10.0 && vl > 40.0 / 3.6) ? -2.0 : 0.0;
    in.enable = true;
    in.set_speed_mps = 90.0 / 3.6;
    in.time_gap_s = 2.0;
    in.ego_speed_mps = v;
    in.ego_accel_mps2 = a;
    in.lead_valid = true;
    in.lead_gap_m = xl - xe;
    in.lead_speed_mps = vl;
    in.lead_accel_mps2 = al;
    in.resume = false;
    in.engine_rpm = 1200.0;
    in.gear = 12;
    acc.setExternalInputs(&in);
    acc.step();
    const auto &out = acc.getExternalOutputs();
    // crude plant: acceleration follows a_des with a 0.4 s lag
    a += (out.a_des_mps2 - a) * dt / 0.4;
    v = v + a * dt; if (v < 0.0) { v = 0.0; }
    xe += v * dt;
    vl += al * dt; xl += vl * dt;
    if (k % 20 == 0) {
      std::printf("t %5.1f  mode %u  a_des %6.2f  thr %.2f  brk %.3f  ret %d  v %5.1f  vl %5.1f  gap %5.1f\n",
                  t, static_cast<unsigned>(out.mode), out.a_des_mps2, out.throttle, out.brake,
                  static_cast<int>(out.retarder_step), v * 3.6, vl * 3.6, xl - xe);
    }
  }
  ets2::AccController::terminate();
  return 0;
}
