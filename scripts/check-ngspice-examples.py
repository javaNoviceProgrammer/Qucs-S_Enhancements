#!/usr/bin/env python3
"""Runs every example the ngspice_commands tool shows through ngspice.

The examples live in qucs-s-26.1.1/qucs/qucscontrol_ngspice.cpp (kExamples):
the lines of a .control block. Each is run inside a small circuit written
here, in a scratch folder, and every line of ngspice's output that reads as
an error is reported. An example whose command this ngspice lacks (a stock
ngspice has none of the enhanced build's) is skipped and said.

    python3 scripts/check-ngspice-examples.py [path/to/ngspice [model.osdi]]

model.osdi, any compiled Verilog-A model, stands for the bsim4.osdi the osdi
example loads; without it that example is skipped.

Exits 1 when an example needs looking at.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.join(HERE, "..", "qucs-s-26.1.1", "qucs", "qucscontrol_ngspice.cpp")

RC = "Vin in 0 dc 0 ac 1\nR1 in out 1k\nC1 out 0 100n\n"
STEP = "Vin in 0 pulse(0 1 1u 1n 1n 1 2) ac 1\nR1 in out 1k\nC1 out 0 100n\n"
DIVIDER = "V1 in 0 dc 5\nR1 in out 2k\nR2 out 0 3k\n"
NMOS = "Vds d 0 dc 1\nVgs g 0 dc 2\nM1 d g 0 0 NM W=20u L=1u\n.model NM NMOS(level=1 vto=0.7 kp=120u lambda=0.02)\n"
MIXER = "V1 lo 0 SIN(0 1 1meg)\nVDC a 0 DC 1 AC 1\nR1 a b 10k\nB1 b 0 I=(1m + 0.8m*v(lo))*v(b)\nC1 b 0 100p\n"
PORTS = "L1 in out 1u\nC1 out 0 400p\nV1 in 0 dc 0 ac 1 portnum 1 z0 50\nV2 out 0 dc 0 ac 1 portnum 2 z0 50\n"
CLIPPER = "Vin in 0 sin(0 1.2 10k)\nR1 in a 1k\nD1 a 0 DL\nD2 0 a DL\n.model DL D(is=1e-12)\nRo a out 1\nRl out 0 1meg\n"
BITS = ("Vd tx 0 pwl(0 0 1n 0 1.05n 1 3n 1 3.05n 0 4n 0 4.05n 1 7n 1 7.05n 0 9n 0 9.05n 1 10n 1 10.05n 0 r=0)\n"
        "R1 tx rx 50\nC1 rx 0 5p\n")
CE_AMP = ("Vcc cc 0 dc 5\nVin in 0 dc 0.75 ac 1 distof1 1\nRb cc b 220k\nRc cc c 2.2k\nCin in b 1u\nCout c 0 20p\n"
          "Q1 c b 0 QMOD\n.model QMOD NPN(is=1e-15 bf=150 vaf=80 cjc=3p tf=0.3n)\n")
LOOP = "Vin inp 0 dc 0\nE1 a 0 inp b 1000\nVprobe a x 0\nR1 x b 1k\nR2 b 0 1k\nIprobe 0 b 0\nC1 x 0 1n\n"

# Each example's circuit, the lines run before it and after it, and the
# command it needs (its name when not given).
CASES = {
    "op": (DIVIDER, "", ""),
    "dc": (NMOS, "", ""),
    "ac": (RC, "", ""),
    "tran": (STEP, "", ""),
    "noise": ("Vin in 0 dc 0 ac 1\nR1 in out 10k\nC1 out 0 1n\n", "", ""),
    "tf": ("Vin in 0 dc 1\nR1 in out 1k\nR2 out 0 1k\n", "", ""),
    "pz": ("Vin in 0 dc 0 ac 1\nL1 in m 1m\nR1 m out 60\nC1 out 0 100n\n", "", ""),
    "sens": (DIVIDER, "", ""),
    "disto": (CE_AMP, "", ""),
    "sp": (PORTS, "", "", "wrsnp"),
    "pss": (MIXER + ".pss 1meg 1u b 1024 8 50 5u\n", "", ""),
    ".pac": (MIXER + ".pac 1meg 1u b 1024 6 50 5u lin 60 5k 900k 1\n", "", "", "hb"),
    ".pnoise": (MIXER + ".pnoise 1meg 1u b 1024 6 50 5u b vdc dec 25 1k 400k\n", "", "", "hb"),
    "hb": ("V1 in 0 sin(0 1 1meg)\nR1 in out 1k\nD1 out 0 DL\n.model DL D(is=1e-12)\n", "", ""),
    "stb": (LOOP, "", ""),
    "rfstab": (PORTS, "", ""),
    "meas": (STEP, "", ""),
    "fft": (CLIPPER, "", ""),
    "fourier": (CLIPPER, "", ""),
    "eye": (BITS, "", ""),
    "let": ("Vin in 0 dc 1 ac 1\nR1 in out 1k\nvout out o2 0\nR2 o2 0 1k\n", "ac dec 10 1 1k", ""),
    "print": (DIVIDER, "op\nlet vmax = v(out)", ""),
    "wrdata": (DIVIDER, "op", ""),
    "pyplot": (RC, "ac dec 10 1 1meg", ""),
    "alter": (DIVIDER + NMOS.replace("Vds", "Vdd"), "", ""),
    "altermod": (NMOS, "", "print i(vds)"),
    "alterparam": (".param vsup=5\nV1 in 0 dc {vsup}\nR1 in 0 1k\n", "", ""),
    "show": (NMOS, "", ""),
    "osdi": ("R1 1 0 1\n", "", "op", "pre_osdi"),
    "snp": ("R1 1 0 1\n", "", "op"),
    "montecarlo": (".param rr=agauss(1k, 50, 3)\nR1 in out {rr}\nC1 out 0 159n\nVin in 0 dc 0 ac 1\n", "", ""),
    "optimize": (RC, "", ""),
    "sweep": (DIVIDER, "", ""),
    "stop": (STEP, "", ""),
    "savestate": (STEP + ".tran 1n 2u\n", "", ""),
    "foreach": (RC, "", ""),
    "if": (STEP, "tran 1u 1m", ""),
    "dowhile": (DIVIDER, "", "op"),
}

ERROR = re.compile(r"rror|unknown command|not found|can't|cannot|no such|not a valid|syntax|failed|illegal|undefined"
                   r"|no vector|not available", re.I)
# (Said by ngspice whatever the example: no spinit beside it, and sweep's
# scale vector, looked for as a device.)
NOISE = ("spinit", "checkvalid: vector r1")


def examples():
    text = open(SOURCE).read()
    start = text.index("} kExamples[] = {")
    table = text[start:text.index("};", start)]
    for m in re.finditer(r'\{"([^"]+)", "((?:[^"\\]|\\.)*)"\}', table):
        yield m.group(1), m.group(2).encode().decode("unicode_escape")


def commands(ngspice, folder):
    deck = os.path.join(folder, "help.cir")
    with open(deck, "w") as f:
        f.write("* commands\nR1 1 0 1\n.control\nhelp all\n.endc\n.end\n")
    out = subprocess.run([ngspice, "-b", deck], capture_output=True, text=True, timeout=60).stdout
    return {m.group(1) for m in re.finditer(r"^([a-z_][a-z0-9_]*)( .*)? : ", out, re.M)}


def main():
    ngspice = sys.argv[1] if len(sys.argv) > 1 else "ngspice"
    folder = tempfile.mkdtemp(prefix="ngspice-examples-")
    # The osdi example loads bsim4.osdi: any compiled model will do.
    model = sys.argv[2] if len(sys.argv) > 2 else None
    if model:
        shutil.copy(model, os.path.join(folder, "bsim4.osdi"))
    has = commands(ngspice, folder)
    checked = skipped = looks = 0
    for name, example in examples():
        case = CASES.get(name)
        if case is None:
            print(f"{name}: no circuit for it here")
            looks += 1
            continue
        circuit, before, after = case[:3]
        needs = case[3] if len(case) > 3 else name
        if needs not in has or (name == "osdi" and not model):
            print(f"{name}: skipped - {'no compiled model given' if needs in has else 'this ngspice has no ' + needs}")
            skipped += 1
            continue
        deck = os.path.join(folder, "deck.cir")
        with open(deck, "w") as f:
            f.write(f"* {name}\n{circuit}.control\n" + "\n".join(l for l in (before, example, after) if l) + "\n.endc\n.end\n")
        try:
            r = subprocess.run([ngspice, "-b", deck], capture_output=True, text=True, timeout=120, cwd=folder)
            out = r.stdout + r.stderr
        except subprocess.TimeoutExpired:
            out = "Error: no answer within 120 s"
        said = [l for l in out.splitlines() if ERROR.search(l) and not any(n in l for n in NOISE)]
        checked += 1
        if said:
            looks += 1
            print(f"{name}: look at it")
            for l in said[:6]:
                print("    " + l[:200])
        else:
            print(f"{name}: ok")
    if looks:
        print(f"{checked} examples run, {skipped} skipped, {looks} to look at (their decks and output in {folder})")
        return 1
    shutil.rmtree(folder)
    print(f"{checked} examples run, {skipped} skipped, none to look at")
    return 0


if __name__ == "__main__":
    sys.exit(main())
