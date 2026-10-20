function lcc_codegen_config(model)
%LCC_CODEGEN_CONFIG Solver and Embedded Coder settings of LCC_Controller.slx.
%   Fixed-step discrete 0.02 s, ERT, C++ class interface, no dynamic memory, inlined
%   parameters, single rate, class ets2::LccController (same settings as the ACC,
%   acc_codegen_config.m). Called by build_lcc_controller.m and generate_lcc_code.m.
set_param(model, "SolverType", "Fixed-step", "Solver", "FixedStepDiscrete", "FixedStep", "0.02", ...
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
try
    cm = coder.mapping.api.get(model);
catch
    coder.mapping.utils.create(model);
    cm = coder.mapping.api.get(model);
end
setClassName(cm, "LccController");
setClassNamespace(cm, "ets2");
end
