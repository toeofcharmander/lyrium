// Confirm Ghidra's view of DAOrigins.exe against what the DLL already knows.
//
// Everything downstream assumes Ghidra disassembled the same bytes lyrium patches at
// runtime. This proves it, at the fourteen addresses whose bytes are already verified
// against the file, and it is the gate the rest of the survey runs behind. A mismatch
// here means Ghidra chose the wrong image base, processor or function boundary -- all
// of which are reported in sanity_meta.tsv.
//
// Reads known_addresses.tsv and known_imports.tsv, both produced by
// tools/ghidra/emit_known_addresses.py, so no address is retyped into this file.
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

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;
import ghidra.program.model.symbol.SymbolTable;

public class DaoSanity extends GhidraScript {

    private static final int PROBE_BYTES = 16;

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            printerr("usage: DaoSanity <outDir>");
            return;
        }
        File outDir = new File(args[0]);
        outDir.mkdirs();

        writeMeta(new File(outDir, "sanity_meta.tsv"));
        writeBytes(new File(outDir, "known_addresses.tsv"), new File(outDir, "sanity_bytes.tsv"));
        writeImports(new File(outDir, "known_imports.tsv"), new File(outDir, "sanity_imports.tsv"));
        println("DaoSanity: wrote sanity_meta.tsv, sanity_bytes.tsv, sanity_imports.tsv to " + outDir);
    }

    private void writeMeta(File out) throws Exception {
        try (BufferedWriter w = open(out)) {
            row(w, "key", "value");
            row(w, "language_id", String.valueOf(currentProgram.getLanguageID()));
            row(w, "compiler_spec", String.valueOf(currentProgram.getCompilerSpec().getCompilerSpecID()));
            row(w, "image_base", currentProgram.getImageBase().toString());
            row(w, "executable_format", String.valueOf(currentProgram.getExecutableFormat()));
            row(w, "executable_sha256", String.valueOf(currentProgram.getExecutableSHA256()));
            row(w, "executable_md5", String.valueOf(currentProgram.getExecutableMD5()));
            row(w, "function_count", String.valueOf(currentProgram.getFunctionManager().getFunctionCount()));
            for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
                row(w, "block:" + b.getName(),
                        b.getStart() + ".." + b.getEnd()
                                + " size=" + b.getSize()
                                + " x=" + b.isExecute()
                                + " w=" + b.isWrite()
                                + " init=" + b.isInitialized());
            }
        }
    }

    private void writeBytes(File known, File out) throws Exception {
        if (!known.isFile()) {
            printerr("missing " + known + " -- run tools/ghidra/emit_known_addresses.py first");
            return;
        }
        try (BufferedWriter w = open(out)) {
            row(w, "va", "name", "bytes", "instruction", "is_func_start", "func_va", "func_name");
            for (String line : Files.readAllLines(known.toPath(), StandardCharsets.UTF_8)) {
                String[] c = line.split("\t", -1);
                if (c.length < 2 || c[0].equals("va")) {
                    continue;
                }
                Address a = toAddr(Long.parseLong(c[0].substring(2), 16));
                String bytes;
                try {
                    bytes = hex(getBytes(a, PROBE_BYTES));
                }
                catch (Exception e) {
                    bytes = "UNREADABLE:" + e.getClass().getSimpleName();
                }
                Instruction insn = getInstructionAt(a);
                Function containing = getFunctionContaining(a);
                Function at = getFunctionAt(a);
                row(w, c[0], c[1],
                        bytes,
                        insn == null ? "NO_INSTRUCTION" : insn.toString(),
                        String.valueOf(at != null),
                        containing == null ? "-" : containing.getEntryPoint().toString(),
                        containing == null ? "-" : containing.getName());
            }
        }
    }

    private void writeImports(File known, File out) throws Exception {
        if (!known.isFile()) {
            printerr("missing " + known + " -- run tools/ghidra/emit_known_addresses.py first");
            return;
        }
        SymbolTable st = currentProgram.getSymbolTable();
        try (BufferedWriter w = open(out)) {
            row(w, "name", "iat_va", "symbols_at_iat", "refs_to_iat", "other_symbol_addrs");
            for (String line : Files.readAllLines(known.toPath(), StandardCharsets.UTF_8)) {
                String[] c = line.split("\t", -1);
                if (c.length < 2 || c[0].equals("name")) {
                    continue;
                }
                String name = c[0];
                Address iat = toAddr(Long.parseLong(c[1].substring(2), 16));

                List<String> at = new ArrayList<>();
                for (Symbol s : st.getSymbols(iat)) {
                    at.add(s.getName());
                }

                int refs = 0;
                for (ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(iat); it.hasNext();) {
                    it.next();
                    refs++;
                }

                // Any other place Ghidra decided this symbol lives -- an <EXTERNAL>
                // location or a synthesized thunk. The xref sweep has to seed from all
                // of them, so this is where we find out which ones exist.
                List<String> elsewhere = new ArrayList<>();
                for (Symbol s : st.getGlobalSymbols(name)) {
                    elsewhere.add(s.getAddress().toString());
                }
                for (SymbolIterator it = st.getExternalSymbols(name); it.hasNext();) {
                    elsewhere.add(it.next().getAddress().toString());
                }
                for (SymbolIterator it = st.getSymbols(name); it.hasNext();) {
                    elsewhere.add(it.next().getAddress().toString());
                }

                row(w, name, c[1],
                        at.isEmpty() ? "-" : String.join(",", at),
                        String.valueOf(refs),
                        elsewhere.isEmpty() ? "-" : String.join(",", elsewhere));
            }
        }
    }

    // Windows JVM, WSL consumer: PrintWriter.println would put a CR on the end of every
    // last field and every downstream comparison would fail in a way that looks like data.
    static BufferedWriter open(File f) throws Exception {
        return new BufferedWriter(
                new OutputStreamWriter(new FileOutputStream(f), StandardCharsets.UTF_8));
    }

    static void row(BufferedWriter w, String... cells) throws Exception {
        w.write(String.join("\t", cells));
        w.write("\n");
    }

    static String hex(byte[] b) {
        StringBuilder sb = new StringBuilder(b.length * 2);
        for (byte v : b) {
            sb.append(Character.forDigit((v >> 4) & 0xF, 16)).append(Character.forDigit(v & 0xF, 16));
        }
        return sb.toString();
    }
}
