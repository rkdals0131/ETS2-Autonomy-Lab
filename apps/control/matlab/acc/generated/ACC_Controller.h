//
// Academic License - for use in teaching, academic research, and meeting
// course requirements at degree granting institutions only.  Not for
// government, commercial, or other organizational use.
//
// File: ACC_Controller.h
//
// Code generated for Simulink model 'ACC_Controller'.
//
// Model version                  : 1.1
// Simulink Coder version         : 26.2 (R2026b) 22-May-2026
// C/C++ source code generated on : Wed Oct 21 02:24:25 2026
//
// Target selection: ert.tlc
// Embedded hardware selection: Intel->x86-64 (Windows64)
// Code generation objectives: Unspecified
// Validation result: Not run
//
#ifndef ACC_CONTROLLER_H_
#define ACC_CONTROLLER_H_
#include <stdbool.h>
#include <stdint.h>
#include <limits>
#include <cmath>
#include "complex_types.h"
#include "rt_nonfinite.h"
#include "ACC_Controller_types.h"

extern "C"
{

#include "rtGetInf.h"

}

extern "C"
{

#include "rtGetNaN.h"

}

// Class declaration for model ACC_Controller
namespace ets2
{
  class AccController final
  {
    // public data and function members
   public:
    // Block signals and states (default storage) for system '<Root>'
    struct DW_ACC_Controller_T {
      double useq[162];                // '<S53>/optimizer'
      double useq_b[162];              // '<S31>/optimizer'
      double mv_last_DSTATE[2];        // '<S1>/mv_last'
      double uk1_DSTATE[2];            // '<S4>/u[k-1]'
      double yb[21];                   // '<S1>/MassEstimator'
      double pb[21];                   // '<S1>/MassEstimator'
      double c_Kv[1944];
      double I2Jm[1920];
      double WySuJm[2880];
      double WuI2Jm[1920];
      double c_Kv_m[1944];
      double I2Jm_c[1920];
      double WySuJm_k[2880];
      double WuI2Jm_c[1920];
      double y[1920];
      double th;                       // '<S1>/InputConditioning'
      double v;                        // '<S1>/InputConditioning'
      double vl;                       // '<S1>/InputConditioning'
      double ret_last_DSTATE;          // '<S1>/ret_last'
      double t_hold;                   // '<S1>/ModeLogic'
      double gap_stop;                 // '<S1>/ModeLogic'
      double theta;                    // '<S1>/MassEstimator'
      double P;                        // '<S1>/MassEstimator'
      double s2;                       // '<S1>/MassEstimator'
      double t_info;                   // '<S1>/MassEstimator'
      double m_prev_prior;             // '<S1>/MassEstimator'
      double Te_m;                     // '<S1>/MassEstimator'
      double F_lag;                    // '<S1>/MassEstimator'
      double yf;                       // '<S1>/MassEstimator'
      double phif;                     // '<S1>/MassEstimator'
      double w;                        // '<S1>/MassEstimator'
      double cnt;                      // '<S1>/MassEstimator'
      double gear_last;                // '<S1>/MassEstimator'
      double m_out;                    // '<S1>/MassEstimator'
      double zbar;                     // '<S1>/MassEstimator'
      double n_open;                   // '<S1>/MassEstimator'
      double kb_tot;                   // '<S1>/MassEstimator'
      double kb_n;                     // '<S1>/MassEstimator'
      double bt;                       // '<S1>/MassEstimator'
      double pred_f;                   // '<S1>/MassEstimator'
      double meas_f;                   // '<S1>/MassEstimator'
      double a2;                       // '<S1>/MPC_Prep'
      double vs_f;                     // '<S1>/MPC_Prep'
      double freq;                     // '<S53>/optimizer'
      double freq_o;                   // '<S31>/optimizer'
      double af;                       // '<S1>/InputConditioning'
      double s;                        // '<S1>/Allocator'
      double ret_t;                    // '<S1>/Allocator'
      double ret_dir;                  // '<S1>/Allocator'
      double integ;                    // '<S1>/Allocator'
      double r5_t;                     // '<S1>/Allocator'
      double r5_on_t;                  // '<S1>/Allocator'
      double last_gear;                // '<S1>/Allocator'
      double gear_t;                   // '<S1>/Allocator'
      double stop_i;                   // '<S1>/Allocator'
      uint32_t durationCounter_1;      // '<S1>/ModeLogic'
      uint32_t durationCounter_1_j;    // '<S1>/ModeLogic'
      uint32_t durationCounter_1_g;    // '<S1>/ModeLogic'
      uint32_t durationCounter_2;      // '<S1>/ModeLogic'
      int8_t a[25600];
      int8_t a_b[25600];
      uint8_t is_active_c3_ACC_Controller;// '<S1>/ModeLogic'
      uint8_t is_c3_ACC_Controller;    // '<S1>/ModeLogic'
      uint8_t is_ON;                   // '<S1>/ModeLogic'
      bool Memory_PreviousInput[208];  // '<S33>/Memory'
      bool Memory_PreviousInput_h[208];// '<S11>/Memory'
      bool eb_last_DSTATE;             // '<S1>/eb_last'
      bool icLoad;                     // '<S4>/u[k-1]'
      bool init;                       // '<S1>/MassEstimator'
      bool freq_not_empty;             // '<S53>/optimizer'
      bool freq_not_empty_k;           // '<S31>/optimizer'
      bool brk_on;                     // '<S1>/Allocator'
      bool r5;                         // '<S1>/Allocator'
      bool drv;                        // '<S1>/Allocator'
    };

    // Invariant block signals (default storage)
    struct ConstB_ACC_Controller_T {
      double MathFunction1[2];         // '<S11>/Math Function1'
      double MathFunction2[2];         // '<S11>/Math Function2'
      double Reshape4[2];              // '<S11>/Reshape4'
      double Reshape5[2];              // '<S11>/Reshape5'
      double MathFunction1_d[2];       // '<S33>/Math Function1'
      double MathFunction2_f[2];       // '<S33>/Math Function2'
      double Reshape4_j[2];            // '<S33>/Reshape4'
      double Reshape5_b[2];            // '<S33>/Reshape5'
    };

    // Constant parameters (default storage)
    struct ConstP_ACC_Controller_T {
      // Pooled Parameter (Expression: MVscale(:,ones(1,p+1))')
      //  Referenced by:
      //    '<S11>/umin_scale3'
      //    '<S33>/umin_scale3'

      double pooled41[162];
    };

    // External inputs (root inport signals with default storage)
    struct ExtU_ACC_Controller_T {
      bool enable;                     // '<Root>/enable'
      double set_speed_mps;            // '<Root>/set_speed_mps'
      double time_gap_s;               // '<Root>/time_gap_s'
      double ego_speed_mps;            // '<Root>/ego_speed_mps'
      double ego_accel_mps2;           // '<Root>/ego_accel_mps2'
      bool lead_valid;                 // '<Root>/lead_valid'
      double lead_gap_m;               // '<Root>/lead_gap_m'
      double lead_speed_mps;           // '<Root>/lead_speed_mps'
      double lead_accel_mps2;          // '<Root>/lead_accel_mps2'
      bool resume;                     // '<Root>/resume'
      double engine_rpm;               // '<Root>/engine_rpm'
      int32_t gear;                    // '<Root>/gear'
      double pitch_rad;                // '<Root>/pitch_rad'
      double mass_prior_kg;            // '<Root>/mass_prior_kg'
      bool actuation_valid;            // '<Root>/actuation_valid'
      double throttle_applied;         // '<Root>/throttle_applied'
      double brake_applied;            // '<Root>/brake_applied'
      int32_t retarder_applied;        // '<Root>/retarder_applied'
    };

    // External outputs (root outports fed by signals with default storage)
    struct ExtY_ACC_Controller_T {
      double throttle;                 // '<Root>/throttle'
      double brake;                    // '<Root>/brake'
      int32_t retarder_step;           // '<Root>/retarder_step'
      bool engine_brake;               // '<Root>/engine_brake'
      double a_des_mps2;               // '<Root>/a_des_mps2'
      uint8_t mode;                    // '<Root>/mode'
      double mass_est_kg;              // '<Root>/mass_est_kg'
      double mass_sigma_kg;            // '<Root>/mass_sigma_kg'
      bool mass_valid;                 // '<Root>/mass_valid'
    };

    // Real-time Model Data Structure
    struct RT_MODEL_ACC_Controller_T {
      const char * volatile errorStatus;
      const char* getErrorStatus() const;
      void setErrorStatus(const char* const volatile aErrorStatus);
    };

    // Copy Constructor
    AccController(AccController const&) = delete;

    // Assignment Operator
    AccController& operator= (AccController const&) & = delete;

    // Move Constructor
    AccController(AccController &&) = delete;

    // Move Assignment Operator
    AccController& operator= (AccController &&) = delete;

    // Real-Time Model get method
    AccController::RT_MODEL_ACC_Controller_T * getRTM();

    // Root inports set method
    void setExternalInputs(const ExtU_ACC_Controller_T *pExtU_ACC_Controller_T)
    {
      ACC_Controller_U = *pExtU_ACC_Controller_T;
    }

    // Root outports get method
    const ExtY_ACC_Controller_T &getExternalOutputs() const
    {
      return ACC_Controller_Y;
    }

    // model initialize function
    void initialize();

    // model step function
    void step();

    // model terminate function
    static void terminate();

    // Constructor
    AccController();

    // Destructor
    ~AccController();

    // private data and function members
   private:
    // External inputs
    ExtU_ACC_Controller_T ACC_Controller_U;

    // External outputs
    ExtY_ACC_Controller_T ACC_Controller_Y;

    // Block states
    DW_ACC_Controller_T ACC_Controller_DW;

    // private member function(s) for subsystem '<Root>'
    double ACC_Controller_follow_range(void) const;
    void ACC_Controller_FOLLOW(const double *vset, const double *dreq, const
      bool *lead_ok, const double *gap, const double *ttc);
    void ACC_Controller_GO(const double *vset, const double *dreq, const bool
      *lead_ok, const double *gap, const double *ttc);
    void ACC_Controller_ON(const double *vset, const double *dreq, const bool
      *lead_ok, const double *gap, const double *ttc);
    int32_t ACC_Controller_bsearch(const double x[8], double xi);
    double ACC_Controller_interp1_o(double varargin_3);
    double ACC_Controller_interp2(double varargin_4, double varargin_5);
    double ACC_Controller_interp1_oi(double varargin_3);
    double ACC_Controller_interp1(const double varargin_2[4], double varargin_3);
    double ACC_Controller_interp1_oimb(const double varargin_2[10], double
      varargin_3);
    int32_t ACC_Controller_xpotrf(double b_A[169]);
    double ACC_Controller_minimum(const double x[13]);
    void ACC_Contr_validateHessianMatrix(double b_H[169], double b_L[169],
      double *BadH);
    void ACC_Controller_trisolve(const double b_A[169], double B[169]);
    void ACC_Controller_tic_n(double *tstart_tv_sec, double *tstart_tv_nsec);
    double ACC_Controller_norm(const double x[13]);
    double ACC_Controller_toc_h(double tstart_tv_sec, double tstart_tv_nsec);
    double ACC_Controller_maximum(const double x[13]);
    double ACC_Controller_xnrm2(int32_t n, const double x[169], int32_t ix0);
    void ACC_Controller_xgemv(int32_t b_m, int32_t n, const double b_A[169],
      int32_t ia0, const double x[169], int32_t ix0, double y[13]);
    void ACC_Controller_xgerc(int32_t b_m, int32_t n, double alpha1, int32_t ix0,
      const double y[13], double b_A[169], int32_t ia0);
    double ACC_Controller_KWIKfactor(const double b_Ac[2704], const int32_t iC
      [208], int32_t nA, const double b_Linv[169], double RLinv[169], double D
      [169], double b_H[169], int32_t n);
    void ACC_Controller_DropConstraint(int32_t kDrop, bool iA[208], int32_t *nA,
      int32_t iC[208]);
    void ACC_Controller_qpkwik_h(const double b_Linv[169], const double b_Hinv
      [169], const double f[13], const double b[208], bool iA[208], int32_t
      maxiter, double FeasTol, double x[13], double lambda[208], int32_t *status);
    void ACC_Controll_enforceInputLimits(const double uk[2], const double uk1[2],
      const double b_uoff[2], const double b_Mlim[208], const int32_t b_Mrows
      [208], int32_t b_ny, int32_t b_degrees, double uk_clipped[2]);
    void ACC_Contro_mpcblock_optimizer_b(const double rseq[240], const double
      vseq[162], const double umin[2], const double umax[2], const double ymin[3],
      const double ymax[3], int32_t switch_in, const double x[5], const double
      old_u[2], const bool iA[208], const double b_yoff[3], int32_t
      b_enable_value, const double ywt[3], double u[2], double *cost, double
      useq[162], double *status, bool iAout[208]);
    void ACC_Controller_tic(double *tstart_tv_sec, double *tstart_tv_nsec);
    double ACC_Controller_toc(double tstart_tv_sec, double tstart_tv_nsec);
    void ACC_Controller_qpkwik(const double b_Linv[169], const double b_Hinv[169],
      const double f[13], const double b[208], bool iA[208], int32_t maxiter,
      double FeasTol, double x[13], double lambda[208], int32_t *status);
    void ACC_Controll_mpcblock_optimizer(const double rseq[240], const double
      vseq[162], const double umin[2], const double umax[2], const double ymin[3],
      const double ymax[3], int32_t switch_in, const double x[5], const double
      old_u[2], const bool iA[208], const double b_yoff[3], int32_t
      b_enable_value, const double ywt[3], double u[2], double *cost, double
      useq[162], double *status, bool iAout[208]);
    double ACC_Controller_interp1_p(const double varargin_1[10], double
      varargin_3);
    double ACC_Contro_acc_inverse_throttle(double a_req, double v, double gear,
      double rpm, double m);

    // Real-Time Model
    RT_MODEL_ACC_Controller_T ACC_Controller_M;
  };
}

extern const ets2::AccController::ConstB_ACC_Controller_T ACC_Controller_ConstB;// constant block i/o 

// Constant parameters (default storage)
extern const ets2::AccController::ConstP_ACC_Controller_T ACC_Controller_ConstP;

//-
//  These blocks were eliminated from the model due to optimizations:
//
//  Block '<S11>/Constant' : Unused code path elimination
//  Block '<S11>/Floor' : Unused code path elimination
//  Block '<S11>/Floor1' : Unused code path elimination
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
//  Block '<S24>/Vector Dimension Check' : Unused code path elimination
//  Block '<S25>/Vector Dimension Check' : Unused code path elimination
//  Block '<S26>/Vector Dimension Check' : Unused code path elimination
//  Block '<S27>/Vector Dimension Check' : Unused code path elimination
//  Block '<S28>/Vector Dimension Check' : Unused code path elimination
//  Block '<S29>/Vector Dimension Check' : Unused code path elimination
//  Block '<S11>/Min' : Unused code path elimination
//  Block '<S11>/last_x' : Unused code path elimination
//  Block '<S30>/Vector Dimension Check' : Unused code path elimination
//  Block '<S11>/umin_scale5' : Unused code path elimination
//  Block '<S9>/m_zero' : Unused code path elimination
//  Block '<S9>/p_zero' : Unused code path elimination
//  Block '<S33>/Constant' : Unused code path elimination
//  Block '<S33>/Floor' : Unused code path elimination
//  Block '<S33>/Floor1' : Unused code path elimination
//  Block '<S34>/Matrix Dimension Check' : Unused code path elimination
//  Block '<S35>/Matrix Dimension Check' : Unused code path elimination
//  Block '<S36>/Matrix Dimension Check' : Unused code path elimination
//  Block '<S37>/Matrix Dimension Check' : Unused code path elimination
//  Block '<S38>/Matrix Dimension Check' : Unused code path elimination
//  Block '<S39>/Matrix Dimension Check' : Unused code path elimination
//  Block '<S40>/Matrix Dimension Check' : Unused code path elimination
//  Block '<S41>/Matrix Dimension Check' : Unused code path elimination
//  Block '<S42>/Matrix Dimension Check' : Unused code path elimination
//  Block '<S43>/Matrix Dimension Check' : Unused code path elimination
//  Block '<S44>/Matrix Dimension Check' : Unused code path elimination
//  Block '<S45>/Matrix Dimension Check' : Unused code path elimination
//  Block '<S46>/Vector Dimension Check' : Unused code path elimination
//  Block '<S47>/Vector Dimension Check' : Unused code path elimination
//  Block '<S48>/Vector Dimension Check' : Unused code path elimination
//  Block '<S49>/Vector Dimension Check' : Unused code path elimination
//  Block '<S50>/Vector Dimension Check' : Unused code path elimination
//  Block '<S51>/Vector Dimension Check' : Unused code path elimination
//  Block '<S33>/Min' : Unused code path elimination
//  Block '<S33>/last_x' : Unused code path elimination
//  Block '<S52>/Vector Dimension Check' : Unused code path elimination
//  Block '<S33>/umin_scale5' : Unused code path elimination
//  Block '<S10>/m_zero' : Unused code path elimination
//  Block '<S10>/p_zero' : Unused code path elimination
//  Block '<S4>/cost_switch' : Unused code path elimination
//  Block '<S4>/cost_zero' : Unused code path elimination
//  Block '<S4>/est.state_switch' : Unused code path elimination
//  Block '<S4>/est.state_zero' : Unused code path elimination
//  Block '<S4>/x.seq_switch' : Unused code path elimination
//  Block '<S4>/x.seq_zero' : Unused code path elimination
//  Block '<S4>/y.seq_switch' : Unused code path elimination
//  Block '<S4>/y.seq_zero' : Unused code path elimination
//  Block '<S1>/eb_bool' : Eliminate redundant data type conversion


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
//  '<Root>' : 'ACC_Controller'
//  '<S1>'   : 'ACC_Controller/ACC_Controller'
//  '<S2>'   : 'ACC_Controller/ACC_Controller/Allocator'
//  '<S3>'   : 'ACC_Controller/ACC_Controller/InputConditioning'
//  '<S4>'   : 'ACC_Controller/ACC_Controller/MPC'
//  '<S5>'   : 'ACC_Controller/ACC_Controller/MPC_Post'
//  '<S6>'   : 'ACC_Controller/ACC_Controller/MPC_Prep'
//  '<S7>'   : 'ACC_Controller/ACC_Controller/MassEstimator'
//  '<S8>'   : 'ACC_Controller/ACC_Controller/ModeLogic'
//  '<S9>'   : 'ACC_Controller/ACC_Controller/MPC/MPC1'
//  '<S10>'  : 'ACC_Controller/ACC_Controller/MPC/MPC2'
//  '<S11>'  : 'ACC_Controller/ACC_Controller/MPC/MPC1/MPC'
//  '<S12>'  : 'ACC_Controller/ACC_Controller/MPC/MPC1/MPC/MPC Matrix Signal Check'
//  '<S13>'  : 'ACC_Controller/ACC_Controller/MPC/MPC1/MPC/MPC Matrix Signal Check1'
//  '<S14>'  : 'ACC_Controller/ACC_Controller/MPC/MPC1/MPC/MPC Matrix Signal Check2'
//  '<S15>'  : 'ACC_Controller/ACC_Controller/MPC/MPC1/MPC/MPC Preview Signal Check'
//  '<S16>'  : 'ACC_Controller/ACC_Controller/MPC/MPC1/MPC/MPC Preview Signal Check1'
//  '<S17>'  : 'ACC_Controller/ACC_Controller/MPC/MPC1/MPC/MPC Preview Signal Check2'
//  '<S18>'  : 'ACC_Controller/ACC_Controller/MPC/MPC1/MPC/MPC Preview Signal Check3'
//  '<S19>'  : 'ACC_Controller/ACC_Controller/MPC/MPC1/MPC/MPC Preview Signal Check4'
//  '<S20>'  : 'ACC_Controller/ACC_Controller/MPC/MPC1/MPC/MPC Preview Signal Check5'
//  '<S21>'  : 'ACC_Controller/ACC_Controller/MPC/MPC1/MPC/MPC Preview Signal Check6'
//  '<S22>'  : 'ACC_Controller/ACC_Controller/MPC/MPC1/MPC/MPC Preview Signal Check7'
//  '<S23>'  : 'ACC_Controller/ACC_Controller/MPC/MPC1/MPC/MPC Preview Signal Check8'
//  '<S24>'  : 'ACC_Controller/ACC_Controller/MPC/MPC1/MPC/MPC Scalar Signal Check'
//  '<S25>'  : 'ACC_Controller/ACC_Controller/MPC/MPC1/MPC/MPC Scalar Signal Check1'
//  '<S26>'  : 'ACC_Controller/ACC_Controller/MPC/MPC1/MPC/MPC Scalar Signal Check2'
//  '<S27>'  : 'ACC_Controller/ACC_Controller/MPC/MPC1/MPC/MPC Vector Signal Check'
//  '<S28>'  : 'ACC_Controller/ACC_Controller/MPC/MPC1/MPC/MPC Vector Signal Check1'
//  '<S29>'  : 'ACC_Controller/ACC_Controller/MPC/MPC1/MPC/MPC Vector Signal Check6'
//  '<S30>'  : 'ACC_Controller/ACC_Controller/MPC/MPC1/MPC/moorx'
//  '<S31>'  : 'ACC_Controller/ACC_Controller/MPC/MPC1/MPC/optimizer'
//  '<S32>'  : 'ACC_Controller/ACC_Controller/MPC/MPC1/MPC/optimizer/optimizer'
//  '<S33>'  : 'ACC_Controller/ACC_Controller/MPC/MPC2/MPC'
//  '<S34>'  : 'ACC_Controller/ACC_Controller/MPC/MPC2/MPC/MPC Matrix Signal Check'
//  '<S35>'  : 'ACC_Controller/ACC_Controller/MPC/MPC2/MPC/MPC Matrix Signal Check1'
//  '<S36>'  : 'ACC_Controller/ACC_Controller/MPC/MPC2/MPC/MPC Matrix Signal Check2'
//  '<S37>'  : 'ACC_Controller/ACC_Controller/MPC/MPC2/MPC/MPC Preview Signal Check'
//  '<S38>'  : 'ACC_Controller/ACC_Controller/MPC/MPC2/MPC/MPC Preview Signal Check1'
//  '<S39>'  : 'ACC_Controller/ACC_Controller/MPC/MPC2/MPC/MPC Preview Signal Check2'
//  '<S40>'  : 'ACC_Controller/ACC_Controller/MPC/MPC2/MPC/MPC Preview Signal Check3'
//  '<S41>'  : 'ACC_Controller/ACC_Controller/MPC/MPC2/MPC/MPC Preview Signal Check4'
//  '<S42>'  : 'ACC_Controller/ACC_Controller/MPC/MPC2/MPC/MPC Preview Signal Check5'
//  '<S43>'  : 'ACC_Controller/ACC_Controller/MPC/MPC2/MPC/MPC Preview Signal Check6'
//  '<S44>'  : 'ACC_Controller/ACC_Controller/MPC/MPC2/MPC/MPC Preview Signal Check7'
//  '<S45>'  : 'ACC_Controller/ACC_Controller/MPC/MPC2/MPC/MPC Preview Signal Check8'
//  '<S46>'  : 'ACC_Controller/ACC_Controller/MPC/MPC2/MPC/MPC Scalar Signal Check'
//  '<S47>'  : 'ACC_Controller/ACC_Controller/MPC/MPC2/MPC/MPC Scalar Signal Check1'
//  '<S48>'  : 'ACC_Controller/ACC_Controller/MPC/MPC2/MPC/MPC Scalar Signal Check2'
//  '<S49>'  : 'ACC_Controller/ACC_Controller/MPC/MPC2/MPC/MPC Vector Signal Check'
//  '<S50>'  : 'ACC_Controller/ACC_Controller/MPC/MPC2/MPC/MPC Vector Signal Check1'
//  '<S51>'  : 'ACC_Controller/ACC_Controller/MPC/MPC2/MPC/MPC Vector Signal Check6'
//  '<S52>'  : 'ACC_Controller/ACC_Controller/MPC/MPC2/MPC/moorx'
//  '<S53>'  : 'ACC_Controller/ACC_Controller/MPC/MPC2/MPC/optimizer'
//  '<S54>'  : 'ACC_Controller/ACC_Controller/MPC/MPC2/MPC/optimizer/optimizer'

#endif                                 // ACC_CONTROLLER_H_

//
// File trailer for generated code.
//
// [EOF]
//
