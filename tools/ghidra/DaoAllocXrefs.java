// Find every place DAOrigins.exe reaches an allocation primitive.
//
// Seeds are unioned from three representations, because a PE import reference may land
// on the import address table slot, on an <EXTERNAL> location, or on a synthesized
// thunk, and which one you get depends on the loader options and on what analysis
// decided. Missing one produces an empty report that looks like an answer.
//
// The statically linked CRT is seeded by ADDRESS only -- there are no symbols in this
// binary, so malloc is FUN_008e47aa and always will be.
//
// Outputs, all TSV, all raw enough to re-cut with awk without re-running Ghidra:
//   alloc_targets.tsv        per seed: how it resolved, how many references. The smoke
//                            detector -- zero references on HeapAlloc means the seed
//                            logic missed a representation, not that the game is chaste.
//   alloc_xrefs.tsv          one row per call site, function metrics denormalised inline
//   func_metrics.tsv         one row per function that contains at least one site
//   wrappers.tsv             func_metrics with a default wrapper rule applied
//   alloc_args_peephole.tsv  constants staged in the 24 instructions before each site
//
//@category Lyrium

import java.io.BufferedWriter;
import java.io.File;
import java.io.FileOutputStream;
import java.io.OutputStreamWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;
import java.util.TreeSet;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;
import ghidra.program.model.symbol.SymbolTable;

public class DaoAllocXrefs extends GhidraScript {

    /** How far back to look for arguments being staged. Covers a six-argument setup. */
    private static final int PEEPHOLE_DEPTH = 24;

    /** Default wrapper rule. Emitted as a column, not applied destructively. */
    private static final int WRAPPER_MAX_INSN = 32;

    private final Map<String, Set<Address>> seeds = new LinkedHashMap<>();
    private final Map<Address, String> apiOfSeed = new LinkedHashMap<>();
    private final Set<Address> seedFunctionEntries = new LinkedHashSet<>();

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            printerr("usage: DaoAllocXrefs <outDir> [extraSeedFile]");
            return;
        }
        File outDir = new File(args[0]);
        outDir.mkdirs();

        loadImportSeeds(new File(outDir, "known_imports.tsv"));
        loadCrtSeeds(new File(outDir, "known_addresses.tsv"));
        if (args.length > 1) {
            loadExtraSeeds(new File(args[1]));
        }
        expandThunks();
        indexSeeds();

        List<Site> sites = collectSites(new File(outDir, "alloc_targets.tsv"));
        Map<Address, Metrics> metrics = measureFunctions(sites);
        writeXrefs(new File(outDir, "alloc_xrefs.tsv"), sites, metrics);
        writeMetrics(new File(outDir, "func_metrics.tsv"), new File(outDir, "wrappers.tsv"), metrics);
        writePeephole(new File(outDir, "alloc_args_peephole.tsv"), sites);

        println("DaoAllocXrefs: " + sites.size() + " call sites across "
                + metrics.size() + " functions, from " + apiOfSeed.size() + " seeds");
    }

    // ---- seeds -------------------------------------------------------------------

    private void loadImportSeeds(File known) throws Exception {
        if (!known.isFile()) {
            printerr("missing " + known + " -- run tools/ghidra/emit_known_addresses.py first");
            return;
        }
        SymbolTable st = currentProgram.getSymbolTable();
        for (String line : Files.readAllLines(known.toPath(), StandardCharsets.UTF_8)) {
            String[] c = line.split("\t", -1);
            if (c.length < 2 || c[0].equals("name")) {
                continue;
            }
            String api = c[0];
            Set<Address> set = seeds.computeIfAbsent(api, k -> new LinkedHashSet<>());
            set.add(toAddr(Long.parseLong(c[1].substring(2), 16)));
            for (Symbol s : st.getGlobalSymbols(api)) {
                set.add(s.getAddress());
            }
            for (SymbolIterator it = st.getExternalSymbols(api); it.hasNext();) {
                set.add(it.next().getAddress());
            }
            for (SymbolIterator it = st.getSymbols(api); it.hasNext();) {
                set.add(it.next().getAddress());
            }
        }
    }

    private void loadCrtSeeds(File known) throws Exception {
        if (!known.isFile()) {
            return;
        }
        for (String line : Files.readAllLines(known.toPath(), StandardCharsets.UTF_8)) {
            String[] c = line.split("\t", -1);
            if (c.length < 2 || !c[1].startsWith("crt_")) {
                continue;
            }
            seeds.computeIfAbsent(c[1], k -> new LinkedHashSet<>())
                    .add(toAddr(Long.parseLong(c[0].substring(2), 16)));
        }
    }

    /** Second-pass seeding: feed wrappers.tsv back in to find who really allocates. */
    private void loadExtraSeeds(File file) throws Exception {
        if (!file.isFile()) {
            printerr("extra seed file not found: " + file);
            return;
        }
        for (String line : Files.readAllLines(file.toPath(), StandardCharsets.UTF_8)) {
            String[] c = line.split("\t", -1);
            if (c.length < 2 || !c[0].startsWith("0x")) {
                continue;
            }
            seeds.computeIfAbsent("wrapper:" + c[1], k -> new LinkedHashSet<>())
                    .add(toAddr(Long.parseLong(c[0].substring(2), 16)));
        }
    }

    /** A thunk to a seed is a seed. getThunkedFunction(true) already resolves chains. */
    private void expandThunks() {
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            if (monitor.isCancelled()) {
                break;
            }
            if (!f.isThunk()) {
                continue;
            }
            Function target = f.getThunkedFunction(true);
            if (target == null) {
                continue;
            }
            for (Map.Entry<String, Set<Address>> e : seeds.entrySet()) {
                if (e.getValue().contains(target.getEntryPoint())) {
                    e.getValue().add(f.getEntryPoint());
                }
            }
        }
    }

    private void indexSeeds() {
        for (Map.Entry<String, Set<Address>> e : seeds.entrySet()) {
            for (Address a : e.getValue()) {
                apiOfSeed.putIfAbsent(a, e.getKey());
                Function f = getFunctionAt(a);
                if (f != null) {
                    seedFunctionEntries.add(f.getEntryPoint());
                }
            }
        }
    }

    // ---- sites -------------------------------------------------------------------

    private List<Site> collectSites(File targetsOut) throws Exception {
        List<Site> sites = new ArrayList<>();
        try (BufferedWriter w = open(targetsOut)) {
            row(w, "api", "seed_va", "kind", "symbol", "nrefs", "nrefs_in_code");
            for (Map.Entry<String, Set<Address>> e : seeds.entrySet()) {
                for (Address seed : e.getValue()) {
                    if (monitor.isCancelled()) {
                        break;
                    }
                    int total = 0;
                    int inCode = 0;
                    for (ReferenceIterator it =
                            currentProgram.getReferenceManager().getReferencesTo(seed); it.hasNext();) {
                        Reference r = it.next();
                        total++;
                        Address from = r.getFromAddress();
                        MemoryBlock block = currentProgram.getMemory().getBlock(from);
                        if (block == null || !block.isExecute()) {
                            continue;
                        }
                        inCode++;
                        sites.add(new Site(e.getKey(), seed, from,
                                String.valueOf(r.getReferenceType()),
                                getInstructionAt(from), getFunctionContaining(from)));
                    }
                    Symbol[] at = currentProgram.getSymbolTable().getSymbols(seed);
                    Function f = getFunctionAt(seed);
                    row(w, e.getKey(), seed.toString(),
                            f == null ? "data" : (f.isThunk() ? "thunk" : (f.isExternal() ? "external" : "code")),
                            at.length == 0 ? "-" : at[0].getName(),
                            String.valueOf(total), String.valueOf(inCode));
                }
            }
        }
        return sites;
    }

    // ---- function metrics --------------------------------------------------------

    private Map<Address, Metrics> measureFunctions(List<Site> sites) throws Exception {
        Map<Address, Metrics> out = new TreeMap<>();
        for (Site s : sites) {
            if (s.function == null) {
                continue;
            }
            Address entry = s.function.getEntryPoint();
            Metrics m = out.get(entry);
            if (m == null) {
                m = measure(s.function);
                out.put(entry, m);
            }
            m.apis.add(s.api);
        }
        return out;
    }

    private Metrics measure(Function f) throws Exception {
        Metrics m = new Metrics();
        m.function = f;
        m.bodyBytes = f.getBody().getNumAddresses();

        // A backward flow that stays inside the body is a loop. Cheap, and it is the
        // signal that separates a real allocation site from a one-line wrapper.
        for (Instruction insn : currentProgram.getListing().getInstructions(f.getBody(), true)) {
            m.instructions++;
            if (m.hasLoop) {
                continue;
            }
            for (Address flow : insn.getFlows()) {
                if (flow.compareTo(insn.getAddress()) <= 0 && f.getBody().contains(flow)) {
                    m.hasLoop = true;
                    break;
                }
            }
        }

        for (Function callee : f.getCalledFunctions(monitor)) {
            m.callees++;
            if (!seedFunctionEntries.contains(callee.getEntryPoint())) {
                m.nonAllocCallees++;
            }
        }
        m.callers = f.getCallingFunctions(monitor).size();
        return m;
    }

    // ---- output ------------------------------------------------------------------

    private void writeXrefs(File out, List<Site> sites, Map<Address, Metrics> metrics) throws Exception {
        sites.sort(Comparator.comparing((Site s) -> s.api).thenComparing(s -> s.callSite));
        try (BufferedWriter w = open(out)) {
            row(w, "api", "seed_va", "ref_type", "call_site_va", "mnemonic", "instruction",
                    "func_va", "func_name", "func_bytes", "func_ninsn", "func_ncallees",
                    "func_ncallers", "func_is_thunk", "func_has_loop");
            for (Site s : sites) {
                Metrics m = s.function == null ? null : metrics.get(s.function.getEntryPoint());
                row(w, s.api, s.seed.toString(), s.refType, s.callSite.toString(),
                        s.instruction == null ? "-" : s.instruction.getMnemonicString(),
                        s.instruction == null ? "-" : s.instruction.toString(),
                        s.function == null ? "-" : s.function.getEntryPoint().toString(),
                        s.function == null ? "-" : s.function.getName(),
                        m == null ? "-" : String.valueOf(m.bodyBytes),
                        m == null ? "-" : String.valueOf(m.instructions),
                        m == null ? "-" : String.valueOf(m.callees),
                        m == null ? "-" : String.valueOf(m.callers),
                        s.function == null ? "-" : String.valueOf(s.function.isThunk()),
                        m == null ? "-" : String.valueOf(m.hasLoop));
            }
        }
    }

    private void writeMetrics(File metricsOut, File wrappersOut, Map<Address, Metrics> metrics)
            throws Exception {
        try (BufferedWriter w = open(metricsOut); BufferedWriter ww = open(wrappersOut)) {
            String[] header = {"func_va", "func_name", "body_bytes", "ninsn", "ncallees",
                    "nonalloc_callees", "ncallers", "is_thunk", "has_loop", "apis"};
            row(w, header);
            row(ww, header);
            for (Map.Entry<Address, Metrics> e : metrics.entrySet()) {
                Metrics m = e.getValue();
                String[] cells = {
                        e.getKey().toString(),
                        m.function.getName(),
                        String.valueOf(m.bodyBytes),
                        String.valueOf(m.instructions),
                        String.valueOf(m.callees),
                        String.valueOf(m.nonAllocCallees),
                        String.valueOf(m.callers),
                        String.valueOf(m.function.isThunk()),
                        String.valueOf(m.hasLoop),
                        String.join(",", m.apis)};
                row(w, cells);
                if (m.isWrapper()) {
                    row(ww, cells);
                }
            }
        }
    }

    private void writePeephole(File out, List<Site> sites) throws Exception {
        try (BufferedWriter w = open(out)) {
            row(w, "call_site_va", "api", "distance", "mnemonic", "dest", "imm_hex", "imm_dec",
                    "instruction");
            for (Site s : sites) {
                if (s.instruction == null || s.function == null) {
                    continue;
                }
                Instruction prev = s.instruction;
                for (int d = 1; d <= PEEPHOLE_DEPTH; d++) {
                    prev = prev.getPrevious();
                    if (prev == null || !s.function.getBody().contains(prev.getAddress())) {
                        break;
                    }
                    String mnemonic = prev.getMnemonicString();
                    Scalar imm = null;
                    String dest = "-";
                    if (mnemonic.equals("PUSH")) {
                        imm = prev.getScalar(0);
                    }
                    else if (mnemonic.equals("MOV") && prev.getNumOperands() >= 2) {
                        imm = prev.getScalar(1);
                        dest = prev.getDefaultOperandRepresentation(0);
                    }
                    if (imm == null) {
                        continue;
                    }
                    long v = imm.getUnsignedValue();
                    row(w, s.callSite.toString(), s.api, String.valueOf(d), mnemonic, dest,
                            "0x" + Long.toHexString(v), String.valueOf(v), prev.toString());
                }
            }
        }
    }

    // ---- plumbing ----------------------------------------------------------------

    private static final class Site {
        final String api;
        final Address seed;
        final Address callSite;
        final String refType;
        final Instruction instruction;
        final Function function;

        Site(String api, Address seed, Address callSite, String refType, Instruction insn, Function f) {
            this.api = api;
            this.seed = seed;
            this.callSite = callSite;
            this.refType = refType;
            this.instruction = insn;
            this.function = f;
        }
    }

    private static final class Metrics {
        Function function;
        long bodyBytes;
        int instructions;
        int callees;
        int nonAllocCallees;
        int callers;
        boolean hasLoop;
        final Set<String> apis = new TreeSet<>();

        boolean isWrapper() {
            return function.isThunk()
                    || (instructions <= WRAPPER_MAX_INSN && nonAllocCallees == 0 && !hasLoop);
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
