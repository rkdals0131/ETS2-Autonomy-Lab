#pragma once
#include "drive_api.hpp"
#include <scssdk_input.h>
namespace ot {
const OtDriveHost* drive_host();
scs_result_t input_initialize(scs_u32_t,const scs_input_init_params_t*);
void input_shutdown();
void input_pause(bool);
void input_release(OtDriveReason);
}
