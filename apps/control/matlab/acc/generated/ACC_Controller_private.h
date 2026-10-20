//
// Academic License - for use in teaching, academic research, and meeting
// course requirements at degree granting institutions only.  Not for
// government, commercial, or other organizational use.
//
// File: ACC_Controller_private.h
//
// Code generated for Simulink model 'ACC_Controller'.
//
// Model version                  : 1.1
// Simulink Coder version         : 26.2 (R2026b) 22-May-2026
// C/C++ source code generated on : Tue Oct 20 21:31:27 2026
//
// Target selection: ert.tlc
// Embedded hardware selection: Intel->x86-64 (Windows64)
// Code generation objectives: Unspecified
// Validation result: Not run
//
#ifndef ACC_CONTROLLER_PRIVATE_H_
#define ACC_CONTROLLER_PRIVATE_H_
#include <stdbool.h>
#include <stdint.h>
#include "complex_types.h"
#include "ACC_Controller_types.h"
#include "ACC_Controller.h"

extern double rt_powd_snf(double u0, double u1);
extern double rt_hypotd_snf(double u0, double u1);
extern int32_t div_nde_s32_floor(int32_t numerator, int32_t denominator);

#endif                                 // ACC_CONTROLLER_PRIVATE_H_

//
// File trailer for generated code.
//
// [EOF]
//
