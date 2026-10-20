function status = compile_generated()
%COMPILE_GENERATED Compile generated/ + example/acc_smoke_main.cpp without MATLAB's build.
%   status = compile_generated() calls MSVC (vcvars64 of the installed Build Tools) on the
%   versioned sources only, links codegen/smoke/acc_smoke.exe and runs it. This shows that
%   generated/ is complete for a stand-alone C++ build (e.g. a ROS 2 node).
here = fileparts(mfilename("fullpath"));
out = fullfile(here, "codegen", "smoke");
if ~isfolder(out), mkdir(out); end
cc = mex.getCompilerConfigurations("C++", "Selected");
vcvars = fullfile(cc.Location, "VC", "Auxiliary", "Build", "vcvars64.bat");
gen = fullfile(here, "generated");
src = ["ACC_Controller.cpp", "ACC_Controller_data.cpp", "rtGetInf.cpp", "rtGetNaN.cpp", "rt_nonfinite.cpp"];
srcList = strjoin(compose('"%s"', fullfile(gen, src)), " ");
cmd = sprintf(['call "%s" >nul && cd /d "%s" && cl /nologo /EHsc /O2 /std:c++17 /W3 /I"%s" ' ...
    '"%s" %s /Tc"%s" /Fe:acc_smoke.exe && acc_smoke.exe'], vcvars, out, gen, ...
    fullfile(here, "example", "acc_smoke_main.cpp"), srcList, fullfile(gen, "coder_posix_time.c"));
[status, txt] = system(cmd);
disp(txt);
fprintf("compile + run status %d\n", status);
end
