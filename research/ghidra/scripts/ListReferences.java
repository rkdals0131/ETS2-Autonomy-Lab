// Lists the instructions and functions that reference each given image RVA
// (a global, a vtable or a function), one line per reference.
// Arguments: <out file> <rva> [<rva> ...]
//@category ETS2
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import java.io.File;
import java.io.FileOutputStream;
import java.io.OutputStreamWriter;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;

public class ListReferences extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File out = new File(args[0]);
        out.getAbsoluteFile().getParentFile().mkdirs();
        long base = currentProgram.getImageBase().getOffset();
        try (PrintWriter writer = new PrintWriter(new OutputStreamWriter(new FileOutputStream(out), StandardCharsets.UTF_8))) {
            writer.println("target,from,type,function_entry,function");
            for (int i = 1; i < args.length; ++i) {
                long rva = Long.parseUnsignedLong(args[i].replaceFirst("^(0x|0X)", ""), 16);
                Address target = toAddr(base + rva);
                int count = 0;
                for (Reference reference : getReferencesTo(target)) {
                    Address from = reference.getFromAddress();
                    Function function = getFunctionContaining(from);
                    writer.printf("%X,%X,%s,%s,%s%n", rva, from.getOffset() - base, reference.getReferenceType(),
                                  function == null ? "" : String.format("%X", function.getEntryPoint().getOffset() - base),
                                  function == null ? "" : function.getName(true));
                    ++count;
                }
                println(String.format("0x%X: %d references", rva, count));
            }
        }
    }
}
