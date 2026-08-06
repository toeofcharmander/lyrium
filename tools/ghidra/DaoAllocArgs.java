// Recover the arguments actually passed at a list of allocation call sites.
//
// On 32-bit MSVC these arrive by PUSH, and the one at 0x004B8F30 arrives by
// MOV dword ptr [ESP + 0x18], imm32. The decompiler has already reconstructed those
// into CALL p-code inputs as a side effect of doing its job, which is exactly what a
// register-oriented value analysis will not do for you -- so read them from there
// rather than reimplementing stack tracking.
//
// Run this on the few hundred import call sites, not on the thousands of malloc
// callers: it costs roughly 0.1-1s per function.
//
// decomp_status is emitted per row on purpose. On a stripped 10 MB C++ binary a real
// fraction of functions time out, and "no constant found" has to stay distinguishable
// from "the decompiler gave up".
//
//@category Lyrium

import java.io.BufferedWriter;
import java.io.File;
import java.io.FileOutputStream;
import java.io.OutputStreamWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Iterator;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;
import java.util.TreeSet;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.decompiler.component.DecompilerUtils;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.pcode.HighFunction;
import ghidra.program.model.pcode.HighVariable;
import ghidra.program.model.pcode.PcodeOp;
import ghidra.program.model.pcode.PcodeOpAST;
import ghidra.program.model.pcode.Varnode;

public class DaoAllocArgs extends GhidraScript {

    private static final int DECOMPILE_TIMEOUT_SECONDS = 120;

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            printerr("usage: DaoAllocArgs <outDir> <siteListFile>");
            return;
        }
        File outDir = new File(args[0]);
        outDir.mkdirs();
        File siteList = new File(args[1]);
        if (!siteList.isFile()) {
            printerr("site list not found: " + siteList);
            return;
        }

        // Group by containing function so each one is decompiled exactly once.
        Map<Address, java.util.Set<Address>> byFunction = new TreeMap<>();
        Map<Address, String> apiOfSite = new TreeMap<>();
        int orphans = 0;
        for (String line : Files.readAllLines(siteList.toPath(), StandardCharsets.UTF_8)) {
            String[] c = line.trim().split("\t", -1);
            if (c.length == 0 || !c[0].startsWith("0")) {
                continue;
            }
            Address site = toAddr(Long.parseLong(c[0].replaceFirst("^0[xX]", ""), 16));
            apiOfSite.put(site, c.length > 1 ? c[1] : "-");
            Function f = getFunctionContaining(site);
            if (f == null) {
                orphans++;
                continue;
            }
            byFunction.computeIfAbsent(f.getEntryPoint(), k -> new TreeSet<>()).add(site);
        }
        if (orphans > 0) {
            println("DaoAllocArgs: " + orphans + " sites are not inside any function; skipped");
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

        int rows = 0;
        try (BufferedWriter w = open(new File(outDir, "alloc_args.tsv"))) {
            row(w, "call_site_va", "api", "func_va", "arg_index", "is_const", "value",
                    "decomp_status");
            for (Map.Entry<Address, java.util.Set<Address>> e : byFunction.entrySet()) {
                if (monitor.isCancelled()) {
                    break;
                }
                Function f = getFunctionAt(e.getKey());
                DecompileResults res = ifc.decompileFunction(f, DECOMPILE_TIMEOUT_SECONDS, monitor);
                String status = status(res);
                HighFunction hf = res.getHighFunction();
                for (Address site : e.getValue()) {
                    String api = apiOfSite.getOrDefault(site, "-");
                    if (hf == null) {
                        row(w, site.toString(), api, e.getKey().toString(), "-", "-", "-", status);
                        rows++;
                        continue;
                    }
                    List<String[]> found = argsAt(hf, site);
                    if (found.isEmpty()) {
                        row(w, site.toString(), api, e.getKey().toString(), "-", "-",
                                "NO_CALL_OP", status);
                        rows++;
                    }
                    for (String[] a : found) {
                        row(w, site.toString(), api, e.getKey().toString(), a[0], a[1], a[2], status);
                        rows++;
                    }
                }
            }
        }
        finally {
            ifc.dispose();
        }
        println("DaoAllocArgs: " + rows + " rows across " + byFunction.size() + " functions");
    }

    private static List<String[]> argsAt(HighFunction hf, Address site) {
        List<String[]> out = new ArrayList<>();
        Iterator<PcodeOpAST> ops = hf.getPcodeOps(site.getPhysicalAddress());
        while (ops.hasNext()) {
            PcodeOpAST op = ops.next();
            if (op.getOpcode() != PcodeOp.CALL && op.getOpcode() != PcodeOp.CALLIND) {
                continue;
            }
            // Input 0 is the call target; the arguments start at 1.
            for (int i = 1; i < op.getNumInputs(); i++) {
                Varnode v = op.getInput(i);
                if (v == null) {
                    continue;
                }
                if (v.isConstant()) {
                    out.add(new String[] {String.valueOf(i - 1), "true",
                            "0x" + Long.toHexString(v.getOffset())});
                    continue;
                }
                HighVariable hv = v.getHigh();
                out.add(new String[] {String.valueOf(i - 1), "false",
                        hv == null || hv.getName() == null ? v.toString() : hv.getName()});
            }
        }
        return out;
    }

    private static String status(DecompileResults res) {
        if (res == null) {
            return "NULL_RESULT";
        }
        if (res.decompileCompleted()) {
            return "ok";
        }
        String message = res.getErrorMessage();
        return message == null || message.isEmpty() ? "FAILED" : "FAILED:" + message.replace('\t', ' ');
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
