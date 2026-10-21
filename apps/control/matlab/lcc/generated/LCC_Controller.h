//
// Academic License - for use in teaching, academic research, and meeting
// course requirements at degree granting institutions only.  Not for
// government, commercial, or other organizational use.
//
// File: LCC_Controller.h
//
// Code generated for Simulink model 'LCC_Controller'.
//
// Model version                  : 1.1
// Simulink Coder version         : 26.2 (R2026b) 22-May-2026
// C/C++ source code generated on : Wed Oct 21 17:53:43 2026
//
// Target selection: ert.tlc
// Embedded hardware selection: Intel->x86-64 (Windows64)
// Code generation objectives: Unspecified
// Validation result: Not run
//
#ifndef LCC_CONTROLLER_H_
#define LCC_CONTROLLER_H_
#include <stdbool.h>
#include <stdint.h>
#include <limits>
#include <cmath>
#include "complex_types.h"
#include "rt_nonfinite.h"
#include "LCC_Controller_types.h"

extern "C"
{

#include "rtGetInf.h"

}

extern "C"
{

#include "rtGetNaN.h"

}

// Class declaration for model LCC_Controller
namespace ets2
{
  class LccController final
  {
    // public data and function members
   public:
    // Block signals and states (default storage) for system '<Root>'
    struct DW_LCC_Controller_T {
      double A[49];                    // '<S1>/MPC_Prep'
      double B[14];                    // '<S1>/MPC_Prep'
      double Cm[42];                   // '<S1>/MPC_Prep'
      double Dm[12];                   // '<S1>/MPC_Prep'
      double memory_DSTATE[8];         // '<S1>/memory'
      double Bu[532];
      double Bv[1064];
      double Dv[912];
      double r[2212];
      double t[48032];
      double v[2844];
      double c_SuJm[3600];
      double c_Sx[3150];
      double c_Hv[68400];
      double c_Kv[1216];
      double Su[33750];
      double I2Jm[600];
      double WySuJm[3600];
      double WuI2Jm[600];
      double y[600];
      double CA[912];
      double freq;                     // '<S33>/FixedHorizonOptimizer'
      bool Memory_PreviousInput[316];  // '<S5>/Memory'
      bool freq_not_empty;             // '<S33>/FixedHorizonOptimizer'
    };

    // Invariant block signals (default storage)
    struct ConstB_LCC_Controller_T {
      double MathFunction[6];          // '<S5>/Math Function'
      double MathFunction1;            // '<S5>/Math Function1'
      double MathFunction2;            // '<S5>/Math Function2'
      double Reshape3[6];              // '<S5>/Reshape3'
      double Reshape4;                 // '<S5>/Reshape4'
      double Reshape5;                 // '<S5>/Reshape5'
    };

    // External inputs (root inport signals with default storage)
    struct ExtU_LCC_Controller_T {
      bool enable;                     // '<Root>/enable'
      double speed_mps;                // '<Root>/speed_mps'
      double lateral_error_m;          // '<Root>/lateral_error_m'
      double heading_error_rad;        // '<Root>/heading_error_rad'
      double curvature_preview[61];    // '<Root>/curvature_preview'
      double wheel_angle_rad;          // '<Root>/wheel_angle_rad'
      double wheelbase_m;              // '<Root>/wheelbase_m'
      bool trailer_connected;          // '<Root>/trailer_connected'
      double hitch_angle_rad;          // '<Root>/hitch_angle_rad'
    };

    // External outputs (root outports fed by signals with default storage)
    struct ExtY_LCC_Controller_T {
      double wheel_angle_cmd_rad;      // '<Root>/wheel_angle_cmd_rad'
      double wheel_rate_cmd_rad_s;     // '<Root>/wheel_rate_cmd_rad_s'
      uint8_t status;                  // '<Root>/status'
    };

    // Real-time Model Data Structure
    struct RT_MODEL_LCC_Controller_T {
      const char * volatile errorStatus;
      const char* getErrorStatus() const;
      void setErrorStatus(const char* const volatile aErrorStatus);
    };

    // Copy Constructor
    LccController(LccController const&) = delete;

    // Assignment Operator
    LccController& operator= (LccController const&) & = delete;

    // Move Constructor
    LccController(LccController &&) = delete;

    // Move Assignment Operator
    LccController& operator= (LccController &&) = delete;

    // Real-Time Model get method
    LccController::RT_MODEL_LCC_Controller_T * getRTM();

    // Root inports set method
    void setExternalInputs(const ExtU_LCC_Controller_T *pExtU_LCC_Controller_T)
    {
      LCC_Controller_U = *pExtU_LCC_Controller_T;
    }

    // Root outports get method
    const ExtY_LCC_Controller_T &getExternalOutputs() const
    {
      return LCC_Controller_Y;
    }

    // model initialize function
    static void initialize();

    // model step function
    void step();

    // model terminate function
    static void terminate();

    // Constructor
    LccController();

    // Destructor
    ~LccController();

    // private data and function members
   private:
    // External inputs
    ExtU_LCC_Controller_T LCC_Controller_U;

    // External outputs
    ExtY_LCC_Controller_T LCC_Controller_Y;

    // Block states
    DW_LCC_Controller_T LCC_Controller_DW;

    // private member function(s) for subsystem '<Root>'
    double LCC_Controller_norm_a(const double x[25]);
    void LCC_Controller_xzgetrf(double A[25], int32_t ipiv[5], int32_t *info);
    void LCC_Controller_xtrsm(const double A[25], double B[25]);
    void LCC_Controller_inv(const double x[25], double y[25]);
    void LCC_Controller_mpower(const double a[25], double b, double c[25]);
    double LCC_Controller_log2(double x);
    void LCC_Controlle_padeApproximation(const double A[25], const double A2[25],
      const double A4[25], const double A6[25], int32_t m, double F[25]);
    void LCC_Controll_recomputeBlockDiag(const double A[25], double F[25], const
      int32_t blockFormat[4]);
    double LCC_Controller_xnrm2_e(int32_t n, const double x[25], int32_t ix0);
    void LCC_Controller_xzsyhetrd(double A[25], double D[5], double E[4], double
      tau[4]);
    void LCC_Controller_xzlascl(double cfrom, double cto, int32_t m, int32_t n,
      double A[5], int32_t iA0, int32_t lda);
    void LCC_Controller_xzlascl_o(double cfrom, double cto, int32_t m, int32_t n,
      double A[4], int32_t iA0, int32_t lda);
    void LCC_Controller_xzlartg(double f, double g, double *cs, double *sn,
      double *r);
    void LCC_Controller_rotateRight_j(int32_t m, int32_t n, double z[25],
      int32_t iz0, int32_t ldz, const double cs[8], int32_t ic0, int32_t is0);
    void LCC_Controller_xdlaev2(double a, double b, double c, double *rt1,
      double *rt2, double *cs1, double *sn1);
    void LCC_Controller_rotateRight(int32_t m, int32_t n, double z[25], int32_t
      iz0, int32_t ldz, const double cs[8], int32_t ic0, int32_t is0);
    int32_t LCC_Controller_xzsteqr(double d[5], double e[4], double z[25]);
    void LCC_Controller_xsyheev(double A[25], int32_t *info, double W[5]);
    void LCC_Controller_expm(double A[25], double F[25]);
    void LCC_Controller_lcc_mpc_model(double v, double wheelbase, bool trailer,
      double Ad[49], double Bd[14], double Cm[42], double Dm[12], double
      *steady_slip, double rack[2]);
    int32_t LCC_Controller_xpotrf(double b_A[81]);
    double LCC_Controller_minimum(const double x[9]);
    void LCC_Controller_trisolve(const double b_A[81], double b_B[81]);
    void LCC_Controller_tic(double *tstart_tv_sec, double *tstart_tv_nsec);
    double LCC_Controller_norm(const double x[9]);
    double LCC_Controller_toc(double tstart_tv_sec, double tstart_tv_nsec);
    double LCC_Controller_maximum(const double x[9]);
    double LCC_Controller_xnrm2(int32_t n, const double x[81], int32_t ix0);
    void LCC_Controller_xgemv(int32_t b_m, int32_t n, const double b_A[81],
      int32_t ia0, const double x[81], int32_t ix0, double y[9]);
    void LCC_Controller_xgerc(int32_t b_m, int32_t n, double alpha1, int32_t ix0,
      const double y[9], double b_A[81], int32_t ia0);
    double LCC_Controller_KWIKfactor(const double b_Ac[2844], const int32_t iC
      [316], int32_t nA, const double b_Linv[81], double RLinv[81], double b_D
      [81], double b_H[81], int32_t n);
    void LCC_Controller_DropConstraint(int32_t kDrop, bool iA[316], int32_t *nA,
      int32_t iC[316]);
    void LCC_Controller_qpkwik(const double b_Linv[81], const double b_Hinv[81],
      const double f[9], const double b_Ac[2844], const double b[316], bool iA
      [316], int32_t maxiter, double FeasTol, double x[9], double lambda[316],
      int32_t *status);
    double LCC_Controll_enforceInputLimits(double uk, double uk1, double b_uoff,
      const double b_Mlim[316], const int32_t b_Mrows[316], int32_t b_ny,
      int32_t b_degrees);
    void LCC_Controll_mpcblock_optimizer(const double rseq[450], const double
      vseq[152], double umin, double umax, const double x[7], double old_u,
      const bool iA[316], const double b_Mlim[316], double b_Mx[2212], double
      b_Mu1[316], double b_Mv[48032], const double b_utarget[75], double b_uoff,
      double b_Ac[2844], const double b_A[49], const double Bu[532], const
      double Bv[1064], const double b_C[42], const double Dv[912], double *u,
      double useq[76], double *status, bool iAout[316]);

    // Real-Time Model
    RT_MODEL_LCC_Controller_T LCC_Controller_M;
  };
}

extern const ets2::LccController::ConstB_LCC_Controller_T LCC_Controller_ConstB;// constant block i/o 

//-
//  These blocks were eliminated from the model due to optimizations:
//
//  Block '<S5>/Constant' : Unused code path elimination
//  Block '<S5>/Floor' : Unused code path elimination
//  Block '<S5>/Floor1' : Unused code path elimination
//  Block '<S6>/Matrix Dimension Check' : Unused code path elimination
//  Block '<S7>/Matrix Dimension Check' : Unused code path elimination
//  Block '<S8>/Matrix Dimension Check' : Unused code path elimination
//  Block '<S9>/Matrix Dimension Check' : Unused code path elimination
//  Block '<S10>/Matrix Dimension Check' : Unused code path elimination
//  Block '<S11>/Matrix Dimension Check' : Unused code path elimination
//  Block '<S12>/Matrix Dimension Check' : Unused code path elimination
//  Block '<S13>/Matrix Dimension Check' : Unused code path elimination
//  Block '<S14>/Matrix Dimension Check' : Unused code path elimination
//  Block '<S15>/Matrix Dimension Check' : Unused code path elimination
//  Block '<S16>/Matrix Dimension Check' : Unused code path elimination
//  Block '<S17>/Matrix Dimension Check' : Unused code path elimination
//  Block '<S18>/Matrix Dimension Check' : Unused code path elimination
//  Block '<S19>/Matrix Dimension Check' : Unused code path elimination
//  Block '<S20>/Matrix Dimension Check' : Unused code path elimination
//  Block '<S21>/Matrix Dimension Check' : Unused code path elimination
//  Block '<S22>/Matrix Dimension Check' : Unused code path elimination
//  Block '<S23>/Matrix Dimension Check' : Unused code path elimination
//  Block '<S24>/Matrix Dimension Check' : Unused code path elimination
//  Block '<S25>/Matrix Dimension Check' : Unused code path elimination
//  Block '<S26>/Vector Dimension Check' : Unused code path elimination
//  Block '<S27>/Vector Dimension Check' : Unused code path elimination
//  Block '<S28>/Vector Dimension Check' : Unused code path elimination
//  Block '<S29>/Vector Dimension Check' : Unused code path elimination
//  Block '<S30>/Vector Dimension Check' : Unused code path elimination
//  Block '<S31>/Vector Dimension Check' : Unused code path elimination
//  Block '<S5>/Min' : Unused code path elimination
//  Block '<S5>/last_x' : Unused code path elimination
//  Block '<S32>/Vector Dimension Check' : Unused code path elimination
//  Block '<S5>/useq_scale' : Unused code path elimination
//  Block '<S5>/useq_scale1' : Unused code path elimination
//  Block '<S2>/m_zero' : Unused code path elimination
//  Block '<S2>/p_zero' : Unused code path elimination


//-
//  The generated code includes comments that allow you to trace directly
//  back to the appropriate location in the model.  The basic format
//  is <system>/block_name, where system is the system number (uniquely
//  assigned by Simulink) and block_name is the name of the block.
//
//  Use the MATLAB hilite_system command to trace the generated code back
//  to the model.  For example,
//
//  hilite_system('<S3>')    - opens system 3
//  hilite_system('<S3>/Kp') - opens and selects block Kp which resides in S3
//
//  Here is the system hierarchy for this model
//
//  '<Root>' : 'LCC_Controller'
//  '<S1>'   : 'LCC_Controller/LCC_Controller'
//  '<S2>'   : 'LCC_Controller/LCC_Controller/MPC'
//  '<S3>'   : 'LCC_Controller/LCC_Controller/MPC_Post'
//  '<S4>'   : 'LCC_Controller/LCC_Controller/MPC_Prep'
//  '<S5>'   : 'LCC_Controller/LCC_Controller/MPC/MPC'
//  '<S6>'   : 'LCC_Controller/LCC_Controller/MPC/MPC/MPC Matrix Signal Check'
//  '<S7>'   : 'LCC_Controller/LCC_Controller/MPC/MPC/MPC Matrix Signal Check A'
//  '<S8>'   : 'LCC_Controller/LCC_Controller/MPC/MPC/MPC Matrix Signal Check B'
//  '<S9>'   : 'LCC_Controller/LCC_Controller/MPC/MPC/MPC Matrix Signal Check C'
//  '<S10>'  : 'LCC_Controller/LCC_Controller/MPC/MPC/MPC Matrix Signal Check D'
//  '<S11>'  : 'LCC_Controller/LCC_Controller/MPC/MPC/MPC Matrix Signal Check DX'
//  '<S12>'  : 'LCC_Controller/LCC_Controller/MPC/MPC/MPC Matrix Signal Check U'
//  '<S13>'  : 'LCC_Controller/LCC_Controller/MPC/MPC/MPC Matrix Signal Check X'
//  '<S14>'  : 'LCC_Controller/LCC_Controller/MPC/MPC/MPC Matrix Signal Check Y'
//  '<S15>'  : 'LCC_Controller/LCC_Controller/MPC/MPC/MPC Matrix Signal Check1'
//  '<S16>'  : 'LCC_Controller/LCC_Controller/MPC/MPC/MPC Matrix Signal Check2'
//  '<S17>'  : 'LCC_Controller/LCC_Controller/MPC/MPC/MPC Preview Signal Check'
//  '<S18>'  : 'LCC_Controller/LCC_Controller/MPC/MPC/MPC Preview Signal Check1'
//  '<S19>'  : 'LCC_Controller/LCC_Controller/MPC/MPC/MPC Preview Signal Check2'
//  '<S20>'  : 'LCC_Controller/LCC_Controller/MPC/MPC/MPC Preview Signal Check3'
//  '<S21>'  : 'LCC_Controller/LCC_Controller/MPC/MPC/MPC Preview Signal Check4'
//  '<S22>'  : 'LCC_Controller/LCC_Controller/MPC/MPC/MPC Preview Signal Check5'
//  '<S23>'  : 'LCC_Controller/LCC_Controller/MPC/MPC/MPC Preview Signal Check6'
//  '<S24>'  : 'LCC_Controller/LCC_Controller/MPC/MPC/MPC Preview Signal Check7'
//  '<S25>'  : 'LCC_Controller/LCC_Controller/MPC/MPC/MPC Preview Signal Check8'
//  '<S26>'  : 'LCC_Controller/LCC_Controller/MPC/MPC/MPC Scalar Signal Check'
//  '<S27>'  : 'LCC_Controller/LCC_Controller/MPC/MPC/MPC Scalar Signal Check1'
//  '<S28>'  : 'LCC_Controller/LCC_Controller/MPC/MPC/MPC Scalar Signal Check2'
//  '<S29>'  : 'LCC_Controller/LCC_Controller/MPC/MPC/MPC Vector Signal Check'
//  '<S30>'  : 'LCC_Controller/LCC_Controller/MPC/MPC/MPC Vector Signal Check1'
//  '<S31>'  : 'LCC_Controller/LCC_Controller/MPC/MPC/MPC Vector Signal Check6'
//  '<S32>'  : 'LCC_Controller/LCC_Controller/MPC/MPC/moorx'
//  '<S33>'  : 'LCC_Controller/LCC_Controller/MPC/MPC/optimizer'
//  '<S34>'  : 'LCC_Controller/LCC_Controller/MPC/MPC/optimizer/FixedHorizonOptimizer'

#endif                                 // LCC_CONTROLLER_H_

//
// File trailer for generated code.
//
// [EOF]
//
