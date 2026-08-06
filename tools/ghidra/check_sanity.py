#!/usr/bin/env python3
"""Gate the survey on Ghidra agreeing with the bytes lyrium patches at runtime.

emit_known_addresses.py has already proved the header matches the file. So a
disagreement here is Ghidra's: a wrong image base, a wrong processor, or a function
boundary that landed mid-instruction. All three are visible in sanity_meta.tsv.

Exits non-zero if anything fails, so it can gate a shell pipeline.

Usage:
  tools/ghidra/check_sanity.py <out_dir>
"""

from __future__ import annotations

import sys
from pathlib import Path


def read_tsv(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        raise SystemExit(f"missing {path} -- did the Ghidra run actually produce output?")
    lines = path.read_text(encoding="utf-8").splitlines()
    if not lines:
        raise SystemExit(f"{path} is empty")
    header = lines[0].split("\t")
    return [dict(zip(header, line.split("\t"))) for line in lines[1:] if line]


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    out = Path(sys.argv[1])

    expected = {r["va"]: r for r in read_tsv(out / "known_addresses.tsv")}
    observed = {r["va"]: r for r in read_tsv(out / "sanity_bytes.tsv")}

    failures = 0

    for va, want in sorted(expected.items()):
        got = observed.get(va)
        if got is None:
            print(f"FAIL {va} {want['name']}: absent from sanity_bytes.tsv")
            failures += 1
            continue
        # known_addresses carries the recorded prologue, which is shorter than the
        # 16 bytes Ghidra was asked for at the pool site. Compare the prefix.
        want_hex = want["prologue_hex"]
        got_hex = got["bytes"][: len(want_hex)]
        if got_hex != want_hex:
            print(f"FAIL {va} {want['name']}: byte mismatch")
            print(f"       header {want_hex}")
            print(f"       ghidra {got_hex}")
            failures += 1
        elif got["instruction"] == "NO_INSTRUCTION":
            print(f"FAIL {va} {want['name']}: bytes match but Ghidra did not disassemble it")
            failures += 1
        else:
            print(f"ok   {va} {want['name']:<28} {got['instruction']}")

    # The pool site is the one instruction the whole survey is aimed at. Decoding it as
    # anything other than the 850 MB store means the rest of the run is not measuring
    # what it claims to.
    pool = observed.get("0x004b8f30")
    if pool is None:
        print("FAIL 0x004b8f30 missing entirely")
        failures += 1
    elif "35200000" not in pool["instruction"].lower():
        print(f"FAIL 0x004b8f30 does not carry the 850 MB immediate: {pool['instruction']}")
        failures += 1

    imports = read_tsv(out / "sanity_imports.tsv")
    for r in imports:
        if int(r["refs_to_iat"]) == 0:
            print(f"FAIL {r['name']} at {r['iat_va']}: zero references, seed resolution is wrong")
            failures += 1
        else:
            print(f"ok   {r['iat_va']} {r['name']:<28} {r['refs_to_iat']} refs")

    print()
    if failures:
        print(f"{failures} failure(s). Do not run the survey; read sanity_meta.tsv.")
        return 1
    print(f"{len(expected)} addresses and {len(imports)} imports agree. Survey may proceed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
