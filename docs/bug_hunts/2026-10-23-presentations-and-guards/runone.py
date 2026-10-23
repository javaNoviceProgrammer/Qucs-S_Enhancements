#!/usr/bin/env python3
"""runone.py <exe> <deck> [timeout] [limitMB]: renderDecksGiven on one deck, its peak RSS, time, end."""
import os, subprocess, sys, time
exe, deck = sys.argv[1], sys.argv[2]
timeout = float(sys.argv[3]) if len(sys.argv) > 3 else 120
limit = int(sys.argv[4]) if len(sys.argv) > 4 else 6000
env = dict(os.environ, QT_QPA_PLATFORM="offscreen", QUCS_PPTX_OUT="-", QUCS_PPTX_RENDER=deck, ASAN_OPTIONS="detect_leaks=0",
           QUCS_CACHE_DIR=os.path.join(os.path.dirname(os.path.abspath(__file__)), "cache"))
log = open(deck + ".log", "w")
t0 = time.time(); p = subprocess.Popen([exe, "renderDecksGiven", "-maxwarnings", "0"], env=env, stdout=log, stderr=subprocess.STDOUT)
peak = 0; why = ""
while p.poll() is None:
    time.sleep(0.25)
    try: r = int(subprocess.run(["ps", "-o", "rss=", "-p", str(p.pid)], capture_output=True, text=True).stdout.strip() or 0) // 1024
    except ValueError: r = 0
    peak = max(peak, r)
    if r > limit: why = "KILLED at %d MB" % r; p.kill()
    elif time.time() - t0 > timeout: why = "KILLED after %d s" % timeout; p.kill()
p.wait(); log.close()
text = open(deck + ".log", errors="replace").read()
import re
m = re.search(r"\d+ slides, read [^\n]*", text)
asan = re.search(r"(ERROR: AddressSanitizer[^\n]*|runtime error[^\n]*)", text)
print("%-28s exit %s %s peak %d MB %.1f s %s %s" % (os.path.basename(deck), p.returncode, why, peak, time.time() - t0, m.group(0) if m else "", asan.group(0)[:200] if asan else ""))
