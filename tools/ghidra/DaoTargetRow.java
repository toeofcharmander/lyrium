// Emit the instruction geometry needed to record a new row in dao/targets.h.
//
// Only the parts that need a disassembler come from here: where the function
// starts, how long its body is, and where the instruction boundaries fall inside
// the first 16 bytes. patch_len has to land on a boundary -- a jump written across
// the middle of an instruction leaves a tail the trampoline then executes as
// garbage -- and nothing but a real decoder knows where those are.
//
// The bytes, the digest, the relocation mask and the final row are composed by
// tools/ghidra/emit_target_rows.py, which reads the PE directly. Splitting it that
// way keeps the relocation data coming from .reloc rather than from Ghidra's
// interpretation of it.
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

public class DaoTargetRow extends GhidraScript {

    /** Matches lyrium::dao::prologue_bytes. */
    private static final int PROLOGUE_BYTES = 16;

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            printerr("usage: DaoTargetRow <outDir> <addressListFile>");
            return;
        }
        File outDir = new File(args[0]);
        outDir.mkdirs();
        File list = new File(args[1]);
        if (!list.isFile()) {
            printerr("address list not found: " + list);
            return;
        }

        try (BufferedWriter w = open(new File(outDir, "target_rows.tsv"))) {
            row(w, "va", "name", "func_va", "is_func_start", "body_bytes", "boundaries", "mnemonics");
            for (String line : Files.readAllLines(list.toPath(), StandardCharsets.UTF_8)) {
                String[] c = line.trim().split("\t", -1);
                if (c.length == 0 || !c[0].startsWith("0")) {
                    continue;
                }
                Address a = toAddr(Long.parseLong(c[0].replaceFirst("^0[xX]", ""), 16));
                String name = c.length > 1 ? c[1] : "-";
                Function f = getFunctionAt(a);
                if (f == null) {
                    Function containing = getFunctionContaining(a);
                    printerr(c[0] + " is not a function entry point"
                            + (containing == null ? " and is in no function" : ", it is inside " + containing.getName()));
                    row(w, c[0], name, containing == null ? "-" : containing.getEntryPoint().toString(),
                            "false", "-", "-", "-");
                    continue;
                }

                // Instruction boundaries as offsets from the entry, while still inside
                // the window that prologue[] records.
                List<String> offsets = new ArrayList<>();
                List<String> mnemonics = new ArrayList<>();
                Instruction insn = getInstructionAt(a);
                long offset = 0;
                while (insn != null && offset < PROLOGUE_BYTES) {
                    offset += insn.getLength();
                    offsets.add(String.valueOf(offset));
                    mnemonics.add(insn.getMnemonicString());
                    insn = insn.getNext();
                    if (insn != null && !f.getBody().contains(insn.getAddress())) {
                        break;
                    }
                }
                row(w, c[0], name, f.getEntryPoint().toString(), "true",
                        String.valueOf(f.getBody().getNumAddresses()),
                        String.join(",", offsets), String.join(",", mnemonics));
            }
        }
        println("DaoTargetRow: wrote target_rows.tsv to " + outDir);
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
