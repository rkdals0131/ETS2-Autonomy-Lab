function acc_controller_init()
%ACC_CONTROLLER_INIT Put the MPC objects of ACC_Controller.slx in the base workspace.
%   The MPC blocks resolve their controller objects in the base workspace only, so the
%   model's InitFcn calls this (the other parameters live in the model workspace).
%   Creates mpcComfort and mpcEmergency from acc_controller_params / acc_design_mpc.
C = acc_controller_params();
[mpcComfort, mpcEmergency] = acc_design_mpc(C);
assignin("base", "mpcComfort", mpcComfort);
assignin("base", "mpcEmergency", mpcEmergency);
end
