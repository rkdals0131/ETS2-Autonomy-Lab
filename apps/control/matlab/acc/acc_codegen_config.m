function acc_codegen_config(model)
%ACC_CODEGEN_CONFIG Solver and Embedded Coder settings of ACC_Controller.slx.
%   Fixed-step discrete 0.05 s, ERT, C++ class interface, no dynamic memory, inlined
%   parameters, single rate, class ets2::AccController. Called by build_acc_controller.m (so the saved model already
%   carries them) and again by generate_acc_code.m.
set_param(model, "SolverType", "Fixed-step", "Solver", "FixedStepDiscrete", "FixedStep", "0.05", ...
    "StopTime", "inf");
set_param(model, "SystemTargetFile", "ert.tlc");
set_param(model, "TargetLang", "C++");
set_param(model, "CodeInterfacePackaging", "C++ class");
set_param(model, "GenerateReport", "on", "LaunchReport", "off");
set_param(model, "MultiInstanceErrorCode", "Error");
set_param(model, "DefaultParameterBehavior", "Inlined");
set_param(model, "MATLABDynamicMemAlloc", "off");
set_param(model, "SupportVariableSizeSignals", "off");
set_param(model, "SupportNonFinite", "on");          % the MPC solver uses Inf for unused bounds
set_param(model, "CombineOutputUpdateFcns", "on");
set_param(model, "GenerateSampleERTMain", "on");
set_param(model, "IncludeMdlTerminateFcn", "on");
set_param(model, "ProdHWDeviceType", "Intel->x86-64 (Windows64)");   % host = SIL; types come from rtwtypes.h
set_param(model, "GenCodeOnly", "off");
set_param(model, "InstructionSetExtensions", {'None'});   % no SSE intrinsics: portable C++
set_param(model, "SaveFormat", "Dataset");
% Utility functions outside the class (rt_powd_snf, rt_hypotd_snf, div_nde_s32_floor, ...) have
% external linkage; prefix them with the model name so they do not clash with other generated
% code linked into the same node (the LCC controller defines the same names).
set_param(model, "CustomSymbolStrUtil", "$R$N$C");
% C++ class ets2::AccController (code mappings)
try
    cm = coder.mapping.api.get(model);
catch
    coder.mapping.utils.create(model);
    cm = coder.mapping.api.get(model);
end
setClassName(cm, "AccController");
setClassNamespace(cm, "ets2");
end
