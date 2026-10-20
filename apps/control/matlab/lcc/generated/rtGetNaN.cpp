//
// Academic License - for use in teaching, academic research, and meeting
// course requirements at degree granting institutions only.  Not for
// government, commercial, or other organizational use.
//
// File: rtGetNaN.cpp
//
// Code generated for Simulink model 'LCC_Controller'.
//
// Model version                  : 1.1
// Simulink Coder version         : 26.2 (R2026b) 22-May-2026
// C/C++ source code generated on : Wed Oct 21 00:25:24 2026
//
// Target selection: ert.tlc
// Embedded hardware selection: Intel->x86-64 (Windows64)
// Code generation objectives: Unspecified
// Validation result: Not run
//

extern "C"
{

#include "rtGetNaN.h"

}

#include <stdint.h>

extern "C"
{
  // Return rtNaN needed by the generated code.
  double rtGetNaN(void)
  {
    return rtNaN;
  }

  // Return rtNaNF needed by the generated code.
  float rtGetNaNF(void)
  {
    return rtNaNF;
  }
}

//
// File trailer for generated code.
//
// [EOF]
//
