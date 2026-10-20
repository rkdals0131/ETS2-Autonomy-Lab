function lcc_controller_init()
%LCC_CONTROLLER_INIT Put the MPC object of LCC_Controller.slx in the base workspace.
%   The MPC block resolves its controller object in the base workspace only, so the model's
%   InitFcn calls this (the other parameters live in the model workspace). Creates mpcLcc
%   from lcc_controller_params / lcc_design_mpc.
C = lcc_controller_params();
assignin("base", "mpcLcc", lcc_design_mpc(C));
end
