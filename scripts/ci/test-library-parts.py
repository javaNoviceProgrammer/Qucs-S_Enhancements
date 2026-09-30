#!/usr/bin/env python3
"""Every part of Qucs-S's component libraries, run once under ngspice.

Each part is placed alone on a schematic, each of its pins tied to ground
through 1 MOhm (a DC path for every node), and its operating point run
with ngspice through `qucs-s --mcp-server`. A part passes when it is
placed, netlists and the operating point converges. This is a smoke test:
it finds a model the translation breaks or ngspice refuses - not one that
runs and does the wrong thing (a 741 whose tail current flows backwards).

So a part that passes, and is of a kind there is a bench for, is then put
in a circuit of its kind and its numbers checked against a range (the
assessment of 29 September, wishlist 3):
- an op-amp (its inputs, output and supplies named): a follower of 1 V and
  a gain of 11 of 0.5 V, on +-15 V - out within 5 %;
- a bipolar transistor: 10 V, 1 MOhm to the base and 1 kOhm to the
  collector (9 uA in), or 10 kOhm and 10 Ohm (0.9 mA in, a power part's)
  - at either, Vbe 0.1 to 1.6 V (germanium from 0.15) and beta 3 to 5000
  (a high-voltage switch's is 4 to 10; or saturated);
- a MOSFET: 10 V through 1 kOhm, Vgs 10 V - it conducts more than 1 mA;
- a JFET: 10 V through 1 kOhm, the gate at the source - it conducts;
- a diode: 10 V through 9.3 kOhm, about 1 mA - its forward drop 0.1 to
  4.5 V.
find_library_component and describe_part report each part's outcome
("ngspice") from the file this writes beside the libraries, the bench's
too, and "tested" lists only the parts that pass.

    python3 scripts/ci/test-library-parts.py [--qucs BIN] [--library DIR]
        [--out FILE] [--only OpAmps,LEDs] [--jobs 4] [--baseline FILE]
        [--no-benches] [--merge]

With --merge, the parts run (--only) replace theirs in the results file,
the others kept. With --baseline, exits 1 when a part (or its bench) that passed there
fails now, or is missing from the results (the nightly job's regression check). Each server gets
settings, a home and a workspace of its own in a temporary folder.
"""
import argparse
import collections
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
    """(library, part, its model's first line) for every <Component> of every
    .lib there."""
    found = []
    for name in sorted(os.listdir(library)):
        if not name.endswith(".lib"):
            continue
        lib = name[:-4]
        if only and lib not in only:
            continue
        with open(os.path.join(library, name), encoding="utf-8", errors="replace") as f:
            text = f.read()
        for m in re.finditer(r"<Component\s+([^>]+)>(.*?)</Component>", text, re.S):
            model = re.search(r"<Model>\s*(.*?)\s*(\n|</Model>)", m.group(2), re.S)
            found.append((lib, m.group(1).strip(), model.group(1).strip() if model else ""))
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


def test(server, lib, part, model="", benches=True):
    """The outcome of one part: placed, netlisted, its operating point (on
    a schematic of its own, closed after) - and, when it passes and there
    is a bench for its kind, the bench's."""
    server.call("new_document", {"kind": "schematic"})
    try:
        outcome = tried(server, lib, part)
    finally:
        server.call("close_document", {"unsaved": "discard"})
    if benches and outcome.get("passes"):
        ok, text = server.call("describe_part", {"library": lib, "part": part})
        try:
            described = json.loads(text) if ok else {}
        except ValueError:
            described = {}
        kind = kind_of(model, described)
        if kind:
            outcome["bench"] = bench(server, lib, part, kind, described)
    return outcome


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


# ---- The benches: a part in a circuit of its kind, its numbers in a range.

NEGATIVE_SUPPLY = re.compile(r"^(vee|vss|v-|vs-|neg|negrail|v(ee|ss|s-)\d*)$", re.I)
INVERTING = re.compile(r"^(inn|in-|-in|in_?n|in_?neg|negin|inv|vinn|vin-)$", re.I)


def kind_of(model, described):
    """What bench fits the part: op-amp, npn, pnp, nfet, pfet, njf, pjf,
    diode - or None."""
    placed = (described or {}).get("placed as", "")
    if placed in ("_BJT", "BJT"):
        return "pnp" if '"pnp"' in model else "npn"
    if placed in ("_MOSFET", "MOSFET"):
        return "pfet" if '"pfet"' in model else "nfet"
    if placed == "JFET":
        return "pjf" if '"pfet"' in model else "njf"
    if placed == "Diode":
        return "diode"
    roles = [p.get("role") for p in (described or {}).get("pins", [])]
    if roles.count("input") == 2 and roles.count("output") == 1 and roles.count("supply") >= 2:
        return "op-amp"
    return None


def op_nodes(answer):
    """The operating point's node voltages, by name in lower case."""
    out = {}
    for k, v in ((answer.get("operating point") or {}).get("nodes") or {}).items():
        key = k.lower()
        m = re.match(r"^v\((.*)\)$", key)
        out[m.group(1) if m else key] = v
    return out


def run_bench(server, calls):
    """The bench built (calls) and its operating point run: its node
    voltages, or why not."""
    server.call("new_document", {"kind": "schematic"})
    try:
        calls = calls + [{"tool": "add_component", "arguments": {"type": ".DC", "name": "DC1", "x": 100, "y": 100}}]
        ok, text = server.call("batch", {"calls": calls, "brief": True}, 120)
        if not ok:
            return None, "not built: " + first_line(text)
        ok, text = server.call("simulate", {"operating_point": True, "timeout": RUN_SECONDS}, RUN_SECONDS + 30)
        try:
            answer = json.loads(text.split("\n")[-1]) if ok else {}
        except ValueError:
            answer = {}
        if not (ok and answer.get("succeeded") and answer.get("operating point")):
            errors = answer.get("errors") or []
            why = errors[0].get("message", "") if errors and isinstance(errors[0], dict) else (answer.get("note") or text)
            return None, "its operating point fails: " + first_line(str(why))
        return op_nodes(answer), None
    finally:
        server.call("close_document", {"unsaved": "discard"})


def place(name, type_, x, y, properties=None, rotation=None):
    a = {"type": type_, "name": name, "x": x, "y": y}
    if properties:
        a["properties"] = properties
    if rotation is not None:
        a["rotation"] = rotation
    return {"tool": "add_component", "arguments": a}


def label(at, net):
    return {"tool": "set_label", "arguments": {"at": at, "name": net}}


def ground(at):
    return {"tool": "connect", "arguments": {"from": at, "to": "ground"}}


def source(name, x, value, plus):
    """A DC source, its + on net 'plus', its - on ground."""
    return [place(name, "Vdc", x, 800, {"U": value}), label(name + ".1", plus), ground(name + ".2")]


def resistor(name, x, value, a, b):
    """A resistor from net a to net b (None: ground)."""
    calls = [place(name, "R", x, 600, {"R": value}, 1)]
    for end, net in (("1", a), ("2", b)):
        calls.append(ground(name + "." + end) if net is None else label(name + "." + end, net))
    return calls


def the_part(lib, part):
    return place("X1", "Lib", 300, 300, {"Lib": lib, "Comp": part})


def within(value, want, share):
    return value is not None and abs(value - want) <= abs(want) * share


def bench(server, lib, part, kind, described):
    """The part in its bench: {"passes", "bench", "measured", "why"}."""
    pins = described.get("pins", [])
    number = lambda p: "X1.%d" % p["pin"]
    if kind == "op-amp":
        inputs = [p for p in pins if p.get("role") == "input"]
        inverting = [p for p in inputs if INVERTING.match(p.get("name", ""))]
        noninverting = [p for p in inputs if p not in inverting]
        supplies = [p for p in pins if p.get("role") == "supply"]
        negative = [p for p in supplies if NEGATIVE_SUPPLY.match(p.get("name", ""))]
        positive = [p for p in supplies if p not in negative]
        output = [p for p in pins if p.get("role") == "output"][0]
        if len(inverting) != 1 or len(noninverting) != 1 or not negative or not positive:
            return {"passes": False, "untested": True, "bench": "op-amp",
                    "why": "its pins' names do not say which input inverts, or which supply is negative"}
        others = [p for p in pins if p not in inputs + supplies + [output]]
        measured = {}
        for gain, vin in ((1, 0.5 * 2), (11, 0.5)):
            calls = [the_part(lib, part), label(number(noninverting[0]), "in"), label(number(output), "out")]
            calls += [label(number(p), "vp") for p in positive] + [label(number(p), "vn") for p in negative]
            for k, p in enumerate(others):   # (offset, compensation, mute: to ground through 1 MOhm, as in the smoke test)
                calls += [label(number(p), "o%d" % k)] + resistor("RO%d" % k, 900 + 100 * k, "1 MOhm", "o%d" % k, None)
            if gain == 1:
                calls.append(label(number(inverting[0]), "out"))
            else:
                calls += [label(number(inverting[0]), "fb")] + resistor("RF", 500, "10 kOhm", "out", "fb") + resistor("RG", 600, "1 kOhm", "fb", None)
            calls += source("VP", 100, "15 V", "vp") + source("VN", 200, "-15 V", "vn") + source("VIN", 300, "%g V" % vin, "in")
            nodes, why = run_bench(server, calls)
            name = "follower of 1 V" if gain == 1 else "gain of 11 of 0.5 V"
            if nodes is None:
                return {"passes": False, "bench": "op-amp", "why": "%s: %s" % (name, why)}
            out = nodes.get("out")
            measured[name] = out
            if not within(out, gain * vin, 0.05):
                return {"passes": False, "bench": "op-amp", "measured": measured,
                        "why": "%s on +-15 V gives %s V, not %g V within 5 %%" % (name, out, gain * vin)}
        return {"passes": True, "bench": "op-amp: follower of 1 V, gain of 11 of 0.5 V, on +-15 V", "measured": measured}
    if kind in ("npn", "pnp"):
        # Two bias points: about 9 uA into the base (a small-signal part's),
        # and about 0.9 mA (a power part's, whose model is fitted at amps and
        # may have next to no gain at microamps). Sane at either: it passes.
        # (Vbe from 0.1 V: a germanium part's is 0.15 to 0.3 V.)
        n = len(pins)
        what = "%s: 10 V, the base through 1 MOhm (collector 1 kOhm) or 10 kOhm (collector 10 Ohm)" % kind
        measured, whys = {}, []
        for rb, rc, point in (("1 MOhm", "1 kOhm", "at 9 uA"), ("10 kOhm", "10 Ohm", "at 0.9 mA")):
            ohms_b, ohms_c = (1e6, 1e3) if point == "at 9 uA" else (1e4, 10.0)
            if kind == "npn":
                calls = [the_part(lib, part), label("X1.1", "b"), label("X1.2", "c"), ground("X1.3")] + ([ground("X1.4")] if n >= 4 else [])
                calls += source("VCC", 100, "10 V", "vcc") + resistor("RC", 500, rc, "vcc", "c") + resistor("RB", 600, rb, "vcc", "b")
            else:
                calls = [the_part(lib, part), label("X1.1", "b"), label("X1.2", "c"), label("X1.3", "vcc")] + ([label("X1.4", "vcc")] if n >= 4 else [])
                calls += source("VCC", 100, "10 V", "vcc") + resistor("RC", 500, rc, "c", None) + resistor("RB", 600, rb, "b", None)
            nodes, why = run_bench(server, calls)
            if nodes is None:
                whys.append("%s: %s" % (point, why))
                continue
            vb, vc = nodes.get("b"), nodes.get("c")
            if vb is None or vc is None:
                whys.append("%s: no voltage at its base or collector" % point)
                continue
            vbe, vce = (vb, vc) if kind == "npn" else (10 - vb, 10 - vc)
            here = {"Vbe": round(vbe, 4), "Vce": round(vce, 4)}
            measured[point] = here
            if not 0.1 <= vbe <= 1.6:
                whys.append("%s: Vbe is %.3f V, not 0.1 to 1.6 V" % (point, vbe))
                continue
            if vce < -0.05 or vce > 10.05:
                whys.append("%s: Vce is %.3f V, outside the supply" % (point, vce))
                continue
            if vce > 0.3:
                beta = ((10 - vce) / ohms_c) / ((10 - vbe) / ohms_b)
                here["beta"] = round(beta, 1)
                if not 3 <= beta <= 5000:
                    whys.append("%s: beta is %.1f, not 3 to 5000" % (point, beta))
                    continue
            else:
                here["saturated"] = True
            return {"passes": True, "bench": what, "measured": measured}
        return {"passes": False, "bench": what, "measured": measured, "why": "; ".join(whys)}
    if kind in ("nfet", "pfet", "njf", "pjf"):
        fet = kind in ("nfet", "pfet")
        n_type = kind in ("nfet", "njf")
        n = len(pins)
        if n_type:
            calls = [the_part(lib, part), label("X1.2", "d"), ground("X1.3")] + ([ground("X1.4")] if n >= 4 else [])
            calls += [label("X1.1", "g"), *source("VG", 200, "10 V", "g")] if fet else [ground("X1.1")]
            calls += source("VDD", 100, "10 V", "vdd") + resistor("RD", 500, "1 kOhm", "vdd", "d")
        else:
            calls = [the_part(lib, part), label("X1.2", "d"), label("X1.3", "vdd")] + ([label("X1.4", "vdd")] if n >= 4 else [])
            calls += [ground("X1.1")] if fet else [label("X1.1", "vdd")]
            calls += source("VDD", 100, "10 V", "vdd") + resistor("RD", 500, "1 kOhm", "d", None)
        nodes, why = run_bench(server, calls)
        what = "%s: 10 V through 1 kOhm, %s" % (kind, "|Vgs| 10 V" if fet else "the gate at the source")
        if nodes is None:
            return {"passes": False, "bench": what, "why": why}
        vd = nodes.get("d")
        if vd is None:
            return {"passes": False, "bench": what, "why": "no voltage at its drain"}
        current = (10 - vd) if n_type else vd   # mA, through 1 kOhm
        measured = {"Id mA": round(current, 4)}
        least = 1.0 if fet else 0.001
        if not least <= current <= 10.05:
            return {"passes": False, "bench": what, "measured": measured,
                    "why": "it conducts %.4f mA, not %g to 10 mA" % (current, least)}
        return {"passes": True, "bench": what, "measured": measured}
    if kind == "diode":
        calls = [the_part(lib, part), label("X1.2", "a"), ground("X1.1")]
        calls += source("VS", 100, "10 V", "vs") + resistor("R1", 500, "9.3 kOhm", "vs", "a")
        nodes, why = run_bench(server, calls)
        what = "diode: 10 V through 9.3 kOhm, about 1 mA forward"
        if nodes is None:
            return {"passes": False, "bench": what, "why": why}
        va = nodes.get("a")
        measured = {"Vf": None if va is None else round(va, 4)}
        if va is None or not 0.1 <= va <= 4.5:
            return {"passes": False, "bench": what, "measured": measured, "why": "its forward drop is %s V, not 0.1 to 4.5 V" % va}
        return {"passes": True, "bench": what, "measured": measured}
    return None


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
    ap.add_argument("--no-benches", action="store_true", help="the smoke test alone")
    ap.add_argument("--merge", action="store_true", help="the parts run replace theirs in the results file; the others are kept")
    args = ap.parse_args()
    library = os.path.abspath(args.library)
    out = args.out or os.path.join(library, "ngspice-tested.json")
    only = {s for s in args.only.split(",") if s}
    todo = parts_of(library, only)
    # A part named twice within one library: every lookup by name finds the
    # first, and the second can never be placed or tested (13 were so, until
    # 30 September). Said, and the run fails at the end.
    counts = collections.Counter((lib, part) for lib, part, _ in todo)
    twice = sorted("%s/%s" % key for key, n in counts.items() if n > 1)
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
                lib, part, model = work.get_nowait()
            except queue.Empty:
                break
            began = time.time()
            try:
                outcome = test(server, lib, part, model, not args.no_benches)
            except (TimeoutError, RuntimeError, BrokenPipeError) as e:
                outcome = {"passes": False, "why": "the test did not end: %s" % e}
                server.stop()
                server = Server(os.path.abspath(args.qucs), library, args.ngspice)
            took = time.time() - began   # (not kept: the file would differ each night)
            with lock:
                results["%s/%s" % (lib, part)] = outcome
                n = len(results)
                b = outcome.get("bench") or {}
                if n % 50 == 0 or not outcome["passes"] or took > 10 or (b and not b.get("passes")):
                    said = outcome["why"] if not outcome["passes"] else "passes" + (
                        "" if not b else "; bench %s" % ("passes" if b.get("passes") else "fails: " + b.get("why", "")))
                    print("%5d/%d %s/%s: %s (%.1f s)" % (n, len(todo), lib, part, said, took), flush=True)
        server.stop()

    threads = [threading.Thread(target=worker) for _ in range(max(1, args.jobs))]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    if args.merge and os.path.isfile(out):
        with open(out, encoding="utf-8") as f:
            kept = json.load(f).get("parts", {})
        kept.update(results)
        results = kept
    passed = sum(1 for r in results.values() if r["passes"])
    untested = sum(1 for r in results.values() if r.get("untested"))
    benched = [r["bench"] for r in results.values() if r.get("bench") and not r["bench"].get("untested")]
    bench_passed = sum(1 for b in benched if b.get("passes"))
    document = {"tested": datetime.date.today().isoformat(), "ngspice": version,
                "how": "each part alone, each pin to ground through 1 MOhm, its operating point: placed, netlisted, converged; "
                       "then, for an op-amp, a transistor, a FET or a diode, a bench of its kind with its numbers in a range",
                "passed": passed, "untested": untested, "of": len(results),
                "benches": {"passed": bench_passed, "of": len(benched)}, "parts": dict(sorted(results.items()))}
    write(document, out)
    print("%d of %d parts pass (%d with no pins, not tested); benches: %d of %d pass; written to %s"
          % (passed, len(results), untested, bench_passed, len(benched), out))

    for k in twice:
        print("DUPLICATE %s: named twice in its library - only the first can be placed or tested" % k)

    if args.baseline:
        with open(args.baseline, encoding="utf-8") as f:
            before = json.load(f).get("parts", {})
        # The parts of the libraries run this time (--only: those alone).
        # One that passed there and is not in this run's results is a
        # regression too: its library no longer loads, or it is gone - a
        # part missing counted as passing, and the job stayed green.
        ran = {k for k in before if not only or k.split("/", 1)[0] in only}
        worse = []
        for k in sorted(ran):
            v, now = before[k], results.get(k)
            bench_before = v.get("bench") or {}
            if not v.get("passes") and not bench_before.get("passes"):
                continue
            if now is None:
                worse.append((k, "not in this run's results: its library did not load, or the part is gone"))
            elif v.get("passes") and not now.get("passes"):
                worse.append((k, now.get("why")))
            elif bench_before.get("passes") and not args.no_benches:
                # (A bench that passed there and fails now - a model that
                # runs and now does the wrong thing - or is not run now.)
                bench_now = now.get("bench") or {}
                if not bench_now or bench_now.get("untested"):
                    worse.append((k, "its bench was not run: %s" % (bench_now.get("why") or "no bench for it this time")))
                elif not bench_now.get("passes"):
                    worse.append((k, bench_now.get("why")))
        for k, why in worse:
            print("REGRESSION %s: %s" % (k, why))
        if worse:
            sys.exit(1)
    if twice:
        sys.exit(1)


if __name__ == "__main__":
    main()
