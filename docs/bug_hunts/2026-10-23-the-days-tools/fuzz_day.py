#!/usr/bin/env python3
"""The day's tools fuzzed over --mcp-server: the wishlist's N1-N14, the FEM
tools of the three re-check rounds, the argument layer (a singular for a
plural, a number as text, keys that are nearly a field's name). Each call
timed; the server's death, a call over its time, a slow call and any answer
that is neither ok nor a refusal are written to the log with the calls
before it.

    fuzz_day.py <app> <home> <calls> <seed> <log>
"""
import json, os, random, select, shutil, subprocess, sys, time

EX = "/Users/meisam/git/Qucs-S_Enhancements/qucs-s-26.1.1/examples"
TOOLS = ["fem_describe", "fem_evaluate", "fem_edit", "fem_converge", "fem_model", "fem_plot", "fem_export",
         "read_pdf", "undo", "redo", "undo_history", "delete", "select", "move", "get_dataset", "import_data",
         "add_component", "add_ground" if False else "add_wire", "get_schematic", "get_state", "describe_tool",
         "batch", "run_script", "list_documents", "open_document", "get_settings", "get_text", "edit_text", "xml"]
WEIRD = [None, True, False, 0, -1, 1, 2, 3, 5, 99999999999, -99999999999, 1e308, -1e308, 0.5, -0.0, "", " ", "0", "-1",
         "1e999", "NaN", "inf", "2", "1-3", "all", "first", "last", "none", ",,,", "a" * 4000, "‮", "\U0001F600",
         "..", "../", "/etc/passwd", "coax.qfem", "one.sch", "nothing.qfem", "out/x.png", "es.C11", "es.normE",
         "1[m]", "1[mm]", "1e-10", "{}", "[]", [], [1, 2], [[0, 0]], [[0, 0], [1, 1]], {}, {"a": 1}, {"only": ["core"]},
         {"numbers": [2]}, [None], [{}], 2147483647, -2147483648, 1e-300]
TIMEOUT = 45


def values_for(prop):
    v = list(WEIRD)
    if isinstance(prop, dict):
        if "enum" in prop:
            v += prop["enum"] * 8
        t = prop.get("type")
        if t == "integer":
            v += [1, 2, 3, 0, -1, 7, 100000] * 2
        if t == "number":
            v += [0.1, 1, 2, 0.01, 1e-12] * 2
        if t == "boolean":
            v += [True, False] * 8
        if t == "array":
            v += [[2], [2, 4], ["es.C11"], [[0, 0]]] * 3
    return v


def main(app, home, calls, seed, log):
    rnd = random.Random(seed)
    for d in ["ws/out", "home", "settings", "cache"]:
        os.makedirs(os.path.join(home, d), exist_ok=True)
    for f in [EX + "/multiphysics/coax.qfem", EX + "/multiphysics/coax_rlgc.qfem"]:
        if os.path.exists(f):
            shutil.copy(f, os.path.join(home, "ws", os.path.basename(f)))
    env = dict(os.environ, HOME=home + "/home", QUCS_SETTINGS_DIR=home + "/settings", QUCS_CACHE_DIR=home + "/cache",
               QT_QPA_PLATFORM="offscreen", ASAN_OPTIONS="detect_leaks=0", UBSAN_OPTIONS="halt_on_error=0:print_stacktrace=1")
    err = open(os.path.join(home, "stderr.txt"), "w")
    p = subprocess.Popen([app, "--mcp-server", "--workspace", home + "/ws"], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=err, env=env)
    out = open(log, "a")
    buf = [b""]

    def send(m):
        p.stdin.write((json.dumps(m) + "\n").encode())
        p.stdin.flush()

    def recv(i, timeout):
        end = time.time() + timeout
        while True:
            while b"\n" in buf[0]:
                line, buf[0] = buf[0].split(b"\n", 1)
                try:
                    m = json.loads(line)
                except ValueError:
                    continue
                if m.get("id") == i:
                    return m
            left = end - time.time()
            if left <= 0:
                return "timeout"
            r, _, _ = select.select([p.stdout], [], [], left)
            if r:
                chunk = os.read(p.stdout.fileno(), 1 << 20)
                if not chunk:
                    return "dead"
                buf[0] += chunk

    def rss():
        try:
            return int(subprocess.run(["ps", "-o", "rss=", "-p", str(p.pid)], capture_output=True, text=True).stdout.strip() or 0) // 1024
        except ValueError:
            return 0

    send({"jsonrpc": "2.0", "id": 1, "method": "initialize",
          "params": {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "fuzz", "version": "1"}}})
    recv(1, 60)
    send({"jsonrpc": "2.0", "method": "notifications/initialized"})
    send({"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
    tools = {t["name"]: t for t in recv(2, 60)["result"]["tools"]}
    names = [t for t in TOOLS if t in tools]
    history = []
    n = 10
    for first in (("fem_model", {"action": "open", "path": "coax.qfem"}), ("new_document", {})):
        n += 1
        send({"jsonrpc": "2.0", "id": n, "method": "tools/call", "params": {"name": first[0], "arguments": first[1]}})
        recv(n, 60)
    counts = {}
    for k in range(calls):
        name = rnd.choice(names)
        props = tools[name]["inputSchema"].get("properties", {})
        keys = list(props)
        args = {}
        for key in rnd.sample(keys, rnd.randint(0, min(len(keys), 4))):
            args[key] = rnd.choice(values_for(props[key]))
        # The argument layer: a plural's singular, a number as text, a near miss.
        if keys and rnd.random() < 0.45:
            key = rnd.choice(keys)
            value = rnd.choice(values_for(props[key]))
            how = rnd.random()
            if how < 0.4 and (key.endswith("s")):
                args.pop(key, None)
                args[key[:-2] if key.endswith("es") and rnd.random() < 0.5 else key[:-1]] = value
            elif how < 0.7:
                args[key] = str(value) if not isinstance(value, (list, dict)) else value
            else:
                args.pop(key, None)
                args[key + rnd.choice(["s", "_", "X", ""])] = value
        if rnd.random() < 0.25 and name in ("fem_describe", "fem_evaluate", "fem_edit", "fem_converge"):
            args.setdefault("path", "coax.qfem")
        # Every so often put the window back in a usable state, so the calls
        # after it reach past "nothing is open".
        if k % 120 == 0:
            for t, a2 in (("fem_model", {"action": "open", "path": "coax.qfem"}),
                          ("new_document", {}),
                          ("add_component", {"type": "R", "x": 100, "y": 100}),
                          ("add_component", {"type": "C", "x": 200, "y": 100})):
                n += 1
                send({"jsonrpc": "2.0", "id": n, "method": "tools/call", "params": {"name": t, "arguments": a2}})
                recv(n, TIMEOUT)
        if name == "batch" and rnd.random() < 0.6:
            inner = rnd.choice(["get_state", "list_documents", "fem_describe", "add_component", "undo"])
            one = {"tool": inner, "arguments": {}}
            args = {rnd.choice(["calls", "call"]): one if rnd.random() < 0.5 else [one]}
        if name == "run_script" and rnd.random() < 0.8:
            args = {"script": rnd.choice(["qucs.call('get_state', {})", "1+1", "while(0){}", "qucs.call('fem_describe', {what: 'tree'})",
                                          "qucs.call('batch', {call: {tool: 'get_state', arguments: {}}})"]), "timeout": 5}
        for bad in ("save_as", "as", "to", "file"):
            if isinstance(args.get(bad), str) and (args[bad].startswith("/") or args[bad].startswith("~")):
                args[bad] = "out/x"
        n += 1
        t0 = time.time()
        send({"jsonrpc": "2.0", "id": n, "method": "tools/call", "params": {"name": name, "arguments": args}})
        r = recv(n, TIMEOUT)
        dt = time.time() - t0
        history.append((name, args))
        if r in ("timeout", "dead"):
            out.write("%s after %s %s (%.1f s); last: %s\n" % (r.upper(), name, json.dumps(args)[:400], dt, json.dumps(history[-4:])[:1800]))
            out.flush()
            print(r, name, json.dumps(args)[:300])
            break
        res = r.get("result") or {}
        kind = "rpc-error" if "error" in r else ("error" if res.get("isError") else "ok")
        counts[(name, kind)] = counts.get((name, kind), 0) + 1
        if dt > 8:
            out.write("SLOW %.1f s %s %s\n" % (dt, name, json.dumps(args)[:400]))
            out.flush()
        if kind == "rpc-error":
            out.write("RPC %s %s -> %s\n" % (name, json.dumps(args)[:300], json.dumps(r["error"])[:300]))
            out.flush()
        if k % 200 == 0:
            m = rss()
            out.write("# %d calls, rss %d MB\n" % (k, m))
            out.flush()
            if m > 6000:
                out.write("MEMORY %d MB after %s %s\n" % (m, name, json.dumps(args)[:300]))
                out.flush()
                break
    out.write("counts %s\n" % json.dumps({"%s:%s" % k: v for k, v in sorted(counts.items())}))
    out.write("rss %d MB at the end\n" % rss())
    out.flush()
    try:
        p.stdin.close()
        p.wait(timeout=60)
    except Exception:
        p.kill()
    out.write("exit %s\n" % p.returncode)
    for line in open(os.path.join(home, "stderr.txt"), errors="replace"):
        if "Sanitizer" in line or "runtime error" in line:
            out.write("SAN " + line)
    out.flush()


main(sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), sys.argv[5])
