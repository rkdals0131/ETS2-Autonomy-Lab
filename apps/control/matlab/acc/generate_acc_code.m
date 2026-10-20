function info = generate_acc_code()
%GENERATE_ACC_CODE Embedded Coder C++ for ACC_Controller only (reproducible).
%   info = generate_acc_code() builds ACC_Controller.slx with ERT / C++ class settings
%   (acc_codegen_config.m: fixed step 0.05 s, no dynamic memory, inlined parameters), class
%   ets2::AccController, into codegen/ (git-ignored build folder). The build compiles and
%   links the generated sample main (ert_main.cpp) with the host toolchain (MSVC 2022 here),
%   which is the stand-alone compile check. The sources are then copied flat into
%   generated/ (versioned) with packNGo: the model code, the MathWorks run-time files it\n%   needs and ert_main.cpp.
here = fileparts(mfilename("fullpath"));
cd(here);
model = 'ACC_Controller';
acc_controller_init();
if ~isfile(fullfile(here, model + ".slx")), build_acc_controller(); end
load_system(model);
acc_codegen_config(model);
cg = fullfile(here, "codegen");
if ~isfolder(cg), mkdir(cg); end
Simulink.fileGenControl("set", "CodeGenFolder", cg, "CacheFolder", fullfile(cg, "cache"), "createDir", true);
restore = onCleanup(@() Simulink.fileGenControl("reset"));
t0 = tic;
slbuild(model);
info.build_s = toc(t0);

src = fullfile(cg, model + "_ert_rtw");
% packNGo collects every file the build compiled (model code, shared utilities, the
% MathWorks run-time files such as coder_posix_time used by the QP solver's timer, ert_main)
w = warning("off", "all");
S = load(fullfile(src, "buildInfo.mat"), "buildInfo");
warning(w);
zipFile = fullfile(cg, "acc_controller_src.zip");
packNGo(S.buildInfo, "packType", "flat", "fileName", zipFile, "minimalHeaders", true);
out = fullfile(here, "generated");
if isfolder(out), rmdir(out, "s"); end
mkdir(out);
unzipped = unzip(zipFile, out);
names = strings(0, 1);
for i = 1:numel(unzipped)
    [~, n, e] = fileparts(unzipped{i});
    if ismember(e, [".cpp", ".c", ".h", ".hpp"])
        names(end+1) = string(n) + string(e); %#ok<AGROW>
    else
        delete(unzipped{i});                 % build files (.mk, .bat, .rsp, .mat) stay in codegen/
    end
end
info.files = sort(names);
exe = dir(fullfile(cg, model + "*.exe"));
info.executable = "";
if ~isempty(exe), info.executable = string(fullfile(exe(1).folder, exe(1).name)); end
info.header = "generated/" + model + ".h";
fprintf("generated %d files into generated/, build %.0f s, executable: %s\n", numel(names), info.build_s, info.executable);
close_system(model, 0);
end
