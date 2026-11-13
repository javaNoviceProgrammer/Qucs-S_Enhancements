#!/usr/bin/env python3
"""breaks.py: the stopped routing run's fixes (12 November) undone on purpose,
one at a time - each built and test_layout_router run through ctest. Sources
are put back whatever happens, and the suite built again after.
BRK_ONLY_CHECK=1 checks the anchors and writes nothing; BRK_ONLY=2 runs that
alone; BRK_BUILD=<dir> the build (the checkout's build). Run as a script only."""
import os, subprocess, sys

W = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../.."))
Q = os.environ.get("BRK_SOURCE", W + "/qucs-s-26.1.1") + "/qucs/"
B = os.environ.get("BRK_BUILD", W + "/build")
E = "layoutrouter/router_engine.cpp"

VIAS = """    for (int k = 0; k < st.result.vias.size() && !bad; ++k) {
        const ViaPlaced& v = st.result.vias.at(k);
        const Coord i = (v.at.x - ox) / pitch, j = (v.at.y - oy) / pitch;
        if (i < 0 || j < 0 || i >= nx || j >= ny) continue;
        const std::size_t c = cellAt(int(i), int(j));
        bad = use[std::size_t(classLandLower[std::size_t(st.cls)][std::size_t(v.via)])][std::size_t(v.via)].get(c) > 0
              || use[std::size_t(classLandUpper[std::size_t(st.cls)][std::size_t(v.via)])][std::size_t(v.via + 1)].get(c) > 0;
    }
"""
SHARED = """    std::vector<quint64> sorted = st.path;
    std::sort(sorted.begin(), sorted.end());
    for (std::size_t k = 0; k < sorted.size() && !bad; ++k) {
        const int l = planeOf(sorted[k]);
        const std::size_t c = cellOf(sorted[k]);
        if (l + 1 < planes && std::binary_search(sorted.begin(), sorted.end(), packState(l + 1, c)))
            bad = use[std::size_t(classLandLower[std::size_t(st.cls)][std::size_t(l)])][std::size_t(l)].get(c) > 0
                  || use[std::size_t(classLandUpper[std::size_t(st.cls)][std::size_t(l)])][std::size_t(l + 1)].get(c) > 0;
    }
"""

BREAKS = [
    ("F1: the clean-up keeps a net without testing what it drew",
     [(E, "                if (!keepsClear(n)) {", "                if (false && !keepsClear(n)) {")]),
    ("F2: no via's landing tested",
     [(E, "    for (int k = 0; k < st.result.vias.size() && !bad; ++k) {", "    for (int k = 0; false && k < st.result.vias.size() && !bad; ++k) {")]),
    ("N1: a landing tested wherever the path is on two metals at one cell", [(E, VIAS, SHARED)]),
]


def apply(edits, back):
    for path, old, new in edits:
        p = Q + path
        s = open(p, encoding="utf-8").read()
        if s.count(old) != 1:
            raise SystemExit("%s: anchor %d times: %s" % (path, s.count(old), old[:90]))
        if back is None:
            continue
        back.setdefault(p, s)
        open(p, "w", encoding="utf-8").write(s.replace(old, new))


def run(cmd):
    return subprocess.run(cmd, cwd=B, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, text=True)


def main():
    if os.environ.get("BRK_ONLY_CHECK"):
        for name, edits in BREAKS:
            apply(edits, None)
        print("%d breaks, every anchor once" % len(BREAKS))
        return 0
    which = {int(n) for n in os.environ.get("BRK_ONLY", "").split(",") if n.strip()}
    results = []
    try:
        for i, (name, edits) in enumerate(BREAKS, 1):
            if which and i not in which:
                continue
            back = {}
            try:
                apply(edits, back)
                if run(["ninja", "-j12", "test_layout_router"]).returncode != 0:
                    results.append((name, "BUILD FAILED"))
                    continue
                r = run(["ctest", "-R", "^test_layout_router$", "--output-on-failure"])
                fails = [l.strip() for l in r.stdout.splitlines() if l.startswith("FAIL!")]
                results.append((name, ("caught: " + fails[0][:220]) if r.returncode != 0 and fails else
                                ("caught (exit %d)" % r.returncode if r.returncode != 0 else "NOT CAUGHT")))
            finally:
                for p, s in back.items():
                    open(p, "w", encoding="utf-8").write(s)
            print("%d. %s: %s" % (i, results[-1][0], results[-1][1]), flush=True)
    finally:
        run(["ninja", "-j12", "test_layout_router"])
    caught = sum(1 for _, r in results if r.startswith("caught"))
    print("%d of %d caught" % (caught, len(results)))
    return 0 if caught == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
