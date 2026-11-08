#!/usr/bin/env python3
"""stress.py: a test of the ASan build run in copies side by side, as a busy
machine runs it, in ctest's environment - counting the runs that failed, the
programs it started that were killed by signal 9, and macOS's new reports of
a child dead in fork().

  stress.py [TEST] [ROUNDS] [COPIES]       (test_git_integration 3 3)
  STRESS_CONTEXT=8 stress.py               the stacks of 14 October

STRESS_CONTEXT sets ASAN_OPTIONS's malloc_context_size, over the build's own
(tests/sanitizer_options.cpp: 0 on macOS). BRK_BUILD the build tree
(build-asan); HUNT_RUNS where the logs go."""
import glob, os, subprocess, sys

W = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../.."))   # (the checkout it is in)
B = os.environ.get("BRK_BUILD", W + "/build-asan")
RUNS = os.environ.get("HUNT_RUNS", "/tmp/asan-fork-stress")
REPORTS = os.path.expanduser("~/Library/Logs/DiagnosticReports")
os.makedirs(RUNS, exist_ok=True)

test = sys.argv[1] if len(sys.argv) > 1 else "test_git_integration"
rounds = int(sys.argv[2]) if len(sys.argv) > 2 else 3
copies = int(sys.argv[3]) if len(sys.argv) > 3 else 3

asan = "detect_leaks=0"
if os.environ.get("STRESS_CONTEXT"):
    asan += ":malloc_context_size=" + os.environ["STRESS_CONTEXT"]
# (ctest's environment, qucs_add_test's: offscreen, no claude or gh of the machine's)
env = dict(os.environ, QT_QPA_PLATFORM="offscreen", ASAN_OPTIONS=asan, UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1",
           QUCS_CLAUDE="/nonexistent/claude", QUCS_GH="/nonexistent/gh", QTEST_FUNCTION_TIMEOUT="300000")

def reports():
    return set(glob.glob(os.path.join(REPORTS, test + "-*.ips")))

def forkDeath(path):
    """Whether macOS's report is of a child dead in fork() under the sanitizer."""
    text = open(path, errors="replace").read()
    return "crashed on child side of fork" in text and "StackDepot" in text

before = reports()
failed = killed = 0
for r in range(1, rounds + 1):
    procs = []
    for c in range(1, copies + 1):
        log = open(os.path.join(RUNS, "%s-%d-%d.txt" % (test, r, c)), "w")
        procs.append((subprocess.Popen(["./" + test], cwd=B + "/qucs/tests", env=env, stdout=log, stderr=subprocess.STDOUT,
                                       stdin=subprocess.DEVNULL), log))
    for p, log in procs:
        code = p.wait()
        log.close()
        text = open(log.name, errors="replace").read()
        nine = text.count("exit 9 QProcess::Crashed")
        failed += code != 0
        killed += nine
        print("round %d: %s, %d killed" % (r, "passed" if code == 0 else "FAILED (%d)" % code, nine), flush=True)
deaths = sum(forkDeath(p) for p in reports() - before)
print("%s, malloc_context_size %s: %d of %d runs failed, %d programs killed, %d children dead in fork()"
      % (test, os.environ.get("STRESS_CONTEXT", "the build's"), failed, rounds * copies, killed, deaths))
sys.exit(1 if failed or deaths else 0)
