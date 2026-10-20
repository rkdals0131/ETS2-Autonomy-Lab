function info = generate_lcc_code()
%GENERATE_LCC_CODE Embedded Coder C++ for LCC_Controller only (reproducible).
%   info = generate_lcc_code() builds LCC_Controller.slx with ERT / C++ class settings
%   (lcc_codegen_config.m: fixed step 0.02 s, no dynamic memory, inlined parameters), class
%   ets2::LccController, into codegen/ (git-ignored build folder). The build compiles and links
%   the generated sample main (ert_main.cpp) with the host toolchain (MSVC 2022 here), which is
%   the stand-alone compile check. The sources are then copied flat into generated/ (versioned)
%   with packNGo: the model code, the MathWorks run-time files it needs and ert_main.cpp.
here = fileparts(mfilename("fullpath"));
cd(here);
model = 'LCC_Controller';
lcc_controller_init();
if ~isfile(fullfile(here, model + ".slx")), build_lcc_controller(); end
load_system(model);
lcc_codegen_config(model);
cg = fullfile(here, "codegen");
if ~isfolder(cg), mkdir(cg); end
Simulink.fileGenControl("set", "CodeGenFolder", cg, "CacheFolder", fullfile(cg, "cache"), "createDir", true);
restore = onCleanup(@() Simulink.fileGenControl("reset"));
t0 = tic;
slbuild(model);
info.build_s = toc(t0);

src = fullfile(cg, model + "_ert_rtw");
w = warning("off", "all");
S = load(fullfile(src, "buildInfo.mat"), "buildInfo");
warning(w);
zipFile = fullfile(cg, "lcc_controller_src.zip");
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
