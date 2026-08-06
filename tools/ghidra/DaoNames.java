// Apply the names lyrium already knows to the Ghidra database, so every later report
// says texture_cache_evict instead of FUN_008481f0.
//
// This is the one script that writes. Run it WITHOUT -readOnly; every other script in
// this directory is a pure reporter and should keep -readOnly on.
//
// Headless does not wrap post-scripts in a transaction (HeadlessAnalyzer.runScript
// calls script.execute directly), so this opens its own.
//
//@category Lyrium

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

public class DaoNames extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            printerr("usage: DaoNames <outDir>");
            return;
        }
        File known = new File(args[0], "known_addresses.tsv");
        if (!known.isFile()) {
            printerr("missing " + known + " -- run tools/ghidra/emit_known_addresses.py first");
            return;
        }

        int named = 0;
        int labelled = 0;
        int created = 0;
        int tx = currentProgram.startTransaction("lyrium: apply known names");
        try {
            for (String line : Files.readAllLines(known.toPath(), StandardCharsets.UTF_8)) {
                String[] c = line.split("\t", -1);
                if (c.length < 2 || c[0].equals("va")) {
                    continue;
                }
                Address a = toAddr(Long.parseLong(c[0].substring(2), 16));
                String name = "lyrium_" + c[1];

                Function f = getFunctionAt(a);
                if (f == null && getFunctionContaining(a) == null) {
                    // The pool site is mid-function, and a target Ghidra failed to
                    // recognise is exactly the kind of thing the sanity gate should have
                    // caught -- so say so rather than silently labelling raw bytes.
                    f = createFunction(a, name);
                    if (f != null) {
                        created++;
                        println("DaoNames: created a function at " + a + " that analysis had missed");
                    }
                }
                if (f != null) {
                    f.setName(name, SourceType.USER_DEFINED);
                    named++;
                }
                else {
                    currentProgram.getSymbolTable().createLabel(a, name, SourceType.USER_DEFINED);
                    labelled++;
                }
            }
        }
        finally {
            currentProgram.endTransaction(tx, true);
        }
        println("DaoNames: " + named + " functions renamed, " + labelled + " labels added, "
                + created + " functions created");
    }
}
