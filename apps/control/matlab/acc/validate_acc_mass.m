function V = validate_acc_mass(opts)
%VALIDATE_ACC_MASS Closed-loop check of the mass-adaptive ACC at several plant masses.
%   V = validate_acc_mass() runs every scenario of acc_scenarios.m on acc_closed_loop.slx for
%   plant masses opts.Masses (12 t = truck alone without trailer brakes, the others with the
%   trailer) and three controller set-ups:
%     default  adaptive, mass_prior_kg = 0 (the estimator starts from the 20.3 t default)
%     prior    adaptive, mass_prior_kg = plant mass
%     fixed    the 20.3 t maps of the previous controller (C.est.adaptive = false)
%   plus, on mass_mixed, road-grade and input-source variants of the estimator (grade +2 % /
%   -1 % with and without pitch_rad, own commands instead of the applied pedals).
%   With the correct prior it also fits the lag from a_des to the plant acceleration
%   (identify_closed_loop_lag) per mass, to check that the MPC's acceleration-domain model holds.
%   Writes results/mass_validation.json (all metrics) and prints the tables used in README.md.
%   The figures of mass_mixed (default set-up) go to results/scenario_mass_mixed_m<mass>.png.
arguments
    opts.Masses (1,:) double = [12000 20300 30000 40000]
    opts.Setups (1,:) string = ["default", "prior", "fixed"]
end
here = fileparts(mfilename("fullpath"));
cd(here);
S = acc_scenarios();
V.runs = struct([]);
for setup = opts.Setups
    for mk = opts.Masses
        switch setup
            case "default", prior = 0; adaptive = true;
            case "prior", prior = mk; adaptive = true;
            case "fixed", prior = 0; adaptive = false;
        end
        fprintf("===== %s, plant %.0f kg\n", setup, mk);
        [~, M] = run_acc_scenarios(Scenarios=S, PlantMass=mk, MassPrior=prior, Adaptive=adaptive, ...
            Plot=false, Save=false);
        for i = 1:numel(M)
            r = M(i); r.setup = setup;
            V.runs = append(V.runs, r);
        end
        if setup == "default"
            run_acc_scenarios(Names="mass_mixed", PlantMass=mk, Plot=true, Save=false, Tag="_m" + mk);
        end
        if setup == "prior"
            lagNames = ["setspeed_60_80", "lead_brake_2", "lead_brake_4", "cut_in", "stopped_vehicle"];
            tag = "_lag_m" + mk;
            run_acc_scenarios(Names=lagNames, PlantMass=mk, MassPrior=mk, Plot=false, Save=true, Tag=tag);
            L = identify_closed_loop_lag(lagNames, Tag=tag, Save=false);
            for j = 1:numel(L), L(j).plant_mass_kg = mk; end
            if isfield(V, "lag"), V.lag = [V.lag L]; else, V.lag = L; end
            delete(fullfile(here, "results", "scenario_*" + tag + ".json"));
        end
    end
end

% estimator variants on mass_mixed
hill = [0 0; 20 0; 30 0.02; 80 0.02; 90 -0.01; 140 -0.01; 150 0];
var = struct("name", {"hill_pitch", "hill_no_pitch", "own_commands"}, ...
    "args", {{"Grade", hill, "PitchKnown", true}, {"Grade", hill, "PitchKnown", false}, {"ActuationValid", false}});
V.variants = struct([]);
for mk = [20300 40000]
    for k = 1:numel(var)
        [~, M] = run_acc_scenarios("Names", "mass_mixed", "PlantMass", mk, "Plot", false, "Save", false, var(k).args{:});
        r = M(1); r.setup = var(k).name;
        V.variants = append(V.variants, r);
    end
end

fid = fopen(fullfile(here, "results", "mass_validation.json"), "w");
fwrite(fid, jsonencode(V, PrettyPrint=true)); fclose(fid);
print_tables(V);
end

% =====================================================================
function A = append(A, r)
if isempty(A), A = r; return, end
f = setdiff(fieldnames(r), fieldnames(A));
for i = 1:numel(f), [A.(f{i})] = deal([]); end
f = setdiff(fieldnames(A), fieldnames(r));
for i = 1:numel(f), r.(f{i}) = []; end
A(end+1) = orderfields(r, A(1));
end

function print_tables(V)
R = V.runs;
names = unique(string({R.name}), "stable");
masses = unique([R.plant_mass_kg]);
setups = unique(string({R.setup}), "stable");
fprintf("\nscenario | mass t | setup | collision | min gap m | min TTC s | peak decel | max jerk | jerk 1 s | brake s / kJ | ret changes | final mass t | within 5 %% from s\n");
for n = names
    for mk = masses
        for su = setups
            r = R(string({R.name}) == n & [R.plant_mass_kg] == mk & string({R.setup}) == su);
            if isempty(r), continue, end
            fprintf("%s | %.1f | %s | %d | %.1f | %.1f | %.2f | %.2f | %.2f | %.1f / %.0f | %d | %.1f | %.1f\n", n, mk/1000, su, ...
                r.collision, r.min_gap_m, r.min_ttc_s, r.peak_decel_mps2, r.max_abs_jerk_mps3, r.max_jerk_1s_mps3, ...
                r.brake_time_s, r.brake_energy_kJ, r.retarder_changes, r.mass_final_kg/1000, r.mass_within5_s);
        end
    end
end
fprintf("\nvariants (mass_mixed)\n");
for r = V.variants
    fprintf("%s | %.1f | final %.0f kg (%+.1f %%) | within 5 %% from %.1f s | valid at %.1f s | collision %d\n", r.setup, ...
        r.plant_mass_kg/1000, r.mass_final_kg, 100*(r.mass_final_kg/r.plant_mass_kg - 1), r.mass_within5_s, r.mass_valid_at_s, r.collision);
end
fprintf("\nany collision: %d\n", any([R.collision]) || any([V.variants.collision]));
end
