//
// Academic License - for use in teaching, academic research, and meeting
// course requirements at degree granting institutions only.  Not for
// government, commercial, or other organizational use.
//
// File: rt_nonfinite.h
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
#ifndef RT_NONFINITE_H_
#define RT_NONFINITE_H_
#include <stdbool.h>
#ifdef __cplusplus

extern "C"
{

#endif

  extern double rtInf;
  extern double rtMinusInf;
  extern double rtNaN;
  extern float rtInfF;
  extern float rtMinusInfF;
  extern float rtNaNF;
  extern bool rtIsInf(double value);
  extern bool rtIsInfF(float value);
  extern bool rtIsNaN(double value);
  extern bool rtIsNaNF(float value);

#ifdef __cplusplus

}                                      // extern "C"

#endif
#endif                                 // RT_NONFINITE_H_

//
// File trailer for generated code.
//
// [EOF]
//
