// Writes the program's initialized memory as a flat file indexed by image RVA
// (uninitialized ranges and gaps stay zero), so a later build can be compared with
// this one byte for byte after the executable on disk has been replaced.
// Arguments: <out file>
//@category ETS2
import ghidra.app.script.GhidraScript;
import ghidra.program.model.mem.MemoryBlock;
import java.io.RandomAccessFile;

public class DumpImage extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        long base = currentProgram.getImageBase().getOffset();
        try (RandomAccessFile out = new RandomAccessFile(args[0], "rw")) {
            out.setLength(0);
            for (MemoryBlock block : currentProgram.getMemory().getBlocks()) {
                if (!block.isInitialized() || block.getStart().getOffset() < base) {
                    continue;
                }
                byte[] bytes = new byte[(int) block.getSize()];
                block.getBytes(block.getStart(), bytes);
                out.seek(block.getStart().getOffset() - base);
                out.write(bytes);
                println(String.format("%s 0x%X +0x%X", block.getName(), block.getStart().getOffset() - base, bytes.length));
            }
        }
    }
}
