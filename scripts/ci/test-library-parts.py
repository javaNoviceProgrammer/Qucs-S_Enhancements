#!/usr/bin/env python3
"""Every part of Qucs-S's component libraries, run once under ngspice.

Each part is placed alone on a schematic, each of its pins tied to ground
through 1 MOhm (a DC path for every node), and its operating point run
with ngspice through `qucs-s --mcp-server`. A part passes when it is
placed, netlists and the operating point converges. This is a smoke test:
it finds a model the translation breaks or ngspice refuses - not one that
runs and does the wrong thing (a 741 whose tail current flows backwards).
find_library_component reports each part's outcome ("ngspice") from the
file this writes beside the libraries, and takes "tested" to list only
the parts that pass.

    python3 scripts/ci/test-library-parts.py [--qucs BIN] [--library DIR]
        [--out FILE] [--only OpAmps,LEDs] [--jobs 4] [--baseline FILE]

With --baseline, exits 1 when a part that passed there fails now (the
nightly job's regression check). Each server gets settings, a home and a
workspace of its own in a temporary folder.
"""
import argparse
import datetime
import json
import os
import queue
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..", "..")
DEFAULT_LIBRARY = os.path.join(ROOT, "qucs-s-26.1.1", "library")
RUN_SECONDS = 30   # an operating point that takes longer is given up
DEFAULT_BINARIES = [os.path.join(ROOT, "build", "qucs", "qucs-s.app", "Contents", "MacOS", "qucs-s"),
                    os.path.join(ROOT, "build", "qucs", "qucs-s")]


def parts_of(library, only):
    """(library, part) for every <Component> of every .lib there."""
    found = []
    for name in sorted(os.listdir(library)):
        if not name.endswith(".lib"):
            continue
        lib = name[:-4]
        if only and lib not in only:
            continue
        with open(os.path.join(library, name), encoding="utf-8", errors="replace") as f:
            text = f.read()
        for m in re.finditer(r"<Component\s+([^>]+)>", text):
            found.append((lib, m.group(1).strip()))
    return found


class Server:
    """One `qucs-s --mcp-server`, its answers read by a thread (a call that
    hangs is given up after its timeout, and the server started again)."""

    def __init__(self, binary, library, ngspice):
        self.binary, self.library, self.ngspice = binary, library, ngspice
        self.start()

    def start(self):
        self.dir = tempfile.mkdtemp(prefix="qucs-libtest-")
        for sub in ("settings/qucs", "home", "ws"):
            os.makedirs(os.path.join(self.dir, sub), exist_ok=True)
        with open(os.path.join(self.dir, "settings", "qucs", "qucs_s.ini"), "w") as f:
            f.write("[General]\nNgspiceExecutable=%s\n" % self.ngspice)
        env = dict(os.environ, QT_QPA_PLATFORM="offscreen", QUCS_SETTINGS_DIR=os.path.join(self.dir, "settings"),
                   HOME=os.path.join(self.dir, "home"), QUCS_NO_SHELL_ENV="1", QUCS_CLAUDE="/nonexistent/claude",
                   QUCS_LIBRARY_DIR=self.library)
        # An ngspice built to be carried about keeps its spinit (which
        # loads the XSPICE code models) in scripts/ beside it and finds it
        # by SPICE_LIB_DIR, which a login shell sets - not this one.
        home = os.path.dirname(os.path.realpath(self.ngspice))
        if "SPICE_LIB_DIR" not in env and os.path.isfile(os.path.join(home, "scripts", "spinit")):
            env["SPICE_LIB_DIR"] = home
        self.p = subprocess.Popen([self.binary, "--mcp-server", "--workspace", os.path.join(self.dir, "ws")],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, env=env, text=True)
        self.answers = queue.Queue()
        threading.Thread(target=self.read, daemon=True).start()
        self.next = 0
        self.rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}}, 60)
        self.p.stdin.write(json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}) + "\n")
        self.p.stdin.flush()
        self.call("set_simulator", {"simulator": "ngspice"})

    def read(self):
        for line in self.p.stdout:
            try:
                self.answers.put(json.loads(line))
            except ValueError:
                pass
        self.answers.put(None)

    def rpc(self, method, params, timeout):
        self.next += 1
        self.p.stdin.write(json.dumps({"jsonrpc": "2.0", "id": self.next, "method": method, "params": params}) + "\n")
        self.p.stdin.flush()
        end = time.time() + timeout
        while True:
            try:
                m = self.answers.get(timeout=max(0.1, end - time.time()))
            except queue.Empty:
                raise TimeoutError("%s took longer than %d s" % (params.get("name", method), timeout))
            if m is None:
                raise RuntimeError("the server stopped")
            if m.get("id") == self.next:
                return m

    def call(self, tool, args, timeout=60):
        r = self.rpc("tools/call", {"name": tool, "arguments": args}, timeout).get("result", {})
        text = "\n".join(c.get("text", "") for c in r.get("content", []))
        return (not r.get("isError", False)), text

    def stop(self):
        try:
            self.p.stdin.close()
            self.p.wait(timeout=20)
        except Exception:
            self.p.kill()
        shutil.rmtree(self.dir, ignore_errors=True)


def first_line(text, limit=240):
    text = " ".join(text.split())
    return text if len(text) <= limit else text[:limit] + " ..."


def test(server, lib, part):
    """The outcome of one part: placed, netlisted, its operating point (on
    a schematic of its own, closed after)."""
    server.call("new_document", {"kind": "schematic"})
    try:
        return tried(server, lib, part)
    finally:
        server.call("close_document", {"unsaved": "discard"})


def tried(server, lib, part):
    ok, text = server.call("add_component", {"type": "Lib", "name": "X1", "x": 300, "y": 300,
                                             "properties": {"Lib": lib, "Comp": part}})
    if not ok:
        return {"passes": False, "why": "not placed: " + first_line(text)}
    try:
        pins = len(json.loads(text).get("pins", []))
    except ValueError:
        pins = 0
    if not pins:
        return {"passes": False, "untested": True, "pins": 0,
                "why": "it has no pins: data for other parts (a substrate's, a core's), not a part of a circuit"}
    calls = []
    for k in range(1, pins + 1):
        calls += [{"tool": "set_label", "arguments": {"at": "X1.%d" % k, "name": "p%d" % k}},
                  {"tool": "add_component", "arguments": {"type": "R", "name": "RT%d" % k, "x": 700, "y": 100 + 100 * k,
                                                          "rotation": 1, "properties": {"R": "1 MOhm"}}},
                  {"tool": "set_label", "arguments": {"at": "RT%d.2" % k, "name": "p%d" % k}},
                  {"tool": "connect", "arguments": {"from": "RT%d.1" % k, "to": "ground"}}]
    calls.append({"tool": "add_component", "arguments": {"type": ".DC", "name": "DC1", "x": 100, "y": 100}})
    ok, text = server.call("batch", {"calls": calls, "brief": True}, 120)
    if not ok:
        return {"passes": False, "pins": pins, "why": "not wired for the test: " + first_line(text)}
    ok, text = server.call("get_netlist", {}, 60)
    if not ok:
        return {"passes": False, "pins": pins, "why": "does not netlist: " + first_line(text)}
    ok, text = server.call("simulate", {"operating_point": True, "timeout": RUN_SECONDS, "brief": True}, RUN_SECONDS + 30)
    try:
        answer = json.loads(text.split("\n")[-1]) if ok else {}
    except ValueError:
        answer = {}
    if ok and answer.get("succeeded") and answer.get("operating point"):
        return {"passes": True, "pins": pins}
    errors = answer.get("errors") or []
    why = errors[0].get("message", "") if errors and isinstance(errors[0], dict) else (answer.get("note") or text)
    if "Still running" in str(why) or answer.get("stopped"):
        # (An amplifier biased by current sources between its rails - the
        # LM3886 - finds no operating point with its rails at ground
        # through 1 MOhm, and runs in a real circuit.)
        return {"passes": False, "pins": pins, "why": "the operating point did not end in %d s with each pin to ground through "
                "1 MOhm - a part biased from its supply pins may need them powered" % RUN_SECONDS}
    return {"passes": False, "pins": pins, "why": "the operating point fails: " + first_line(str(why))}


def write(document, path):
    """The results, a part to a line (a night that changes one part is a
    one-line diff)."""
    parts = document["parts"]
    head = {k: v for k, v in document.items() if k != "parts"}
    lines = ["{"] + ["  %s: %s," % (json.dumps(k), json.dumps(v, ensure_ascii=False)) for k, v in head.items()]
    lines.append('  "parts": {')
    items = list(parts.items())
    for n, (k, v) in enumerate(items):
        lines.append("    %s: %s%s" % (json.dumps(k, ensure_ascii=False), json.dumps(v, ensure_ascii=False), "," if n + 1 < len(items) else ""))
    lines += ["  }", "}"]
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--qucs", default=next((b for b in DEFAULT_BINARIES if os.path.isfile(b)), DEFAULT_BINARIES[-1]))
    ap.add_argument("--library", default=DEFAULT_LIBRARY)
    ap.add_argument("--out", default=None, help="the results (the library's ngspice-tested.json unless given)")
    ap.add_argument("--only", default="", help="libraries by name, comma separated")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--baseline", default=None)
    ap.add_argument("--ngspice", default=shutil.which("ngspice") or "ngspice")
    args = ap.parse_args()
    library = os.path.abspath(args.library)
    out = args.out or os.path.join(library, "ngspice-tested.json")
    only = {s for s in args.only.split(",") if s}
    todo = parts_of(library, only)
    version = subprocess.run([args.ngspice, "-v"], capture_output=True, text=True).stdout
    m = re.search(r"ngspice-(\S+)", version)
    version = m.group(1) if m else "?"
    print("%d parts of %s, %d at a time, ngspice %s" % (len(todo), library, args.jobs, version), flush=True)

    work = queue.Queue()
    for item in todo:
        work.put(item)
    results = {}
    lock = threading.Lock()

    def worker():
        server = Server(os.path.abspath(args.qucs), library, args.ngspice)
        while True:
            try:
                lib, part = work.get_nowait()
            except queue.Empty:
                break
            began = time.time()
            try:
                outcome = test(server, lib, part)
            except (TimeoutError, RuntimeError, BrokenPipeError) as e:
                outcome = {"passes": False, "why": "the test did not end: %s" % e}
                server.stop()
                server = Server(os.path.abspath(args.qucs), library, args.ngspice)
            took = time.time() - began   # (not kept: the file would differ each night)
            with lock:
                results["%s/%s" % (lib, part)] = outcome
                n = len(results)
                if n % 50 == 0 or not outcome["passes"] or took > 10:
                    print("%5d/%d %s/%s: %s (%.1f s)" % (n, len(todo), lib, part, "passes" if outcome["passes"] else outcome["why"], took),
                          flush=True)
        server.stop()

    threads = [threading.Thread(target=worker) for _ in range(max(1, args.jobs))]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    passed = sum(1 for r in results.values() if r["passes"])
    untested = sum(1 for r in results.values() if r.get("untested"))
    document = {"tested": datetime.date.today().isoformat(), "ngspice": version,
                "how": "each part alone, each pin to ground through 1 MOhm, its operating point: placed, netlisted, converged",
                "passed": passed, "untested": untested, "of": len(results), "parts": dict(sorted(results.items()))}
    write(document, out)
    print("%d of %d parts pass (%d with no pins, not tested); written to %s" % (passed, len(results), untested, out))

    if args.baseline:
        with open(args.baseline, encoding="utf-8") as f:
            before = json.load(f).get("parts", {})
        worse = [k for k, v in before.items() if v.get("passes") and not results.get(k, {}).get("passes", True)]
        for k in worse:
            print("REGRESSION %s: %s" % (k, results[k].get("why")))
        if worse:
            sys.exit(1)


if __name__ == "__main__":
    main()
