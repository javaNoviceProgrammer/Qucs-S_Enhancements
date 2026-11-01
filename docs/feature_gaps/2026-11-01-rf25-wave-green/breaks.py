#!/usr/bin/env python3
"""breaks.py: each piece of phase C's first round - the retarded Green's
functions of the stack - undone on purpose: the branch of kz, the scalar
kernel's sign, the ground's short, the source's split, the substrate's
conductivity and loss in its permittivity, the singular terms, the path off
the axis, the tail's panels, its cap and its turn onto Hankel's lines, the
table's interpolation, the heights snapped to a boundary, and J0's half of
the plane. Sources are put back whatever happens. BRK_ONLY_CHECK=1 checks
the anchors; HUNT_RUNS=<dir> for logs."""
import os, subprocess, sys

Q = "/Users/meisam/git/Qucs-S_Enhancements/qucs-s-26.1.1/qucs/"
B = "/Users/meisam/git/Qucs-S_Enhancements/build"
RUNS = os.environ.get("HUNT_RUNS", "/tmp/rf25-wave-green-breaks")
os.makedirs(RUNS, exist_ok=True)

W = "rf25/rf25_wave.cpp"
T = "test_rf25_wave"
BESSEL = "theBesselFunctionOfAComplexArgumentIsJ0"
HANKEL = "hankelsFunctionsAreH0"
SPECTRAL = "theSpectralKernelsAreThoseOfAGroundedSlab"
IMAGE = "overAGroundInOneMediumTheyAreTheSourceAndItsImage"
STATIC = "atLowFrequencyTheyAreTheStaticOnes"
RECIPROCAL = "theyAreReciprocal"
PATH = "thePathOffTheAxisMakesNoDifference"
TABLE = "theTableIsTheIntegral"
SILICON = "aConductiveSubstrateHoldsChargeLikeAGround"

BREAKS = [
 ("kz taken on the principal branch, growing as often as decaying",
  [(W, "    if (s.imag() > 0) s = -s;\n", "")],
  [(T, SPECTRAL), (T, IMAGE)]),

 ("the scalar kernel's sign turned over",
  [(W, "    return {kJ * w / (kr * kr) * (ve - vh), vh / (kJ * w)};", "    return {kJ * w / (kr * kr) * (vh - ve), vh / (kJ * w)};")],
  [(T, SPECTRAL), (T, STATIC)]),

 ("the ground an open circuit for TE",
  [(W, "        downE[0] = downH[0] = Complex(-1, 0);", "        downE[0] = Complex(-1, 0);\n        downH[0] = Complex(1, 0);")],
  [(T, SPECTRAL), (T, IMAGE)]),

 ("the current at the source not split between the two ways",
  [(W, "    const Complex rising = Z[m] * (1.0 + gd) / (2.0 * d);", "    const Complex rising = Z[m] * (1.0 + gd) / d;")],
  [(T, SPECTRAL)]),

 ("a substrate's conductivity left out of its permittivity",
  [(W, "        a_eps << kEps0 * Complex(s.epsr, -s.epsr * s.tand) - Complex(0, s.sigma / a_omega);",
    "        a_eps << kEps0 * Complex(s.epsr, -s.epsr * s.tand);")],
  [(T, SILICON)]),

 ("a loss tangent that gains",
  [(W, "        a_eps << kEps0 * Complex(s.epsr, -s.epsr * s.tand) - Complex(0, s.sigma / a_omega);",
    "        a_eps << kEps0 * Complex(s.epsr, s.epsr * s.tand) - Complex(0, s.sigma / a_omega);")],
  [(T, SPECTRAL)]),

 ("the singular term blind to the interface the charge is on",
  [(W, "        factor = (1.0 + localDown(m, low)) * (1.0 + localUp(m, high)) / 2.0;", "        factor = 0.5;")],
  [(T, SPECTRAL)]),

 ("the vector potential's image left to the integral",
  [(W, "        a_vectorImage.coefficient = -kMu0 / 2;", "        a_vectorImage.coefficient = 0.0;")],
  [(T, IMAGE)]),

 ("the path grazing the real axis",
  [(W, "    const double b = ellipse > 0 ? ellipse : std::min(0.5 * a, std::max(0.05 * a, rho > 0 ? 1.0 / rho : 0.5 * a));",
    "    const double b = ellipse > 0 ? ellipse : 1e-9 * a;")],
  # (Not the table's test or reciprocity's: each compares the path with
  # itself, and both sides graze alike.)
  [(T, IMAGE)]),

 ("no doubling panels past a: one long one to the oscillation",
  [(W, "        if (lo >= half && (decay - lo) * rho >= 4 * M_PI) break;", "        if (rho > 0 && (decay - lo) * rho >= 4 * M_PI) break;")],
  [(T, IMAGE)]),

 ("no cap where rounding takes over",
  [(W, "        if (hi >= cap\n", "        if (false\n")],
  [(T, PATH)]),

 ("the tail's Hankel functions of the wrong kinds",
  [(W, "            const Complex hu = hankelH0(up * rho, 1) * up, hd = hankelH0(down * rho, 2) * down;",
    "            const Complex hu = hankelH0(up * rho, 2) * up, hd = hankelH0(down * rho, 1) * down;")],
  [(T, STATIC), (T, TABLE)]),

 ("the table read by a cubic again",
  [(W, "    const int j = std::clamp(i - 2, 1, int(a_rho.size()) - 6);", "    const int j = std::clamp(i - 1, 1, int(a_rho.size()) - 6);"),
   (W, "    for (int p = j; p < j + 6; ++p) {\n        double weight = 1;\n        for (int q = j; q < j + 6; ++q)",
    "    for (int p = j; p < j + 4; ++p) {\n        double weight = 1;\n        for (int q = j; q < j + 4; ++q)")],
  [(T, TABLE), (T, IMAGE)]),

 ("a height a rounding step off its boundary in no slab",
  [(W, "    zSource = snapped(zSource);\n    zField = snapped(zField);\n", "")],
  [(T, SILICON)]),

 ("J0 by Hankel's form in the left half-plane",
  [(W, "    if (z.real() < 0) z = -z;\n", "")],
  [(T, BESSEL)]),
]

def apply(edits, back):
    for path, old, new in edits:
        p = Q + path
        s = open(p, encoding="utf-8").read()
        if s.count(old) != 1:
            raise SystemExit("%s: anchor %d times: %s" % (path, s.count(old), old[:90]))
        if back is not None and p not in back:
            back[p] = s
        open(p, "w", encoding="utf-8").write(s.replace(old, new))

def main():
    only = os.environ.get("BRK_ONLY_CHECK")
    results = []
    for i, (name, edits, tests) in enumerate(BREAKS, 1):
        back = {}
        print("\n=== %d/%d %s" % (i, len(BREAKS), name), flush=True)
        try:
            apply(edits, back)
            if only:
                results.append((name, "anchors ok"))
                continue
            log = os.path.join(RUNS, "brk%02d.log" % i)
            suites = sorted({s for s, _ in tests})
            with open(log, "w") as out:
                if subprocess.run(["ninja", "-C", B] + suites, stdout=out,
                                  stderr=subprocess.STDOUT).returncode != 0:
                    results.append((name, "BUILD FAILED - see " + log))
                    continue
                said = []
                for suite, fn in tests:
                    try:
                        r = subprocess.run([os.path.join(B, "qucs/tests", suite), fn], stdout=out,
                                           stderr=subprocess.STDOUT, timeout=900,
                                           env=dict(os.environ, QT_QPA_PLATFORM="offscreen",
                                                    ASAN_OPTIONS="detect_leaks=0"))
                        said.append("%s %s" % (fn[:30], "failed (caught)" if r.returncode != 0 else "PASSED - NOT CAUGHT"))
                    except subprocess.TimeoutExpired:
                        said.append("%s hung" % fn[:30])
                results.append((name, "; ".join(said)))
        finally:
            for p, s in back.items():
                open(p, "w", encoding="utf-8").write(s)
    print("\n\n===== what each break did =====")
    bad = 0
    for name, said in results:
        print("%-66s %s" % (name[:66], said))
        if "NOT CAUGHT" in said or "BUILD FAILED" in said or "hung" in said: bad += 1
    print("\n%d of %d uncaught" % (bad, len(results)))
    return 1 if bad else 0

sys.exit(main())
