#!/usr/bin/env python3
"""
Mutation fuzzer for the simulator-output parsers (AbstractSpiceKernel::
parse*Output, convertToQucsData).

The corpus is what the `simulate` smoke suite leaves behind: for every
circuit it ran, <work>/<circuit>/simout/ holds the real ngspice output
files (raw plots with binary sections, sweep tables, noise, sensitivity,
DC print, operating point) and schematic.txt names the schematic they
belong to. Each mutant copies one such directory, damages one output file
(the text operators of fuzz-sch.py, plus raw-header specific ones), and
runs qucs/tests/simout_harness on it, which converts the outputs to a Qucs
dataset exactly as the GUI does after a simulation.

A damaged output may produce an empty or partial dataset; it must never
crash, hang or trip a sanitizer. Findings are kept under
<out-dir>/findings/ with the whole mutant directory and the log.

Usage:
    fuzz-simout.py <simout_harness> <simulate-work-dir> <out-dir>
                   [--count N] [--seed S] [--timeout SEC] [--keep]
                   [--reproduce <mutant-dir>]
"""
import argparse
import hashlib
import importlib.util
import os
import random
import re
import shutil
import subprocess
import sys
from pathlib import Path

# The text mutators and failure classification live in fuzz-sch.py.
_spec = importlib.util.spec_from_file_location("fuzz_sch", Path(__file__).with_name("fuzz-sch.py"))
fuzz_sch = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(fuzz_sch)


# --------------------------------------------------------------------------
# raw-file specific operators (ngspice raw format: text header, then a
# "Binary:" or "Values:" section)
def m_flags(t, r):
    if "Flags: complex" in t:
        return t.replace("Flags: complex", "Flags: real", 1)
    return t.replace("Flags: real", "Flags: complex", 1)


def m_counts(t, r):
    m = re.search(r"No\. (Points|Variables): *(\d+)", t)
    if not m:
        return fuzz_sch.m_hostile_number(t, r)
    return t[: m.start(2)] + r.choice(fuzz_sch.HOSTILE_NUMBERS + ["1", "2", "1000000"]) + t[m.end(2):]


def m_section(t, r):
    if "Binary:" in t:
        return t.replace("Binary:", r.choice(["Values:", "", "Binary"]), 1)
    if "Values:" in t:
        return t.replace("Values:", r.choice(["Binary:", "", "Values"]), 1)
    return t


def m_drop_variable(t, r):
    lines = t.split("\n")
    cands = [i for i, l in enumerate(lines) if re.match(r"^\t\d+\t", l)]
    if not cands:
        return t
    del lines[r.choice(cands)]
    return "\n".join(lines)


def m_binary_bytes(t, r):
    i = t.find("Binary:\n")
    if i < 0:
        return fuzz_sch.m_bit_flip(t, r)
    i += len("Binary:\n")
    body = bytearray(t[i:].encode("latin-1", "replace"))
    if not body:
        return t
    for _ in range(r.randrange(1, 8)):
        body[r.randrange(len(body))] = r.randrange(256)
    return t[:i] + body.decode("latin-1")


RAW_MUTATORS = [m_flags, m_counts, m_counts, m_section, m_drop_variable, m_binary_bytes]


def mutate(text, rng, is_raw):
    ops = []
    pool = fuzz_sch.MUTATORS + (RAW_MUTATORS * 2 if is_raw else [])
    for _ in range(rng.choice([1, 1, 2, 2, 3])):
        op = rng.choice(pool)
        text = op(text, rng)
        ops.append(op.__name__[2:])
    return text, "+".join(ops)


# --------------------------------------------------------------------------
def run_harness(harness, sch, mdir, timeout):
    cmd = [harness, str(sch), str(mdir), str(mdir / "out.dat")]
    env = dict(os.environ)
    env.setdefault("QT_QPA_PLATFORM", "offscreen")
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
        or fuzz_sch.CRASH_RE.search(log) \
        or (fuzz_sch.SANITIZER_RE.search(log) and not fuzz_sch.is_arithmetic_only(log))
    return bool(bad), rc, log


def corpus_entries(work, harness, timeout):
    """(schematic, simout dir, files worth damaging) for every circuit the
    simulate suite stashed. The kernel's work directory accumulates the
    outputs of every circuit run before, so a baseline run of the harness
    tells which files this circuit's conversion actually reads: the outputs
    it names, plus the sweep/noise/operating-point side files (*.cir.*)."""
    entries = []
    for marker in sorted(Path(work).rglob("simout/schematic.txt")):
        simout = marker.parent
        sch = simout.parent / marker.read_text().strip()
        if not sch.is_file():
            continue
        bad, rc, log = run_harness(harness, sch, simout, timeout)
        m = re.search(r"^outputs: (.*)$", log, re.M)
        if bad or not m:
            print(f"  skip  {sch.name}: baseline conversion failed (rc={rc})", file=sys.stderr)
            continue
        named = set(m.group(1).split())
        files = [f for f in sorted(simout.glob("spice4qucs.*"))
                 if f.name in named or ".cir." in f.name]
        if files:
            entries.append((sch, simout, files))
    return entries


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("harness")
    ap.add_argument("work", help="the simulate suite's work directory")
    ap.add_argument("out")
    ap.add_argument("--count", type=int, default=200)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--timeout", type=float, default=60)
    ap.add_argument("--keep", action="store_true", help="keep mutants that passed")
    ap.add_argument("--reproduce", metavar="DIR", help="run the harness on this mutant directory")
    args = ap.parse_args()

    out = Path(args.out)
    mutants = out / "mutants"
    findings = out / "findings"
    for d in (mutants, findings):
        d.mkdir(parents=True, exist_ok=True)

    if args.reproduce:
        mdir = Path(args.reproduce)
        sch = mdir / (mdir / "schematic.txt").read_text().strip()
        bad, rc, log = run_harness(args.harness, sch, mdir, args.timeout)
        (out / "repro.log").write_text(log)
        print(f"{'FAIL' if bad else 'ok  '} rc={rc} {fuzz_sch.signature(log, rc) if bad else ''}")
        return 1 if bad else 0

    entries = corpus_entries(args.work, args.harness, args.timeout)
    if not entries:
        print(f"no simulator outputs under {args.work} (run the simulate smoke suite with a Debug build first)",
              file=sys.stderr)
        return 2
    n_files = sum(len(files) for _, _, files in entries)
    rng = random.Random(args.seed)
    print(f"== fuzz-simout: {args.count} mutants of {len(entries)} circuits ({n_files} output files), seed {args.seed}")

    total = 0
    arithmetic = 0
    seen = {}
    for i in range(args.count):
        sch, simout, files = rng.choice(entries)
        victim_src = rng.choice(files)
        stem = f"m{i:05d}-{sch.stem[:20]}-{victim_src.name[len('spice4qucs.'):][:16]}"
        mdir = mutants / stem
        if mdir.exists():
            shutil.rmtree(mdir)
        shutil.copytree(simout, mdir)
        shutil.copy(sch, mdir / sch.name)
        (mdir / "schematic.txt").write_text(sch.name + "\n")
        victim = mdir / victim_src.name
        text = victim.read_bytes().decode("latin-1")
        is_raw = "Plotname:" in text[:2000]
        mutated, ops = mutate(text, rng, is_raw)
        victim.write_bytes(mutated.encode("latin-1", "replace"))
        (mdir / "mutant.ops").write_text(f"{simout}\n{victim_src.name}: {ops}\n")

        total += 1
        bad, rc, log = run_harness(args.harness, mdir / sch.name, mdir, args.timeout)
        if not bad:
            if fuzz_sch.is_arithmetic_only(log):
                arithmetic += 1
                (findings / "arithmetic.txt").open("a").write(
                    f"{stem} {fuzz_sch.ARITHMETIC_RE.search(log).group(0)}\n")
            if not args.keep:
                shutil.rmtree(mdir, ignore_errors=True)
            continue
        sig = fuzz_sch.signature(log, rc)
        hint = fuzz_sch.frame_hint(log)
        key = hashlib.sha1(f"{sig}|{hint}".encode()).hexdigest()[:10]
        first = key not in seen
        seen.setdefault(key, []).append(stem)
        fdir = findings / key
        fdir.mkdir(exist_ok=True)
        shutil.copytree(mdir, fdir / stem, dirs_exist_ok=True)
        (fdir / stem / "harness.log").write_text(log)
        (fdir / "signature.txt").write_text(f"{sig}\n{hint}\nfrom {simout}\n{victim_src.name}: {ops}\n")
        print(f"  FAIL  {stem} rc={rc}  {sig}{'  ' + hint if hint else ''}{'' if first else '  (dup of ' + key + ')'}")
        if not args.keep:
            shutil.rmtree(mdir, ignore_errors=True)

    n_fail = sum(len(v) for v in seen.values())
    print(f"== fuzz-simout: {total} runs, {n_fail} failures in {len(seen)} distinct signature(s), "
          f"{arithmetic} run(s) with integer-overflow warnings only; seed {args.seed}")
    for key, stems in seen.items():
        print(f"   - {key}: {len(stems)}x  {(findings / key / 'signature.txt').read_text().splitlines()[0]}")
    return 1 if seen else 0


if __name__ == "__main__":
    sys.exit(main())
