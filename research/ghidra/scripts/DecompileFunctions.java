// Decompiles the functions containing the given image RVAs into
// <out>/<entry RVA>.c with callers, callees and the instructions around the
// requested address (a hook site or a sampled return address).
// Arguments: <out directory> <rva> [<rva> ...]
//@category ETS2
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.Listing;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;
import java.util.Set;

public class DecompileFunctions extends GhidraScript {
    long base;

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File directory = new File(args[0]);
        directory.mkdirs();
        base = currentProgram.getImageBase().getOffset();
        DecompInterface decompiler = new DecompInterface();
        decompiler.setOptions(new DecompileOptions());
        decompiler.toggleCCode(true);
        decompiler.setSimplificationStyle("decompile");
        if (!decompiler.openProgram(currentProgram)) throw new Exception("Decompiler: " + decompiler.getLastMessage());
        try {
            for (int i = 1; i < args.length && !monitor.isCancelled(); ++i) {
                long rva = Long.parseUnsignedLong(args[i].replaceFirst("^(0x|0X)", ""), 16);
                Address address = toAddr(base + rva);
                Function function = getFunctionContaining(address);
                if (function == null) {
                    // Targets reached only through vtables may lack a function; the
                    // project is opened read-only, so this one is not saved.
                    disassemble(address);
                    function = createFunction(address, null);
                    if (function != null) println("Created a temporary function at RVA " + hex(rva));
                }
                if (function == null) {
                    println("No function contains RVA " + hex(rva));
                    continue;
                }
                StringBuilder text = new StringBuilder();
                long entry = function.getEntryPoint().getOffset() - base;
                text.append("// ").append(function.getName(true)).append("  entry ").append(hex(entry))
                    .append("  body ").append(function.getBody().getNumAddresses()).append(" bytes\n");
                text.append("// requested ").append(hex(rva)).append(" (+").append(hex(rva - entry)).append(")\n");
                appendFunctions(text, "callers", function.getCallingFunctions(monitor));
                appendFunctions(text, "callees", function.getCalledFunctions(monitor));
                appendListing(text, address);
                DecompileResults result = decompiler.decompileFunction(function, 180, monitor);
                if (result.decompileCompleted()) text.append(result.getDecompiledFunction().getC());
                else text.append("// decompilation failed: ").append(result.getErrorMessage()).append('\n');
                File out = new File(directory, String.format("%X.c", entry));
                Files.writeString(out.toPath(), text, StandardCharsets.UTF_8);
                println("Wrote " + out);
            }
        } finally {
            decompiler.dispose();
        }
    }

    void appendFunctions(StringBuilder text, String title, Set<Function> functions) {
        List<Function> sorted = new ArrayList<>(functions);
        sorted.sort(Comparator.comparingLong(f -> f.getEntryPoint().getOffset()));
        text.append("// ").append(title).append(" (").append(sorted.size()).append(")\n");
        for (Function f : sorted.subList(0, Math.min(sorted.size(), 60)))
            text.append("//   ").append(hex(f.getEntryPoint().getOffset() - base)).append(' ').append(f.getName(true)).append('\n');
    }

    void appendListing(StringBuilder text, Address address) {
        Listing listing = currentProgram.getListing();
        Instruction instruction = listing.getInstructionContaining(address);
        if (instruction == null) return;
        for (int i = 0; i < 12 && instruction.getPrevious() != null; ++i) instruction = instruction.getPrevious();
        text.append("// listing\n");
        for (int i = 0; i < 25 && instruction != null; ++i, instruction = instruction.getNext()) {
            long rva = instruction.getAddress().getOffset() - base;
            text.append(instruction.getAddress().equals(listing.getInstructionContaining(address).getAddress()) ? "// > " : "//   ")
                .append(hex(rva)).append("  ").append(instruction).append('\n');
        }
    }

    static String hex(long value) {
        return String.format("0x%X", value);
    }
}
