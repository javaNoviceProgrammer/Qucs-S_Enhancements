"""A spectrogram's segment far shorter than a sample: how long add_diagram takes (Release build)."""
import sys, threading, time, mcp
from common import rc, show
s = mcp.Server('p3')
p, sim = rc(s)
for seg in (1e-9, 1e-10, 1e-11):
    done = []
    t = threading.Thread(target=lambda: done.append(s.call('add_diagram', {'type': 'spectrogram', 'traces': ['tran.v(out)'], 'spectrogram': {'segment': seg}})), daemon=True)
    t0 = time.time(); t.start(); t.join(90)
    if not done:
        print(f'segment {seg}: no answer after {time.time() - t0:.0f} s; the server is busy (pid {s.p.pid}), killed'); s.p.kill(); sys.exit(0)
    show(f'segment {seg}: {time.time() - t0:.1f} s', done[0].get('analyses') if isinstance(done[0], dict) else done[0], 300)
s.close()
