// Writes every function as image RVAs: entry, end of the entry's contiguous
// range, body size, name and namespace. ot.cmd hotspots reads this table.
//@category ETS2
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.AddressRange;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import java.io.File;
import java.io.FileOutputStream;
import java.io.OutputStreamWriter;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;

public class ExportFunctions extends GhidraScript {
    @Override
    public void run() throws Exception {
        File out = new File(getScriptArgs()[0]);
        out.getAbsoluteFile().getParentFile().mkdirs();
        long base = currentProgram.getImageBase().getOffset();
        int count = 0;
        try (PrintWriter writer = new PrintWriter(new OutputStreamWriter(new FileOutputStream(out), StandardCharsets.UTF_8))) {
            writer.println("entry,end,size,name,namespace,thunk");
            FunctionIterator functions = currentProgram.getFunctionManager().getFunctions(true);
            while (functions.hasNext() && !monitor.isCancelled()) {
                Function function = functions.next();
                AddressRange first = function.getBody().getRangeContaining(function.getEntryPoint());
                long entry = function.getEntryPoint().getOffset() - base;
                long end = (first != null ? first.getMaxAddress().getOffset() : function.getEntryPoint().getOffset()) - base + 1;
                String namespace = function.getParentNamespace().isGlobal() ? "" : function.getParentNamespace().getName(true);
                writer.printf("%X,%X,%d,%s,%s,%d%n", entry, end, function.getBody().getNumAddresses(),
                              csv(function.getName()), csv(namespace), function.isThunk() ? 1 : 0);
                ++count;
            }
        }
        println("Exported " + count + " functions to " + out);
    }

    static String csv(String text) {
        return text.contains(",") || text.contains("\"") ? "\"" + text.replace("\"", "\"\"") + "\"" : text;
    }
}
