#!/usr/bin/env python3
"""Arguments fuzzed for the presentation tools over --mcp-server: values of
the wrong type, out of range, huge, empty, odd text; each call timed, the
server's death or a call over its time recorded with the calls before it.

    fuzz_mcp.py <app> <home> <deck> <calls> <seed> <log>
"""
import json, os, random, select, shutil, subprocess, sys, time

TOOLS = ["read_pptx", "show_pptx", "export_image", "screenshot", "context_menu", "open_document", "list_documents", "get_state"]
WEIRD = [None, True, False, 0, -1, 1, 2, 3, 99999999999, -99999999999, 1e308, -1e308, 0.5, -0.0, "", " ", "0", "-1", "1e999", "NaN", "inf",
         "all", "first", "last", "next", "previous", "1-", "-3", "3-1", "1-99999999", "1,1,1,1,1", "1-3,2-4", "0-0", ",,,", "a" * 5000,
         "‮", "\U0001F600", "..", "../", "deck.pptx", "../deck.pptx", "nonexistent.pptx", "out/x.png", "out/x.pdf", "out/x.svg", "out/x",
         "out/.png", "out/x.PNG", "out/x.webp", "out/x.tiff", "out/x.jpeg", "out/a/b/c.png", [], [1, 2], {}, {"a": 1}, [None], "picture", "text",
         "fit", "fit width", "fit page", "200%", "0%", "-50%", "1e9%", "page", "width", "Lumerical", "Ring", "measured", "\\", "*", "?",
         "0,0,1,1", "0.5,0.5,0.1,0.1", "-1,-1,5,5", "1,1,0,0", "NaN,0,1,1", [0, 0, 1, 1], [0.2, 0.2, 0.5, 0.5], [-1, -1, 2, 2], [0, 0, 0, 0]]
TIMEOUT = 60
SL = [1, 2, 3, 4, 5, 6, 7, "1-3", "all", "2,4", "7", "first", "last", "next", "previous", "Cloud", "Chart"]
VALID = {
    "read_pptx": {"path": ["deck.pptx"], "slides": SL, "search": ["loss", "Bold", "x", ""], "image": [True, False], "region": [[0, 0, 0.5, 0.5], [0.25, 0.25, 0.5, 0.5]]},
    "show_pptx": {"path": ["deck.pptx"], "slide": SL, "find": ["Bold", "e", "", "zzz"], "find_next": ["next", "previous"], "zoom": [50, 100, 275, "fit", "fit width", "fit page", 400],
                  "region": [[0, 0, 0.5, 0.5], [0.4, 0.4, 0.2, 0.2]], "notes": [True, False], "sidebar": [True, False], "play": [True, False, "first"], "reload": [True], "copy": ["picture", "text"]},
    "export_image": {"path": ["deck.pptx"], "slides": SL, "save_as": ["out/a.png", "out/b.pdf", "out/c.webp", "out/d.jpeg", "out/e.tiff", "out/f.bmp", "out/g.svg"], "scale": [0.1, 1, 2, 20],
                     "colours": ["colour", "grayscale", "monochrome"], "format": ["png", "pdf", "svg", "eps", "pdf_tex"], "transparent": [True, False]},
    "screenshot": {"path": ["deck.pptx"], "slide": [1, 2, 3, 7, 8], "region": [[0, 0, 0.5, 0.5], [0.4, 0.4, 0.2, 0.2]], "area": ["paper", "screen", "window", "visible"]},
}


def values_for(prop):
    v = list(WEIRD)
    if isinstance(prop, dict):
        if "enum" in prop:
            v += prop["enum"] * 6
        t = prop.get("type")
        if t == "integer":
            v += [1, 2, 3, 4, 5, 7, 12, 0, -1, 100000] * 2
        if t == "number":
            v += [0.1, 1, 2, 4, 10, 0.0001, 1000] * 2
        if t == "boolean":
            v += [True, False] * 6
    return v


def main(app, home, deck, calls, seed, log):
    rnd = random.Random(seed)
    for d in ["ws/out", "home", "settings", "cache"]:
        os.makedirs(os.path.join(home, d), exist_ok=True)
    shutil.copy(deck, os.path.join(home, "ws", "deck.pptx"))
    env = dict(os.environ, HOME=home + "/home", QUCS_SETTINGS_DIR=home + "/settings", QUCS_CACHE_DIR=home + "/cache", QT_QPA_PLATFORM="offscreen",
               ASAN_OPTIONS="detect_leaks=0")
    err = open(os.path.join(home, "stderr.txt"), "w")
    p = subprocess.Popen([app, "--mcp-server", "--workspace", home + "/ws"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=err, env=env)
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

    send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "fuzz", "version": "1"}}})
    recv(1, 60)
    send({"jsonrpc": "2.0", "method": "notifications/initialized"})
    send({"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
    tools = {t["name"]: t for t in recv(2, 60)["result"]["tools"]}
    history = []
    n = 10
    send({"jsonrpc": "2.0", "id": n, "method": "tools/call", "params": {"name": "open_document", "arguments": {"path": "deck.pptx"}}})
    print("open:", str(recv(n, 60))[:200])
    counts = {}
    valid = os.environ.get("FUZZ_VALID") == "1"
    for k in range(calls):
        name = rnd.choice(TOOLS)
        props = tools[name]["inputSchema"].get("properties", {})
        args = {}
        keys = list(props)
        if valid and name in VALID:
            for key, choices in VALID[name].items():
                if rnd.random() < 0.45:
                    args[key] = rnd.choice(choices)
            if keys and rnd.random() < 0.5:
                key = rnd.choice(keys)
                args[key] = rnd.choice(values_for(props[key]))
        else:
          for key in rnd.sample(keys, rnd.randint(0, min(len(keys), 4))):
            args[key] = rnd.choice(values_for(props[key]))
        if name in ("read_pptx", "show_pptx", "export_image", "screenshot") and rnd.random() < 0.7:
            args.setdefault("path", "deck.pptx")
        if name == "screenshot" and rnd.random() < 0.6:
            args["slide"] = rnd.choice(WEIRD + [1, 2, 3])
        if name == "context_menu" and rnd.random() < 0.6:
            args = {"on": {"slide": rnd.choice(WEIRD + [1, 2, 3, 7, 8])}}
            if rnd.random() < 0.5:
                args["choose"] = rnd.choice(["Copy Slide", "Copy Text", "Next Slide", "Play", "Notes", "Find...", "Export...", "Print...", "Zoom In", "Fit Width", "Save As...", "Reload", "x", ""])
        sa = args.get("save_as")
        if isinstance(sa, str) and (sa.startswith("/") or sa.startswith("~")):
            args["save_as"] = "out/x.png"
        n += 1
        t0 = time.time()
        send({"jsonrpc": "2.0", "id": n, "method": "tools/call", "params": {"name": name, "arguments": args}})
        r = recv(n, TIMEOUT)
        dt = time.time() - t0
        history.append((name, args))
        if r in ("timeout", "dead"):
            out.write("%s after %s %s (%.1f s); last calls: %s\n" % (r.upper(), name, json.dumps(args)[:400], dt, json.dumps(history[-4:])[:1500]))
            out.flush()
            print(r, name, json.dumps(args)[:300])
            break
        res = r.get("result") or {}
        if "error" in r:
            kind = "rpc-error"
        else:
            kind = "error" if res.get("isError") else "ok"
        counts[(name, kind)] = counts.get((name, kind), 0) + 1
        text = " ".join(c.get("text", "") for c in res.get("content", []) if c.get("type") == "text")
        if dt > 10:
            out.write("SLOW %.1f s %s %s\n" % (dt, name, json.dumps(args)[:400]))
        if kind == "rpc-error":
            out.write("RPC %s %s -> %s\n" % (name, json.dumps(args)[:300], json.dumps(r["error"])[:300]))
        if kind == "ok" and name == "export_image" and isinstance(args.get("save_as"), str) is False and "save_as" in args:
            out.write("ODD ok with save_as %r: %s\n" % (args.get("save_as"), text[:200]))
        out.flush()
    print(sorted(counts.items()))
    try:
        p.stdin.close()
        p.wait(timeout=30)
    except Exception:
        p.kill()
    print("exit", p.returncode)
    out.write("run seed %d: %d calls, exit %s\n" % (seed, len(history), p.returncode))
    out.close()


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]), int(sys.argv[5]), sys.argv[6])
