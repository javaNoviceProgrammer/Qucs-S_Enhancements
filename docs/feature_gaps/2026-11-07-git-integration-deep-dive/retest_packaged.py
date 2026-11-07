#!/usr/bin/env python3
"""The review's faults, and what it left unchecked, on a packaged qucs-s:
retest.py <qucs-s binary> <empty work folder>. Own HOME, settings, trash and
workspace. A reader blocked on a pipe is seen at once, and let go."""
import json, os, queue, stat, subprocess, sys, threading, time
exe, wk = sys.argv[1], os.path.realpath(sys.argv[2])
CONTROL = len(sys.argv) > 3   # (an older build: no link to /dev/zero, the freeze part alone)
HOME = wk + "/home"; os.makedirs(HOME)
open(HOME + "/.gitconfig", "w").write("[user]\n\tname = Retest\n\temail = retest@example.invalid\n[init]\n\tdefaultBranch = main\n[protocol \"file\"]\n\tallow = always\n")
ENV = dict(os.environ, HOME=HOME, GIT_CONFIG_NOSYSTEM="1")
for k in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_CONFIG_GLOBAL"): ENV.pop(k, None)
results = []
def report(name, ok, note=""):
    results.append(ok); print(("ok   " if ok else "FAIL ") + name + (" - " + str(note)[:170] if note and not ok else ""), flush=True)
def git(repo, *a, check=True):
    return subprocess.run(["git", "-C", repo, *a], env=ENV, check=check, capture_output=True, text=True).stdout
class Server:
    def __init__(s, ws):
        s.n = 0; os.makedirs(ws + "-set", exist_ok=True); os.makedirs(ws + "-trash", exist_ok=True)
        env = dict(ENV, QT_QPA_PLATFORM="offscreen", QUCS_SETTINGS_DIR=ws + "-set", QUCS_NO_SHELL_ENV="1", QUCS_TRASH_DIR=ws + "-trash", QUCS_CLAUDE="/nonexistent/claude", QUCS_GH="/nonexistent/gh")
        s.p = subprocess.Popen([exe, "--mcp-server", "--workspace", ws], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, env=env, text=True)
        s.q = queue.Queue(); threading.Thread(target=s.read, daemon=True).start()
        s.rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}})
        s.p.stdin.write(json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}) + "\n"); s.p.stdin.flush()
    def read(s):
        for l in s.p.stdout: s.q.put(l)
    def rpc(s, m, params, limit=60):
        s.n += 1
        s.p.stdin.write(json.dumps({"jsonrpc": "2.0", "id": s.n, "method": m, "params": params}) + "\n"); s.p.stdin.flush()
        end = time.time() + limit
        while True:
            try: r = json.loads(s.q.get(timeout=max(0.1, end - time.time())))
            except queue.Empty: raise TimeoutError(m)
            if r.get("id") == s.n: return r
    def call(s, tool, **a):
        r = s.rpc("tools/call", {"name": tool, "arguments": a}).get("result", {})
        return bool(r.get("isError")), "\n".join(c.get("text", "") for c in r.get("content", []))
    def close(s):
        try: s.p.stdin.close(); s.p.wait(timeout=20)
        except Exception: s.p.kill()
class Watch:
    def __init__(s, pipes):
        s.pipes, s.hits, s.done = pipes, [], False; s.t = threading.Thread(target=s.run, daemon=True); s.t.start()
    def run(s):
        while not s.done:
            for p in s.pipes:
                try:
                    os.close(os.open(p, os.O_WRONLY | os.O_NONBLOCK)); s.hits.append(os.path.basename(p))
                except OSError: pass
            time.sleep(0.005)
    def stop(s): s.done = True; s.t.join(); return sorted(set(s.hits))
def repo(path, files={"a.txt": "one\ntwo\n"}):
    os.makedirs(path); git(path, "init", "-q")
    for n, t in files.items(): open(path + "/" + n, "w").write(t)
    git(path, "add", "-A"); git(path, "commit", "-q", "-m", "first"); return path

# ---- the freeze
ws = wk + "/ws"; os.makedirs(ws); prj = ws + "/pipes_prj"; os.makedirs(prj + "/Libraries/lib")
SCH = "<Qucs Schematic 27.0.6>\n<Components>\n</Components>\n<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n"
pipes = [prj + "/" + n for n in ("pipe.sch", "pipe.txt", "sub.sch", "part.v", "part.vhdl", "part.cir", "Libraries/lib/.qucs-library.json", "s.txt.cfg")]
for p in pipes: os.mkfifo(p)
os.symlink(prj + "/pipe.sch", prj + "/link.sch")
if not CONTROL: os.symlink("/dev/zero", prj + "/zero.sch")
open(prj + "/real.sch", "w").write(SCH); open(prj + "/s.txt", "w").write("text\n"); open(prj + "/Libraries/lib/model.va", "w").write("module m; endmodule\n")
open(prj + "/hostile.sch", "w").write(SCH.replace("<Components>\n", '<Components>\n<Sub SUB1 1 100 100 0 0 0 0 "sub.sch" 1>\n<SPICE X1 1 200 100 0 0 0 0 "part.cir" 1 "" 0 "yes" 0 "none" 0>\n<Verilog X2 1 300 100 0 0 0 0 "part.v" 0>\n<VHDL X3 1 400 100 0 0 0 0 "part.vhdl" 0>\n'))
w = Watch(pipes); s = Server(ws)
err, t = s.call("open_project", name=prj); report("a project holding pipes opens, nothing waits", not err and not w.hits, (t, w.hits))
err, t = s.call("git_status", path=prj); report("answering with it open", True)
for name in ("pipe.sch", "link.sch", "pipe.txt"):
    before = len(w.hits); err, t = s.call("open_document", path=prj + "/" + name); report("opening " + name + " is refused, nothing waits", err and len(w.hits) == before, (err, t, w.hits))
for name in ("real.sch", "hostile.sch", "s.txt"):
    before = len(w.hits); err, t = s.call("open_document", path=prj + "/" + name); report("opening " + name + ": nothing waits", len(w.hits) == before, (t, w.hits))
err, t = s.call("git_status", path=prj); report("still answering afterwards", True)
print("     readers blocked:", sorted(set(w.hits)) or "none", flush=True); s.close()
# Started again with that project: the review's second freeze.
before = len(w.hits); s = Server(ws); time.sleep(3)
err, t = s.call("open_project", name=prj); s.call("git_status", path=prj)
report("started again on that project, nothing waits", len(w.hits) == before, sorted(set(w.hits[before:])))
s.close(); w.stop()
if CONTROL:
    print(f"control: {sum(results)} of {len(results)}", flush=True); sys.exit(0)

ws = wk + "/ws3"; os.makedirs(ws); s = Server(ws)
# ---- the review's git faults
r = repo(ws + "/r", {"a.txt": "one\ntwo\n", "gone.txt": "bye\n", "amp[1].sch": "a\n", "amp1.sch": "b\n"})
for tool in ("git_stage", "git_discard"):
    err, t = s.call(tool, path=r, paths=["typo.sch"]); report(tool + " of a path that is nowhere fails", err and "typo.sch" in t, t)
err, t = s.call("git_stage", path=r, paths=["*.sch"]); report("git_stage takes no pattern", err, t)
err, t = s.call("git_commit", path=r, message="typo", paths=["typo.sch"]); report("git_commit's error shows no :(literal)", err and ":(literal)" not in t and "typo.sch" in t, t)
git(r, "commit", "-q", "--allow-empty", "-m", "an empty one")
for p in (r, r + "/a.txt"):
    err, t = s.call("git_show", path=p, commit="HEAD"); report("git_show of an empty commit, path " + os.path.basename(p), "an empty one" in t and "Retest" in t, t)
# ---- what it left unchecked
os.remove(r + "/gone.txt"); err, t = s.call("git_stage", path=r, paths=["gone.txt"]); report("a deletion staged", not err, t)
err, t = s.call("git_commit", path=r, message="gone", paths=["gone.txt"]); report("a staged deletion committed by name (fault 9)", not err and "gone.txt" not in git(r, "ls-files"), t)
open(r + "/w.txt", "w").write("w\n"); err, t = s.call("git_commit", path=r, message="all", paths=["."]); report('git_commit paths ["."] (fault 25)', not err and git(r, "log", "-1", "--format=%s").strip() == "all", t)
open(r + "/run.dat", "w").write("x\n"); err, t = s.call("git_ignore", path=r + "/run.dat"); report("git_ignore", not err and git(r, "check-ignore", "run.dat", check=False).strip() == "run.dat", t)
git(r, "add", "-A"); git(r, "commit", "-q", "-m", "ignore")
bare = wk + "/remote.git"; subprocess.run(["git", "init", "-q", "--bare", bare], env=ENV, check=True)
err, t = s.call("git_remote", path=r, action="add", name="origin", url=bare); report("git_remote add", not err, t)
err, t = s.call("git_remote", path=r, action="push"); report("git_remote push", not err and git(bare, "rev-parse", "main").strip() == git(r, "rev-parse", "HEAD").strip(), t)
err, t = s.call("git_tag", path=r, action="create", name="v1", message="one"); report("git_tag", not err and "v1" in t, t)
err, t = s.call("git_blame", path=r + "/a.txt"); report("git_blame", not err and "one" in t, t)
open(r + "/a.txt", "w").write("stashed\n"); err, t = s.call("git_stash", path=r, action="push", message="kept"); report("git_stash push", not err and open(r + "/a.txt").read() == "one\ntwo\n", t)
err, t = s.call("git_stash", path=r, action="pop"); report("git_stash pop", not err and open(r + "/a.txt").read() == "stashed\n", t)
err, t = s.call("git_discard", path=r, paths=["a.txt"]); report("git_discard", not err and open(r + "/a.txt").read() == "one\ntwo\n", t)
git(r, "checkout", "-q", "-b", "theirs"); open(r + "/a.txt", "w").write("theirs\n"); git(r, "commit", "-q", "-am", "theirs"); git(r, "checkout", "-q", "main"); open(r + "/a.txt", "w").write("mine\n"); git(r, "commit", "-q", "-am", "mine")
err, t = s.call("git_branch", path=r, action="merge", name="theirs"); report("a merge in conflict is said", "conflicts" in t, t)
err, t = s.call("git_resolve", path=r + "/a.txt"); report("git_resolve show", not err and "mine" in t and "theirs" in t, t)
err, t = s.call("git_resolve", path=r + "/a.txt", action="take_theirs"); report("git_resolve take_theirs", not err and open(r + "/a.txt").read() == "theirs\n", t)
err, t = s.call("git_commit", path=r, message="merged"); report("the merge committed", not err, t)
# ---- pattern names, and the repository's own programs
open(r + "/amp[1].sch", "w").write("edit\n"); open(r + "/amp1.sch", "w").write("edit\n"); err, t = s.call("git_discard", path=r, paths=["amp[1].sch"]); report("discard takes that file alone", not err and open(r + "/amp1.sch").read() == "edit\n" and open(r + "/amp[1].sch").read() == "a\n", t)
mark = wk + "/ran"; prog = wk + "/prog.sh"; open(prog, "w").write('#!/bin/sh\necho "$0 $*" >> "%s"\ncat\n' % mark); os.chmod(prog, 0o755)
f = repo(ws + "/flt", {"a.flt": "one\n"})
open(f + "/.gitattributes", "w").write("*.flt filter=a=b\n"); git(f, "add", "-A"); git(f, "commit", "-q", "-m", "attr")
open(f + "/.git/config", "a").write('[filter "a=b"]\n\tclean = %s\n' % prog); open(f + "/a.flt", "w").write("TWO\n")
s.call("git_status", path=f); s.call("git_diff", path=f); report("a filter named a=b does not run (fault 11)", not os.path.exists(mark))
g = repo(ws + "/late", {"a.txt": "one\n"}); s.call("git_status", path=g)
open(g + "/.git/config", "a").write('[filter "late"]\n\tclean = %s\n' % prog); open(g + "/.gitattributes", "w").write("* filter=late\n"); open(g + "/a.txt", "w").write("TWO\n")
s.call("git_status", path=g); s.call("git_diff", path=g); report("a filter added just after a look does not run", not os.path.exists(mark))
s.close()
print(f"{sum(results)} of {len(results)} as they should be", flush=True)
