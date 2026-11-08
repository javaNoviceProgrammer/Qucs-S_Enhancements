#!/usr/bin/env python3
"""abtable.py <folder>: the before-N.txt and after-N.txt of ab.sh as tables,
the least of the passes for each number (the machine was shared)."""
import glob, os, re, sys

D = sys.argv[1]
KEYS = [("open", r"open (\d+) ms"), ("first slide", r"first slide (\d+) ms"), ("switch avg", r"slide switch (\d+) ms average"),
        ("switch worst", r"slide switch \d+ ms average \((\d+) worst"), ("next slide (paced) avg", r"pace (\d+) ms average"),
        ("next slide (paced) worst", r"pace \d+ ms average \((\d+) worst"), ("zoom avg", r"zoom (\d+) ms average"),
        ("zoom worst", r"zoom \d+ ms average \((\d+) worst"), ("scroll 800% avg", r"scroll at 800% (\d+) ms"),
        ("scroll 800% worst", r"scroll at 800% \d+ ms a step \((\d+) worst"), ("thumbnails", r"thumbnails (\d+) ms"),
        ("event loop held", r"held (\d+) ms"), ("memory opened (MB)", r"memory (-?\d+) MB opened"), ("memory at end (MB)", r"(-?\d+) MB at the end")]


def load(side):
    out = {}
    for f in sorted(glob.glob(os.path.join(D, side + "-*.txt"))):
        for line in open(f):
            m = re.match(r"(\S+): .* dpr (\S+)", line)
            if not m:
                continue
            key = (m.group(1), m.group(2))
            vals = out.setdefault(key, {})
            for name, rx in KEYS:
                v = re.search(rx, line)
                if v:
                    vals.setdefault(name, []).append(int(v.group(1)))
    return out


before, after = load("before"), load("after")
for dpr in ("1.0", "2.0"):
    decks = [k for k in before if k[1] == dpr]
    print("\n**Device pixel ratio %s**\n" % dpr[0])
    print("| | " + " | ".join("%s before | after" % d[0].replace(".pptx", "") for d in decks) + " |")
    print("|---|" + "---|---|" * len(decks))
    for name, _ in KEYS:
        row = []
        for d in decks:
            b = before.get(d, {}).get(name, [])
            a = after.get(d, {}).get(name, [])
            row += [str(min(b)) if b else "-", str(min(a)) if a else "-"]
        print("| %s | %s |" % (name, " | ".join(row)))
