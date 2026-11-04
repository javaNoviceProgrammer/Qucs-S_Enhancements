"""The request's third test: the probe-bridge workaround's raw S at bridge heights 20, 10 and 5 um against the
terminal port's, both solved by this build: python3 -B -I bridges.py <models folder>."""
import os, sys, numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from twoline import read_s2p
os.chdir(sys.argv[1])
db = lambda v: 20 * np.log10(abs(v))
for L in (500, 1000):
    f, tips = read_s2p(f"tips_L{L}.s2p")
    print(f"L = {L} um (the structure {L + 280} um): at 60 GHz the terminal port's S11 {db(tips[-1][0, 0]):.2f} dB, S21 {db(tips[-1][1, 0]):.2f} dB")
    for h in (20, 10, 5):
        _, b = read_s2p(f"bridge{h}_L{L}.s2p")
        worst = max(np.abs(b[i] - tips[i]).max() for i in range(len(f)))
        at = max(range(len(f)), key=lambda i: np.abs(b[i] - tips[i]).max())
        d11 = [abs(b[i][0, 0] - tips[i][0, 0]) for i in range(len(f))]
        print(f"   bridge {h:2d} um: S11 {db(b[-1][0, 0]):7.2f} dB, S21 {db(b[-1][1, 0]):6.2f} dB at 60 GHz;"
              f" |S - S_terminal| at most {worst:.4f} (at {f[at] / 1e9:.0f} GHz), at 10 GHz {np.abs(b[4] - tips[4]).max():.4f}")
