"""Shorted and open CPW stubs, one port: Z_short*Z_open = Z0^2 and Z_short/Z_open = -tan^2(beta L)."""
import sys, json, cmath, math; sys.path.insert(0, "."); from run import Server
folder, pairs, freqs, L = sys.argv[1], [p.split("/") for p in sys.argv[2].split(",")], [float(f) for f in sys.argv[3].split(",")], float(sys.argv[4]) * 1e-6
s = Server(folder)
def z(name, f):
    bad, ans, text = s.call("rf25_solve", {"path": name + ".qrf25", "solver": "full_wave", "frequency": f"{f}[GHz]", "timeout": 3600})
    if bad: raise SystemExit(name + ": " + text[:500])
    S11 = complex(ans["s matrix"]["re"][0][0], ans["s matrix"]["im"][0][0])
    return 50 * (1 + S11) / (1 - S11), ans
for short, opn in pairs:
    for f in freqs:
        zs, a = z(short, f); zo, _ = z(opn, f)
        z0 = cmath.sqrt(zs * zo); t = cmath.sqrt(-zs / zo)
        bl = cmath.atan(t).real
        eps = (bl / L * 299792458 / (2 * math.pi * f * 1e9)) ** 2
        print(f"{short} vs {opn} {f} GHz: Zs {zs:.2f} Zo {zo:.2f}  Z0 {z0.real:.2f}  eps_eff {eps:.4f}", flush=True)
    print("   ", [p for p in a.get("ports", [])][:1])
s.close()
