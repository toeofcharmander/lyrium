#!/usr/bin/env python3
"""Compose candidate dao/targets.h rows, and refuse to emit one that would not hold.

Takes the instruction geometry DaoTargetRow.java produced and turns it into a row,
reading the bytes, the digest and the relocation mask straight out of the PE. Every
invariant tests/unit/targets_table_test.cpp asserts is checked here first, so a row
that reaches the header has already been through the same screen.

The relocation mask comes from the image's own .reloc section rather than from any
disassembly of it. InlineHook::verify skips masked bytes when the image is rebased,
so a mask that is wrong in the optimistic direction turns byte verification -- the
safety mechanism -- into a rubber stamp.

Usage:
  tools/ghidra/emit_target_rows.py <exe> <target_rows.tsv> [--call-only]

--call-only emits rows for functions lyrium calls rather than hooks. Nothing is
patched, so the instruction-boundary and relative-branch rules do not apply and
patch_len becomes the whole prologue -- comparing sixteen bytes instead of five to
nine is strictly better verification, and the only thing that matters for a call.
"""

from __future__ import annotations

import hashlib
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from emit_known_addresses import Pe  # noqa: E402

PROLOGUE_BYTES = 16
JUMP_LENGTH = 5

# Mirrors looks_like_relative_branch() in targets_table_test.cpp. A conservative
# byte screen, not a decoder: the trampoline copies these bytes to another address
# without relocating them, so anything self-relative inside the range retargets.
def looks_like_relative_branch(b: int) -> bool:
    return b in (0xE8, 0xE9, 0xEB, 0xE3) or 0xE0 <= b <= 0xE2 or 0x70 <= b <= 0x7F


def base_relocations(pe: Pe) -> set[int]:
    """RVAs of every dword the loader would fix up if the image were rebased."""
    rva, size = struct.unpack_from("<II", pe._data, pe._opt + 96 + 8 * 5)
    if rva == 0 or size == 0:
        return set()
    out: set[int] = set()
    offset = pe.offset_of_rva(rva)
    end = offset + size
    while offset < end:
        page_rva, block_size = struct.unpack_from("<II", pe._data, offset)
        if block_size < 8:
            break
        for i in range((block_size - 8) // 2):
            entry = struct.unpack_from("<H", pe._data, offset + 8 + 2 * i)[0]
            if entry >> 12 == 3:  # IMAGE_REL_BASED_HIGHLOW
                out.add(page_rva + (entry & 0xFFF))
        offset += block_size
    return out


def choose_patch_len(prologue: bytes, boundaries: list[int]) -> tuple[int, str]:
    """Smallest instruction boundary that can hold a 5-byte jump and is safe to copy."""
    for end in boundaries:
        if end < JUMP_LENGTH:
            continue
        if end > PROLOGUE_BYTES:
            break
        offending = [i for i in range(end) if looks_like_relative_branch(prologue[i])]
        if offending:
            # Not fatal on its own -- a longer patch cannot help, since the byte is
            # already inside every longer range too. Report and stop.
            return 0, (
                f"relative-branch byte {prologue[offending[0]]:#04x} at offset {offending[0]} "
                f"lies inside the smallest usable patch ({end} bytes); this target cannot be "
                "hooked by overwriting its prologue"
            )
        return end, ""
    return 0, "no instruction boundary between 5 and 16 bytes"


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    call_only = "--call-only" in sys.argv[3:]
    pe = Pe(Path(sys.argv[1]))
    relocs = base_relocations(pe)

    rows = Path(sys.argv[2]).read_text(encoding="utf-8").splitlines()
    header = rows[0].split("\t")
    failures = 0
    emitted = []

    for line in rows[1:]:
        if not line:
            continue
        r = dict(zip(header, line.split("\t")))
        name = r["name"]
        if r["is_func_start"] != "true":
            print(f"SKIP {r['va']} {name}: not a function entry point", file=sys.stderr)
            failures += 1
            continue

        va = int(r["va"], 16)
        size = int(r["body_bytes"])
        boundaries = [int(x) for x in r["boundaries"].split(",") if x]
        prologue = pe.read_va(va, PROLOGUE_BYTES)

        if call_only:
            patch_len, why = PROLOGUE_BYTES, ""
        else:
            patch_len, why = choose_patch_len(prologue, boundaries)
        if patch_len == 0:
            print(f"REJECT {r['va']} {name}: {why}", file=sys.stderr)
            failures += 1
            continue

        # A masked byte is a byte verify() will not check, so mask only what the
        # loader would actually rewrite, and only inside the patched range.
        # A HIGHLOW fixup names the first byte of a dword; all four of its bytes move.
        mask = 0
        for i in range(patch_len):
            if (va - pe.image_base + i) in relocs:
                for j in range(i, min(i + 4, patch_len)):
                    mask |= 1 << j

        digest = hashlib.sha256(pe.read_va(va, size)).hexdigest()
        # .size is the one field Ghidra is not authoritative about. Its body is the
        # address set it assigned to the function, which misses a detached tail: it
        # is 9 bytes short on crt_free and 15 on crt_realloc, both of which targets.h
        # records correctly. A short body still hashes consistently, it just covers
        # less of the function than the recorder meant it to.
        #
        # There is no cheap rule that separates the two bad cases from the eleven good
        # ones -- a correct body is followed by 0xCC padding, by the next prologue, or
        # by a jump table, and the short ones are followed by ordinary code. So print
        # what follows and let it be read rather than guessing a verdict.
        trailing = pe.read_va(va + size, 8)

        assert JUMP_LENGTH <= patch_len <= PROLOGUE_BYTES
        assert size >= patch_len, f"{name}: body {size} smaller than patch {patch_len}"
        assert mask >> patch_len == 0

        emitted.append((name, va, size, patch_len, mask, digest, prologue,
                        r["mnemonics"].split(","), trailing))

    for name, va, size, patch_len, mask, digest, prologue, mnemonics, trailing in emitted:
        pretty = ", ".join(f"0x{b:02X}" for b in prologue)
        print(f"""    {{
        .name = "{name}",
        .symbol = "sub_{va:X}",
        .comment = "TODO",
        .address = 0x{va:08x},
        .size = {size},
        .patch_len = {patch_len},
        .reloc_mask = 0x{mask:04X},
        .sha256 = "{digest}",
        .prologue = {{{pretty}}},{"" if not call_only else chr(10) + "        .call_only = true,"}
    }},""")
        print(f"        // first instructions: {' '.join(mnemonics[:6])}")
        print(f"        // 8 bytes after the body: {trailing.hex(' ')}"
              f"{'  (padding, body end looks right)' if set(trailing) == {0xCC} else '  -- read this before trusting .size'}\n")

    print(f"{len(emitted)} row(s) emitted, {failures} rejected", file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
