// Ghidra headless script: disassemble and decompile image RVA ranges, with no full analysis pass.
//
// Usage (from a checkout, with the title image and a Ghidra install available):
//   python -I tooling/ghidra/decompile_callers.py --help
//
// The script is invoked by that wrapper (which builds the analyzeHeadless command line); it can
// also be named directly:
//   analyzeHeadless <project-dir> <project> -process <program> -noanalysis -readOnly \
//     -scriptPath tooling/ghidra -postScript DecompileRanges.java "32520-33cb5,2481e90-2482653"
//
// Argument: one comma-separated list of `startRVA-endRVA` ranges, hex, `0x` optional. RVAs are
// relative to the PE image base. For each range the script disassembles from its start, creates a
// function there when none exists, and prints the decompiled C between two markers:
//
//   === RVA 0x32520..0x33cb5 ===
//   <decompiled C>
//
// tooling/ghidra/split_decomp.py turns one headless log into one .c file per range.
//
// Why -noanalysis: a full auto-analysis of a large title takes far longer than a targeted
// decompile, and the decompiler only needs the bytes it is asked for. The practical consequence
// is that function starts must be supplied by the caller; tooling/ghidra/rva2func.py reads them
// from the PE's exception directory (.pdata), which is authoritative and needs no analysis.
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.util.task.ConsoleTaskMonitor;

public class DecompileRanges extends GhidraScript {
    private static final int kDecompileSeconds = 180;

    @Override
    public void run() throws Exception {
        String[] parts = getScriptArgs();
        if (parts.length == 0) {
            println("usage: DecompileRanges <startRVA-endRVA[,startRVA-endRVA...]>");
            return;
        }
        long imageBase = currentProgram.getImageBase().getOffset();
        DecompInterface ifc = new DecompInterface();
        ifc.openProgram(currentProgram);
        ConsoleTaskMonitor monitor = new ConsoleTaskMonitor();
        for (String range : String.join(",", parts).split(",")) {
            String[] se = range.split("-");
            if (se.length != 2) {
                println("[expected startRVA-endRVA, got '" + range + "']");
                continue;
            }
            long rvaStart, rvaEnd;
            try {
                rvaStart = Long.parseLong(se[0].trim().replaceFirst("^0[xX]", ""), 16);
                rvaEnd = Long.parseLong(se[1].trim().replaceFirst("^0[xX]", ""), 16);
            } catch (NumberFormatException e) {
                println("[not a hex RVA range: '" + range + "']");
                continue;
            }
            Address start = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(imageBase + rvaStart);
            println("=== RVA 0x" + Long.toHexString(rvaStart) + "..0x" + Long.toHexString(rvaEnd) + " ===");
            try {
                new DisassembleCommand(start, null, true).applyTo(currentProgram, monitor);
                Function f = getFunctionAt(start);
                if (f == null) f = createFunction(start, "fn_" + Long.toHexString(rvaStart));
                if (f == null) {
                    println("[could not create a function at 0x" + Long.toHexString(rvaStart) + "]");
                    continue;
                }
                DecompileResults res = ifc.decompileFunction(f, kDecompileSeconds, monitor);
                if (res != null && res.decompileCompleted()) {
                    println(res.getDecompiledFunction().getC());
                } else {
                    println("[decompile failed for 0x" + Long.toHexString(rvaStart) + "]");
                }
            } catch (Exception e) {
                println("[error at 0x" + Long.toHexString(rvaStart) + ": " + e + "]");
            }
        }
        ifc.dispose();
    }
}
