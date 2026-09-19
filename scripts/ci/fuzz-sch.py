#!/usr/bin/env python3
"""
Mutation fuzzer for the qucs-s schematic loader and netlister.

Takes the shipped example schematics (and, one time in three, the dataset
shipped next to one), damages them the way corrupted saves, hand edits and
files from newer versions do (truncation, dropped or duplicated lines,
unbalanced quotes and brackets, absurd numbers, missing section
terminators, random bytes) and runs the headless CLI modes of qucs-s on
every mutant:

    qucs-s -n --ngspice -i mutant.sch -o mutant.net     (load + netlist)
    qucs-s -p -i mutant.sch -o mutant.png               (load + render)

A mutant may be rejected with an error; it must never crash, trip a
sanitizer or hang. Every finding is kept under <out-dir>/findings/ with
the mutant directory (schematic, dataset, log) and a one-line signature,
and the run exits non-zero. Re-run one with --reproduce <that .sch>.

The random sequence is seeded, so a given (seed, count, example set) is
reproducible: re-run with the seed printed in the summary.

Usage:
    fuzz-sch.py <qucs-s-executable> <examples-dir> <out-dir>
                [--count N] [--seed S] [--timeout SEC] [--modes n,p]
                [--extra DIR ...] [--keep] [--reproduce mutant.sch]
"""
import argparse
import hashlib
import os
import random
import re
import shutil
import subprocess
import sys
from pathlib import Path

SANITIZER_RE = re.compile(r"ERROR: (Address|UndefinedBehavior|Leak)Sanitizer|runtime error:")
CRASH_RE = re.compile(r"\*\*\* qucs-s crashed|Invariant violated|ASSERT failure|ASSERT: |Assertion failed")
# UBSan findings that wrap silently in practice; reported as warnings, they
# do not fail the run (the smoke suites treat UBSan the same way).
ARITHMETIC_RE = re.compile(r"runtime error: (signed integer overflow|unsigned integer overflow|"
                           r"shift exponent|left shift|negation of|.* is outside the range of representable values)")

NUMBER_RE = re.compile(r"(?<![\w.])-?\d+(?:\.\d+)?(?:[eE][-+]?\d+)?(?![\w.])")
QUOTED_RE = re.compile(r'"[^"\n]*"')
OPEN_TAG_RE = re.compile(r"^\s*<([A-Za-z_][\w]*)", re.M)
CLOSE_TAG_RE = re.compile(r"^\s*</\w+>\s*$", re.M)

HOSTILE_NUMBERS = ["-1", "0", "2147483647", "-2147483648", "4294967295",
                   "99999999999999999999", "1e308", "-1e308", "1e-320",
                   "nan", "inf", "0x7fffffff", "1.5", "-0", ""]
KNOWN_TYPES = ["R", "C", "L", "GND", "Sub", "SpLib", "Eqn", "SPICE", "Lib",
               "Vdc", "Idc", "Diode", "_BJT", "MOSFET", ".AC", ".TR", ".DC",
               ".SW", ".SP", "Port", "Pac", "VProbe", "IProbe", "Tab", "Rect",
               "Smith", "Polar", "Curve", "Time", "Truth", "PS", "Rect3D",
               "Line", "Rectangle", "Ellipse", "Arrow", "Text", ".ID", ".PortSym"]


# --------------------------------------------------------------------------
# mutation operators: each takes (text, rng) and returns the mutated text
def m_truncate(t, r):
    return t[: r.randrange(len(t) + 1)] if t else t


def m_drop_line(t, r):
    lines = t.split("\n")
    if len(lines) > 1:
        del lines[r.randrange(len(lines))]
    return "\n".join(lines)


def m_dup_line(t, r):
    lines = t.split("\n")
    i = r.randrange(len(lines))
    lines.insert(i, lines[i])
    return "\n".join(lines)


def m_swap_lines(t, r):
    lines = t.split("\n")
    if len(lines) > 2:
        i, j = r.randrange(len(lines)), r.randrange(len(lines))
        lines[i], lines[j] = lines[j], lines[i]
    return "\n".join(lines)


def m_hostile_number(t, r):
    nums = list(NUMBER_RE.finditer(t))
    if not nums:
        return t
    m = r.choice(nums)
    return t[: m.start()] + r.choice(HOSTILE_NUMBERS) + t[m.end():]


def m_unbalance_quote(t, r):
    qs = [i for i, c in enumerate(t) if c == '"']
    if not qs:
        return t
    i = r.choice(qs)
    return t[:i] + t[i + 1:]


def m_empty_quoted(t, r):
    qs = list(QUOTED_RE.finditer(t))
    if not qs:
        return t
    m = r.choice(qs)
    return t[: m.start()] + '""' + t[m.end():]


def m_drop_field(t, r):
    lines = t.split("\n")
    cands = [i for i, l in enumerate(lines) if l.lstrip().startswith("<") and " " in l]
    if not cands:
        return t
    i = r.choice(cands)
    parts = lines[i].split(" ")
    if len(parts) > 2:
        del parts[r.randrange(1, len(parts))]
    lines[i] = " ".join(parts)
    return "\n".join(lines)


def m_break_bracket(t, r):
    lines = t.split("\n")
    cands = [i for i, l in enumerate(lines) if l.rstrip().endswith(">")]
    if not cands:
        return t
    i = r.choice(cands)
    l = lines[i].rstrip()
    lines[i] = r.choice([l[:-1], l + ">", l.replace("<", ">", 1), l[:-1] + "<"])
    return "\n".join(lines)


def m_drop_close_tag(t, r):
    tags = list(CLOSE_TAG_RE.finditer(t))
    if not tags:
        return t
    m = r.choice(tags)
    return t[: m.start()] + t[m.end():]


def m_random_bytes(t, r):
    i = r.randrange(len(t) + 1)
    junk = bytes(r.randrange(256) for _ in range(r.randrange(1, 40)))
    return t[:i] + junk.decode("latin-1") + t[i:]


def m_change_type(t, r):
    tags = list(OPEN_TAG_RE.finditer(t))
    if not tags:
        return t
    m = r.choice(tags)
    return t[: m.start(1)] + r.choice(KNOWN_TYPES) + t[m.end(1):]


def m_version(t, r):
    return re.sub(r"<Qucs Schematic [^>]*>",
                  r.choice(["<Qucs Schematic 99.9.9>", "<Qucs Schematic >",
                            "<Qucs Schematic 0.0.0>", "<Qucs Schematic 1.2.3.4.5>",
                            "<Qucs Schematic abc>", "<Qucs Schematic 26.1.1"]),
                  t, count=1)


def m_huge_line(t, r):
    lines = t.split("\n")
    i = r.randrange(len(lines))
    lines[i] = lines[i] + (" " + r.choice(['"x"', "1", "<", '"']) ) * r.randrange(1000, 20000)
    return "\n".join(lines)


def m_bit_flip(t, r):
    b = bytearray(t.encode("latin-1", "replace"))
    if not b:
        return t
    for _ in range(r.randrange(1, 6)):
        i = r.randrange(len(b))
        b[i] ^= 1 << r.randrange(8)
    return b.decode("latin-1")


def m_deep_nesting(t, r):
    # Subcircuit that includes itself: a cycle the netlister must survive.
    return t.replace("<Components>", "<Components>\n  <Sub SUB1 1 0 0 0 0 0 0 \"SELF.sch\" 1>", 1)


MUTATORS = [m_truncate, m_drop_line, m_dup_line, m_swap_lines, m_hostile_number,
            m_hostile_number, m_unbalance_quote, m_empty_quoted, m_drop_field,
            m_drop_field, m_break_bracket, m_drop_close_tag, m_random_bytes,
            m_change_type, m_change_type, m_version, m_huge_line, m_bit_flip,
            m_deep_nesting]


def mutate(text, rng):
    ops = []
    for _ in range(rng.choice([1, 1, 2, 2, 3])):
        op = rng.choice(MUTATORS)
        text = op(text, rng)
        ops.append(op.__name__[2:])
    return text, "+".join(ops)


# --------------------------------------------------------------------------
def is_arithmetic_only(log):
    """True when every sanitizer line in the log is a silent-wrap arithmetic report."""
    lines = [l for l in log.splitlines() if SANITIZER_RE.search(l)]
    return bool(lines) and all(ARITHMETIC_RE.search(l) for l in lines)


def signature(log, rc):
    """Short, stable description of a failure for de-duplication."""
    for line in log.splitlines():
        if "ERROR: AddressSanitizer" in line or "ERROR: UndefinedBehaviorSanitizer" in line:
            return re.sub(r"0x[0-9a-f]+|pc |bp |sp |T\d+", "", line.split("==")[-1]).strip()
        if "runtime error:" in line and not ARITHMETIC_RE.search(line):
            return line.split("runtime error:")[-1].strip()[:120]
        if "*** qucs-s crashed" in line:
            return line.strip()
        if "Invariant violated" in line:
            return line.split("Invariant violated:")[-1].strip()[:120]
        if "ASSERT" in line or "Assertion failed" in line:
            return re.sub(r", file .*", "", line.split("Fatal: ")[-1]).strip()[:140]
    if rc == "timeout":
        return "hang (timeout)"
    if isinstance(rc, int) and rc < 0:
        return f"killed by signal {-rc}"
    return f"exit status {rc}"


def frame_hint(log):
    """First in-tree frame of a sanitizer backtrace, if any."""
    for line in log.splitlines():
        m = re.search(r"#\d+ .* in (\S+) .*?(qucs[^ :]*\.cpp):(\d+)", line)
        if m:
            return f"{m.group(1)} at {m.group(2)}:{m.group(3)}"
    return ""


def run_one(qucs, mode, sch, out_base, timeout):
    if mode == "n":
        cmd = [qucs, "-n", "--ngspice", "-i", str(sch), "-o", f"{out_base}.net"]
    else:
        cmd = [qucs, "-p", "-i", str(sch), "-o", f"{out_base}.png"]
    env = dict(os.environ)
    env.setdefault("QT_QPA_PLATFORM", "offscreen")
    # handle_abort: a backtrace for Qt's own assertions (Debug builds) too.
    env.setdefault("ASAN_OPTIONS", "detect_leaks=0:abort_on_error=0:handle_abort=1:symbolize=1:allocator_may_return_null=1")
    env.setdefault("UBSAN_OPTIONS", "print_stacktrace=1")
    try:
        p = subprocess.run(cmd, env=env, capture_output=True, timeout=timeout)
        log = (p.stdout + p.stderr).decode("utf-8", "replace")
        rc = p.returncode
    except subprocess.TimeoutExpired as e:
        log = ((e.stdout or b"") + (e.stderr or b"")).decode("utf-8", "replace")
        rc = "timeout"
    bad = rc == "timeout" or (isinstance(rc, int) and (rc < 0 or rc >= 128)) \
        or CRASH_RE.search(log) or (SANITIZER_RE.search(log) and not is_arithmetic_only(log))
    return bool(bad), rc, log


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("qucs")
    ap.add_argument("examples")
    ap.add_argument("out")
    ap.add_argument("--count", type=int, default=200, help="number of mutants (default 200)")
    ap.add_argument("--seed", type=int, default=1, help="random seed (default 1)")
    ap.add_argument("--timeout", type=float, default=60, help="seconds per run (default 60)")
    ap.add_argument("--modes", default="n,p", help="comma-separated: n (netlist), p (render)")
    ap.add_argument("--reproduce", help="run the modes on this file instead of generating mutants")
    ap.add_argument("--keep", action="store_true", help="keep mutants that passed")
    ap.add_argument("--data-share", type=float, default=1 / 3, metavar="P",
                    help="share of mutants drawn from schematics that have a dataset; half of "
                         "those damage the dataset (default 1/3)")
    ap.add_argument("--extra", action="append", default=[], metavar="DIR",
                    help="add the .sch files under DIR to the pool (e.g. the simulate suite's "
                         "work directory, whose schematics have datasets next to them)")
    args = ap.parse_args()

    out = Path(args.out)
    mutants = out / "mutants"
    findings = out / "findings"
    for d in (mutants, findings):
        d.mkdir(parents=True, exist_ok=True)
    modes = [m for m in args.modes.split(",") if m]

    if args.reproduce:
        sch = Path(args.reproduce)
        failed = False
        for mode in modes:
            bad, rc, log = run_one(args.qucs, mode, sch, out / f"repro-{mode}", args.timeout)
            (out / f"repro-{mode}.log").write_text(log)
            print(f"{'FAIL' if bad else 'ok  '} {mode} rc={rc} {signature(log, rc) if bad else ''}")
            failed |= bool(bad)
        return 1 if failed else 0

    examples = sorted(p for p in Path(args.examples).rglob("*.sch")
                      if p.is_file() and "OpenVAF" not in p.parts)
    for extra in args.extra:
        examples += sorted(p for p in Path(extra).rglob("*.sch") if p.is_file())
    if not examples:
        print("no example schematics found", file=sys.stderr)
        return 2
    # Datasets sit next to their schematic as <name>.dat or, converted from a
    # simulator's output, <name>.dat.<simulator>.
    def datasets(sch):
        return sorted(p for p in sch.parent.glob(sch.stem + ".dat*") if p.is_file())
    with_data = [p for p in examples if datasets(p)]
    rng = random.Random(args.seed)
    print(f"== fuzz-sch: {args.count} mutants of {len(examples)} examples "
          f"({len(with_data)} with a dataset), seed {args.seed}, modes {modes}")

    total_runs = 0
    arithmetic = 0
    seen = {}
    for i in range(args.count):
        # Datasets and display files travel with the schematic so that the
        # render mode also exercises the dataset -> diagram path. One mutant
        # in three (--data-share) takes a schematic that has a dataset, and
        # half of those damage the dataset instead of the schematic.
        if with_data and rng.random() < args.data_share:
            src = rng.choice(with_data)
            victim_src = rng.choice(datasets(src)) if rng.randrange(2) == 0 else src
        else:
            src = rng.choice(examples)
            victim_src = src
        # Each mutant lives in its own directory under the example's own file
        # names, so that the schematic's DataSet/DataDisplay properties
        # resolve to the copies (mutated or pristine) next to it.
        stem = f"m{i:05d}-{src.stem[:24]}"
        mdir = mutants / stem
        mdir.mkdir(exist_ok=True)
        for side in [src] + datasets(src) + [src.with_suffix(".dpl")]:
            if side.exists():
                shutil.copy(side, mdir / side.name)
        sch = mdir / src.name
        victim = mdir / victim_src.name
        mutated, ops = mutate(victim.read_text(encoding="utf-8", errors="replace"), rng)
        victim.write_text(mutated, encoding="latin-1", errors="replace")
        ops = f"{'sch' if victim_src == src else 'dat'}:{ops}"
        (mdir / "mutant.ops").write_text(f"{src}\n{ops}\n")

        failed_here = False
        for mode in modes:
            total_runs += 1
            bad, rc, log = run_one(args.qucs, mode, sch, mdir / "out", args.timeout)
            if not bad:
                if is_arithmetic_only(log):
                    arithmetic += 1
                    (findings / "arithmetic.txt").open("a").write(
                        f"{stem} [{mode}] {ARITHMETIC_RE.search(log).group(0)}\n")
                continue
            failed_here = True
            sig = signature(log, rc)
            key = hashlib.sha1(f"{mode}|{sig}|{frame_hint(log)}".encode()).hexdigest()[:10]
            first = key not in seen
            seen.setdefault(key, []).append(stem)
            fdir = findings / key
            fdir.mkdir(exist_ok=True)
            shutil.copytree(mdir, fdir / stem, dirs_exist_ok=True)
            (fdir / stem / f"{mode}.log").write_text(log)
            (fdir / "signature.txt").write_text(f"{mode}: {sig}\n{frame_hint(log)}\nfrom {src}\nops {ops}\n")
            print(f"  FAIL  {stem} [{mode}] rc={rc}  {sig}{'  ' + frame_hint(log) if frame_hint(log) else ''}"
                  f"{'' if first else '  (dup of ' + key + ')'}")
        if not failed_here and not args.keep:
            shutil.rmtree(mdir, ignore_errors=True)

    n_fail = sum(len(v) for v in seen.values())
    print(f"== fuzz-sch: {total_runs} runs, {n_fail} failures in {len(seen)} distinct signature(s), "
          f"{arithmetic} run(s) with integer-overflow warnings only; seed {args.seed}")
    for key, stems in seen.items():
        print(f"   - {key}: {len(stems)}x  {(findings / key / 'signature.txt').read_text().splitlines()[0]}")
    return 1 if seen else 0


if __name__ == "__main__":
    sys.exit(main())
