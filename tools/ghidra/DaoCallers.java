// Walk upward from a set of functions to see who drives them.
//
// Emits both an edge list and an indented tree. The edge list carries the depth, so a
// shallower view can be re-cut with awk instead of by re-running Ghidra. Cycles are
// marked and not followed -- recursion through a CRT-adjacent function would otherwise
// never terminate.
//
//@category Lyrium

import java.io.BufferedWriter;
import java.io.File;
import java.io.FileOutputStream;
import java.io.OutputStreamWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.Deque;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;
import java.util.TreeMap;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class DaoCallers extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 3) {
            printerr("usage: DaoCallers <outDir> <depth> <seedFile>");
            return;
        }
        File outDir = new File(args[0]);
        outDir.mkdirs();
        int maxDepth = Integer.parseInt(args[1]);
        File seedFile = new File(args[2]);
        if (!seedFile.isFile()) {
            printerr("seed file not found: " + seedFile);
            return;
        }

        TreeMap<Address, Function> seeds = new TreeMap<>();
        for (String line : Files.readAllLines(seedFile.toPath(), StandardCharsets.UTF_8)) {
            String[] c = line.trim().split("\t", -1);
            if (c.length == 0 || !c[0].startsWith("0")) {
                continue;
            }
            Address a = toAddr(Long.parseLong(c[0].replaceFirst("^0[xX]", ""), 16));
            Function f = getFunctionContaining(a);
            if (f == null) {
                printerr("no function contains " + a);
                continue;
            }
            seeds.put(f.getEntryPoint(), f);
        }
        if (seeds.isEmpty()) {
            printerr("no seeds resolved");
            return;
        }

        Set<Address> visited = new LinkedHashSet<>(seeds.keySet());
        List<Edge> edges = new ArrayList<>();
        Deque<Step> queue = new ArrayDeque<>();
        for (Function f : seeds.values()) {
            queue.add(new Step(f, 0));
        }

        while (!queue.isEmpty()) {
            if (monitor.isCancelled()) {
                break;
            }
            Step step = queue.poll();
            if (step.depth >= maxDepth) {
                continue;
            }
            List<Function> callers = new ArrayList<>(step.function.getCallingFunctions(monitor));
            callers.sort(Comparator.comparing(Function::getEntryPoint));
            for (Function caller : callers) {
                boolean cycle = !visited.add(caller.getEntryPoint());
                edges.add(new Edge(step.depth + 1, step.function, caller,
                        caller.getCallingFunctions(monitor).size(), cycle));
                if (!cycle) {
                    queue.add(new Step(caller, step.depth + 1));
                }
            }
        }

        try (BufferedWriter w = open(new File(outDir, "callers_edges.tsv"))) {
            row(w, "depth", "callee_va", "callee_name", "caller_va", "caller_name",
                    "caller_ncallers", "cycle");
            for (Edge e : edges) {
                row(w, String.valueOf(e.depth),
                        e.callee.getEntryPoint().toString(), e.callee.getName(),
                        e.caller.getEntryPoint().toString(), e.caller.getName(),
                        String.valueOf(e.callerCallers), String.valueOf(e.cycle));
            }
        }

        try (BufferedWriter w = open(new File(outDir, "callers_tree.txt"))) {
            for (Function f : seeds.values()) {
                w.write(f.getEntryPoint() + "  " + f.getName() + "\n");
                writeTree(w, edges, f.getEntryPoint(), 1, maxDepth);
                w.write("\n");
            }
        }

        println("DaoCallers: " + edges.size() + " edges from " + seeds.size()
                + " seeds, depth " + maxDepth);
    }

    private void writeTree(BufferedWriter w, List<Edge> edges, Address callee, int depth, int maxDepth)
            throws Exception {
        if (depth > maxDepth) {
            return;
        }
        for (Edge e : edges) {
            if (e.depth != depth || !e.callee.getEntryPoint().equals(callee)) {
                continue;
            }
            w.write("  ".repeat(depth) + "<- " + e.caller.getEntryPoint() + "  " + e.caller.getName()
                    + (e.cycle ? "  [cycle]" : "") + "\n");
            if (!e.cycle) {
                writeTree(w, edges, e.caller.getEntryPoint(), depth + 1, maxDepth);
            }
        }
    }

    private static final class Step {
        final Function function;
        final int depth;

        Step(Function function, int depth) {
            this.function = function;
            this.depth = depth;
        }
    }

    private static final class Edge {
        final int depth;
        final Function callee;
        final Function caller;
        final int callerCallers;
        final boolean cycle;

        Edge(int depth, Function callee, Function caller, int callerCallers, boolean cycle) {
            this.depth = depth;
            this.callee = callee;
            this.caller = caller;
            this.callerCallers = callerCallers;
            this.cycle = cycle;
        }
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
