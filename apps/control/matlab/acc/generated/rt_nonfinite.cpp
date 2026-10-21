//
// Academic License - for use in teaching, academic research, and meeting
// course requirements at degree granting institutions only.  Not for
// government, commercial, or other organizational use.
//
// File: rt_nonfinite.cpp
//
// Code generated for Simulink model 'ACC_Controller'.
//
// Model version                  : 1.1
// Simulink Coder version         : 26.2 (R2026b) 22-May-2026
// C/C++ source code generated on : Wed Oct 21 18:43:29 2026
//
// Target selection: ert.tlc
// Embedded hardware selection: Intel->x86-64 (Windows64)
// Code generation objectives: Unspecified
// Validation result: Not run
//

#include <stdbool.h>

extern "C"
{

#include "rt_nonfinite.h"

}

#include <limits>
#include <cmath>

extern "C"
{
  double rtNaN { -std::numeric_limits<double>::quiet_NaN() };

  double rtInf { std::numeric_limits<double>::infinity() };

  double rtMinusInf { -std::numeric_limits<double>::infinity() };

  float rtNaNF { -std::numeric_limits<float>::quiet_NaN() };

  float rtInfF { std::numeric_limits<float>::infinity() };

  float rtMinusInfF { -std::numeric_limits<float>::infinity() };
}

extern "C"
{
  // Test if value is infinite
  bool rtIsInf(double value)
  {
    return std::isinf(value);
  }

  // Test if single-precision value is infinite
  bool rtIsInfF(float value)
  {
    return std::isinf(value);
  }

  // Test if value is not a number
  bool rtIsNaN(double value)
  {
    return std::isnan(value);
  }

  // Test if single-precision value is not a number
  bool rtIsNaNF(float value)
  {
    return std::isnan(value);
  }
}

//
// File trailer for generated code.
//
// [EOF]
//
