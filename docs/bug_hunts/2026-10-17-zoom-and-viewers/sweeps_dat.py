#!/usr/bin/env python3
# Writes sweeps.dat beside sweeps.sch: 12 states of 600 points ("wave") and a stepped edge ("Ec").
import math, sys
out = sys.argv[1] if len(sys.argv) > 1 else "sweeps.dat"
pts, states = 600, 12
lines = ["<Qucs Dataset 26.1.1>", f"<indep y {pts}>"] + [f"  {i * 1e-9 / (pts - 1):.12g}" for i in range(pts)]
lines += ["</indep>", f"<indep state {states}>"] + [f"  {k}" for k in range(states)] + ["</indep>", "<dep wave y state>"]
for k in range(states):
    for i in range(pts):
        y = i / (pts - 1)
        lines.append(f"  {0.05 * k + 0.02 * math.sin(math.pi * (k + 1) * y) * math.sin(40 * math.pi * y):.10g}")
lines += ["</dep>", "<dep Ec y>"] + [f"  {0.55 if (i // 50) % 2 else 0.45}" for i in range(pts)] + ["</dep>"]
open(out, "w").write("\n".join(lines) + "\n")
