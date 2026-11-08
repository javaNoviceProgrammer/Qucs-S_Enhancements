"""Deliberate breaks: each replaces one piece of source, builds, runs the tests that should catch it, and puts the
source back. usage: python3 breaks.py <breaks.json> [name...]; results appended to breaks_results.txt.
Each break: {"name", "file", "old", "new", "tree", "tests": [[binary, function...], ...]}."""
import json, os, subprocess, sys, shutil, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
R = os.path.abspath(os.path.join(HERE, "../../.."))            # the repository
S = os.environ.get("SCRATCH", tempfile.gettempdir())             # results and ctest folders
env = dict(os.environ, PATH="/opt/homebrew/opt/bison/bin:" + os.environ["PATH"])   # (a dos2unix on PATH too)
breaks = json.load(open(sys.argv[1])); only = sys.argv[2:]
out = open(os.path.join(S, "breaks_results.txt"), "a")
for b in breaks:
    if only and b["name"] not in only: continue
    path = os.path.join(R, b["file"]); keep = path + ".keep"
    src = open(path).read()
    assert src.count(b["old"]) == 1, (b["name"], src.count(b["old"]))
    shutil.copy2(path, keep)
    try:
        open(path, "w").write(src.replace(b["old"], b["new"]))
        targets = sorted({t[0] for t in b["tests"]})
        tree = os.path.join(R, b.get("tree", "build-rel"))
        r = subprocess.run(["ninja", "-C", tree] + targets, env=env, capture_output=True, text=True)
        if r.returncode != 0:
            verdict = "BUILD FAILED: " + r.stdout[-300:]
        else:
            fails = []
            for t in b["tests"]:
                r = subprocess.run([os.path.join(HERE, "fns.sh"), tree] + t, env=env, capture_output=True, text=True)
                fails += [l for l in r.stdout.splitlines() if l.startswith("FAIL") or "Received signal" in l or "AddressSanitizer" in l]
            verdict = ("CAUGHT: " + fails[0][:220]) if fails else "NOT CAUGHT"
    finally:
        shutil.move(keep, path)
        os.utime(path, None)
    line = f"{b['name']}: {verdict}"
    print(line, flush=True); out.write(line + "\n"); out.flush()
