// Dump decompiled C for a shortlist of functions, one file each.
//
// An address that is not the entry point of a function resolves to whatever function
// contains it, so a call-site list can be fed in directly.
//
//@category Lyrium

import java.io.BufferedWriter;
import java.io.File;
import java.io.FileOutputStream;
import java.io.OutputStreamWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.TreeMap;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.decompiler.DecompiledFunction;
import ghidra.app.decompiler.component.DecompilerUtils;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class DaoDecompile extends GhidraScript {

    // The functions worth reading here are large; the 30s default gives up on them.
    private static final int DECOMPILE_TIMEOUT_SECONDS = 300;

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            printerr("usage: DaoDecompile <outDir> <decompDir> [@listfile | 0xADDR ...]");
            return;
        }
        File outDir = new File(args[0]);
        File decompDir = new File(args[1]);
        outDir.mkdirs();
        decompDir.mkdirs();

        List<String> tokens = new ArrayList<>();
        for (int i = 2; i < args.length; i++) {
            if (args[i].startsWith("@")) {
                for (String line : Files.readAllLines(new File(args[i].substring(1)).toPath(),
                        StandardCharsets.UTF_8)) {
                    String[] c = line.trim().split("\t", -1);
                    if (c.length > 0 && c[0].startsWith("0")) {
                        tokens.add(c[0]);
                    }
                }
            }
            else {
                tokens.add(args[i]);
            }
        }
        if (tokens.isEmpty()) {
            printerr("nothing to decompile");
            return;
        }

        // Distinct containing functions, in address order.
        TreeMap<Address, Function> wanted = new TreeMap<>();
        int unresolved = 0;
        for (String t : tokens) {
            Address a = toAddr(Long.parseLong(t.replaceFirst("^0[xX]", ""), 16));
            Function f = getFunctionContaining(a);
            if (f == null) {
                printerr("no function contains " + a + " -- analysis missed it");
                unresolved++;
                continue;
            }
            wanted.put(f.getEntryPoint(), f);
        }

        DecompInterface ifc = new DecompInterface();
        ifc.setOptions(DecompilerUtils.getDecompileOptions(state.getTool(), currentProgram));
        ifc.toggleCCode(true);
        ifc.toggleSyntaxTree(true);
        ifc.setSimplificationStyle("decompile");
        if (!ifc.openProgram(currentProgram)) {
            printerr("decompiler would not open the program: " + ifc.getLastMessage());
            return;
        }

        int written = 0;
        try (BufferedWriter index = open(new File(outDir, "decomp_index.tsv"))) {
            row(index, "func_va", "func_name", "file", "body_bytes", "ncallers", "status");
            for (Function f : wanted.values()) {
                if (monitor.isCancelled()) {
                    break;
                }
                DecompileResults res = ifc.decompileFunction(f, DECOMPILE_TIMEOUT_SECONDS, monitor);
                DecompiledFunction dec = res == null ? null : res.getDecompiledFunction();
                String status = res == null ? "NULL_RESULT"
                        : res.decompileCompleted() ? "ok" : "FAILED:" + String.valueOf(res.getErrorMessage());
                String name = f.getEntryPoint() + "_" + sanitize(f.getName());
                File target = new File(decompDir, name + ".c");
                if (dec != null) {
                    try (BufferedWriter w = open(target)) {
                        w.write("/* " + f.getName() + "\n");
                        w.write(" * entry      " + f.getEntryPoint() + "\n");
                        w.write(" * body       " + f.getBody().getNumAddresses() + " bytes\n");
                        w.write(" * callers    " + f.getCallingFunctions(monitor).size() + "\n");
                        w.write(" * signature  " + dec.getSignature() + "\n");
                        w.write(" * status     " + status + "\n");
                        w.write(" */\n\n");
                        w.write(dec.getC());
                    }
                    written++;
                }
                row(index, f.getEntryPoint().toString(), f.getName(),
                        dec == null ? "-" : target.getName(),
                        String.valueOf(f.getBody().getNumAddresses()),
                        String.valueOf(f.getCallingFunctions(monitor).size()),
                        status);
            }
        }
        finally {
            ifc.dispose();
        }
        println("DaoDecompile: wrote " + written + " of " + wanted.size() + " functions to "
                + decompDir + " (" + unresolved + " addresses unresolved)");
    }

    private static String sanitize(String name) {
        return name.replaceAll("[^A-Za-z0-9_.-]", "_");
    }

    static BufferedWriter open(File f) throws Exception {
        return new BufferedWriter(
                new OutputStreamWriter(new FileOutputStream(f), StandardCharsets.UTF_8));
    }

    static void row(BufferedWriter w, String... cells) throws Exception {
        w.write(String.join("\t", cells));
        w.write("\n");
    }
}
