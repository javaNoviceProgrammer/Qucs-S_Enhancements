#!/usr/bin/env python3
"""The review's faults, and what it left unchecked, on a packaged qucs-s:
retest.py <qucs-s binary> <empty work folder>. Own HOME, settings, trash and
workspace. A reader blocked on a pipe is seen at once, and let go."""
import json, os, queue, stat, subprocess, sys, threading, time
exe, wk = sys.argv[1], os.path.realpath(sys.argv[2])
MODE = sys.argv[3] if len(sys.argv) > 3 else ""
CONTROL = MODE == "control"   # (an older build: no link to /dev/zero, the freeze part alone)
SECOND = MODE == "second"     # (the second review's part alone; "third", the check's; "worktree", New Worktree's check)
HOME = wk + "/home"; os.makedirs(HOME)
open(HOME + "/.gitconfig", "w").write("[user]\n\tname = Retest\n\temail = retest@example.invalid\n[init]\n\tdefaultBranch = main\n[protocol \"file\"]\n\tallow = always\n")
ENV = dict(os.environ, HOME=HOME, GIT_CONFIG_NOSYSTEM="1")
for k in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_CONFIG_GLOBAL"): ENV.pop(k, None)
results = []
def report(name, ok, note=""):
    ok = bool(ok); results.append(ok); print(("ok   " if ok else "FAIL ") + name + (" - " + str(note)[:170] if note and not ok else ""), flush=True)
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

def second_review(ws):
    """The review at 86a8458 (2026-11-07-claude-git-integration-review.md)."""
    s = Server(ws)
    def lines(r): return git(r, "log", "-1", "--format=%s").strip()
    def state(t):
        try: return json.loads(t)
        except Exception: return {}
    # 1. A path outside the repository.
    r = repo(ws + "/out", {"a.txt": "a\n", "b.txt": "b\n", "c.txt": "c\n"}); open(ws + "/outside.txt", "w").write("x\n")
    for n in "abc": open(r + "/" + n + ".txt", "w").write("edit\n")
    head = git(r, "rev-parse", "HEAD")
    err, t = s.call("git_commit", path=r, message="only a", paths=["a.txt", "../outside.txt"]); report("1 a commit with a path outside commits nothing", err and git(r, "rev-parse", "HEAD") == head, t)
    # 2. Pull on diverged branches.
    b = ws + "/d/o.git"; os.makedirs(ws + "/d"); subprocess.run(["git", "init", "-q", "--bare", b], env=ENV, check=True)
    one = repo(ws + "/d/one", {"f.txt": "base\n", "g.txt": "base\n"}); git(one, "remote", "add", "origin", b); git(one, "push", "-q", "-u", "origin", "main")
    two = ws + "/d/two"; subprocess.run(["git", "clone", "-q", b, two], env=ENV, check=True)
    def both(file, mine, theirs):
        open(two + "/" + file, "w").write(mine); git(two, "commit", "-q", "-am", "mine")
        open(one + "/" + file, "w").write(theirs); git(one, "commit", "-q", "-am", "theirs"); git(one, "pull", "-q", "--no-rebase", "--no-edit"); git(one, "push", "-q")
    open(two + "/m.txt", "w").write("m\n"); git(two, "add", "-A"); git(two, "commit", "-q", "-m", "mine")
    open(one + "/g.txt", "w").write("theirs\n"); git(one, "commit", "-q", "-am", "theirs"); git(one, "push", "-q")
    err, t = s.call("git_remote", path=two, action="pull"); report("2 a pull of diverged branches merges", not err and len(git(two, "log", "-1", "--format=%P").split()) == 2, t)
    both("f.txt", "mine\n", "theirs\n")
    err, t = s.call("git_remote", path=two, action="pull"); report("2 a pull in conflict is said", not err and state(t).get("conflicts") == ["f.txt"], t)
    err, t = s.call("git_abort", path=two)
    git(two, "config", "pull.rebase", "true"); err, t = s.call("git_remote", path=two, action="pull"); report("2 pull.rebase stands", state(t).get("under way") == "rebase", t)
    # 9. A rebase goes on.
    err, t = s.call("git_resolve", path=two + "/f.txt", action="take_theirs")
    err, t = s.call("git_commit", path=two, message="not this"); report("9 a commit in a rebase is refused", err and "git_continue" in t, t)
    err, t = s.call("git_continue", path=two); report("9 git_continue finishes the rebase", not err and "under way" not in state(t) and state(t).get("branch") == "main", t)
    # 3. A name with a space.
    r = repo(ws + "/sp", {"sp": "one\n", "sp ": "one\n"})
    for n in ("sp", "sp "): open(r + "/" + n, "w").write("edit\n")
    err, t = s.call("git_discard", path=r, paths=["sp "]); report("3 discard of 'sp ' takes that file", not err and open(r + "/sp ").read() == "one\n" and open(r + "/sp").read() == "edit\n", t)
    # 5. An untracked folder.
    os.makedirs(r + "/new")
    for n in "xyz": open(r + "/new/" + n + ".sch", "w").write(n)
    err, t = s.call("git_discard", path=r, paths=["new/typo.sch"]); report("5 a name in an untracked folder that is no file is said", err, t)
    err, t = s.call("git_discard", path=r, paths=["new/x.sch", "new/y.sch"]); report("5 discard takes each file in an untracked folder", not err and not os.path.exists(r + "/new/x.sch") and not os.path.exists(r + "/new/y.sch") and os.path.exists(r + "/new/z.sch"), t)
    # 4. Discard all in a merge.
    r = repo(ws + "/m", {"d.txt": "d\n", "z.txt": "z\n"}); git(r, "checkout", "-q", "-b", "t"); open(r + "/d.txt", "w").write("c\n"); open(r + "/new.txt", "w").write("t\n"); git(r, "add", "-A"); git(r, "commit", "-q", "-m", "t")
    git(r, "checkout", "-q", "main"); git(r, "rm", "-q", "d.txt"); open(r + "/new.txt", "w").write("m\n"); git(r, "add", "-A"); git(r, "commit", "-q", "-m", "m"); git(r, "merge", "--no-edit", "t", check=False); open(r + "/z.txt", "w").write("edit\n")
    err, t = s.call("git_discard", path=r, all=True); report("4 discard all in a merge finishes", not err and open(r + "/z.txt").read() == "z\n" and not state(t).get("conflicts"), t)
    # 6, 7. .gitignore no plain file.
    r = repo(ws + "/ig", {"t.txt": "t\n"}); out = ws + "/ig-outside"; open(out, "w").write("kept\n"); os.symlink(out, r + "/.gitignore")
    err, t = s.call("git_ignore", path=r + "/t.txt"); report("7 git_ignore writes nothing through a link", err and open(out).read() == "kept\n", (t, open(out).read()))
    os.remove(r + "/.gitignore"); os.mkfifo(r + "/.gitignore"); w = Watch([r + "/.gitignore"])
    try:
        err, t = s.call("git_ignore", path=r + "/t.txt")
    except TimeoutError:   # (an older build: it waits for ever, so it is started anew)
        err, t = False, "no answer"; s.p.kill(); os.makedirs(ws + "-again"); s = Server(ws + "-again")
    hits = w.stop(); report("6 a pipe named .gitignore: answered, nothing waits", err and not hits, (t, hits)); os.remove(r + "/.gitignore")
    # 8. A commit already applied.
    r = repo(ws + "/ap", {"a.txt": "a\n"}); git(r, "checkout", "-q", "-b", "side"); open(r + "/a.txt", "w").write("same\n"); git(r, "commit", "-q", "-am", "the change"); h = git(r, "rev-parse", "HEAD").strip()
    git(r, "checkout", "-q", "main"); open(r + "/a.txt", "w").write("same\n"); git(r, "commit", "-q", "-am", "by hand")
    err, t = s.call("git_commit_action", path=r, commit=h, action="cherry_pick"); report("8 no conflict is called one", "conflicts:" not in state(t).get("cherry_pick", "") and "git_continue" in t, t)
    err, t = s.call("git_continue", path=r, skip=True); report("8 git_continue skip goes past it", not err and "under way" not in state(t), t)
    # The smaller ones.
    r = repo(ws + "/sm", {"a.txt": "a\n", "main": "a file\n"})
    err, t = s.call("git_unstage", path=r, paths=["typo.txt"]); report("unstage of a name that is no file is said", err, t)
    err, t = s.call("git_status", path=r + "/nowhere.txt"); report("git_status of a file not there", "as committed" not in state(t).get("path", {}).get("state", ""), t)
    err, t = s.call("git_log", path=r, ref="no-such-ref"); report("git_log of an unknown ref fails", err, t)
    err, t = s.call("git_stash", path=r, action="show", index=5); report("git_stash show of a stash not there fails", err, t)
    err, t = s.call("git_log", path=r, ref="main"); report("git_log of a ref a file is named as", not err and state(t).get("commits"), t)
    err, t = s.call("git_show", path=r, commit="main"); report("git_show of a commit a file is named as", not err and "first" in t, t)
    err, t = s.call("git_commit_action", path=r, commit="main", action="reset", mode="mixed"); report("reset to a commit a file is named as", not err, t)
    open(r + "/only-new.txt", "w").write("stashed line\n"); s.call("git_stash", path=r, action="push"); err, t = s.call("git_stash", path=r, action="show"); report("git_stash show of new files alone", "stashed line" in t, t); s.call("git_stash", path=r, action="drop")
    git(r, "mv", "a.txt", "b.txt"); err, t = s.call("git_commit", path=r, message="moved", paths=["b.txt"]); report("a rename committed by its new name", not err and not state(t).get("staged") and "a.txt" not in git(r, "ls-tree", "--name-only", "HEAD"), t)
    open(r + "/long.txt", "w").write("line\n" * 2100); git(r, "add", "-A"); git(r, "commit", "-q", "-m", "long")
    err, t = s.call("git_blame", path=r + "/long.txt"); report("blame's cut is said", "more" in state(t), t[:120])
    f = ws + "/fresh"; os.makedirs(f); git(f, "init", "-q"); err, t = s.call("git_unstage", path=f, all=True); report("unstage all before a first commit", not err, t)
    os.makedirs(r + "/fold"); err, t = s.call("git_resolve", path=r + "/fold"); report("git_resolve of a folder says so", err and "folder" in t, t)
    sl = ws + "/slash.git"; subprocess.run(["git", "init", "-q", "--bare", sl], env=ENV, check=True); git(r, "remote", "add", "up", sl); git(r, "push", "-q", "up", "HEAD:main")
    err, t = s.call("git_clone", url=sl + "/"); report("a clone URL ending in a slash", not err and os.path.isdir(ws + "/slash"), t)
    git(r, "remote", "add", "my/remote", sl); git(r, "update-ref", "refs/remotes/my/remote/feature", "HEAD"); err, t = s.call("git_branch", path=r, action="switch", name="my/remote/feature"); report("a remote with a slash in its name", state(t).get("branch") == "feature", t)
    s.close()

def third_review(ws):
    """The check of dd7cfc4 (2026-11-07-claude-git-fixes-check.md)."""
    s = Server(ws)
    def in_commit(r): return sorted(git(r, "show", "--name-only", "--format=", "--no-renames", "HEAD").split())
    def staged(r): return git(r, "diff", "--cached", "--name-status", "--no-renames").split()
    def conflict(r, name):
        git(r, "checkout", "-q", "-b", "theirs"); open(r + "/" + name, "w").write("theirs\n"); git(r, "add", "-A"); git(r, "commit", "-q", "-m", "theirs")
        git(r, "checkout", "-q", "main"); open(r + "/" + name, "w").write("mine\n"); git(r, "add", "-A"); git(r, "commit", "-q", "-m", "mine")
        git(r, "merge", "-q", "theirs", check=False)
    # Regression 1: a new file at a rename's old name.
    r = repo(ws + "/r1", {"old.txt": "the text of the file\nas it stood\n"}); git(r, "mv", "old.txt", "new.txt"); open(r + "/old.txt", "w").write("nothing like it\n")
    err, t = s.call("git_commit", path=r, message="r", paths=["new.txt"])
    report("R1 a file at the old name is not committed", not err and in_commit(r) == ["new.txt"] and "nothing like it" not in git(r, "show", "HEAD"), (t, in_commit(r)))
    r = repo(ws + "/r1b", {"old.txt": "the text of the file\nas it stood\n"}); git(r, "mv", "old.txt", "new.txt"); open(r + "/old.txt", "w").write("nothing like it\n")
    err, t = s.call("git_unstage", path=r, paths=["new.txt"]); report("R1 nor taken back in unstage", not err and "D" in staged(r) and open(r + "/old.txt").read() == "nothing like it\n", (t, staged(r)))
    # Regressions 2, 3: what failed says so.
    r = repo(ws + "/r2", {"a.txt": "one\n", "b.txt": "b\n", "o": "a file named o\n"})
    git(r, "checkout", "-q", "-b", "side"); open(r + "/b.txt", "w").write("side\n"); git(r, "commit", "-q", "-am", "side"); git(r, "checkout", "-q", "main"); open(r + "/a.txt", "w").write("two\n"); git(r, "commit", "-q", "-am", "main")
    git(r, "merge", "-q", "--no-commit", "--no-ff", "side")
    for commit, action in (("nosuchcommit", "cherry_pick"), ("nosuchcommit", "revert"), ("o", "cherry_pick")):
        err, t = s.call("git_commit_action", path=r, commit=commit, action=action); report(f"R2 {action} of {commit} in a merge is an error", err and "nothing to apply" not in t, t)
    r = repo(ws + "/r3", {"a.txt": "one\n"}); conflict(r, "a.txt")
    for action in ("reset", "check_out", "cherry_pick"):
        err, t = s.call("git_commit_action", path=r, commit="nosuchcommit", action=action); report(f"R3 {action} of a bad commit in a conflict is an error", err and "resolve the files" not in t, t)
    # Regression 4: either spelling of a name.
    for name, given, what in (("\u00e9.txt", "e\u0301.txt", "composed"), ("e\u0301\U0001F600.txt", "\u00e9\U0001F600.txt", "decomposed, with an emoji")):
        r = repo(ws + "/r4" + what[:3], {"a.txt": "one\n"}); conflict(r, name)
        err, t = s.call("git_resolve", path=r + "/" + given); report(f"R4 shown by the other spelling ({what})", not err and "not in conflict" not in t, t)
        err, t = s.call("git_resolve", path=r + "/" + given, action="take_theirs")
        report(f"R4 resolved by the other spelling ({what})", not err and not [l for l in git(r, "status", "--porcelain").splitlines() if l[:2] in ("UU", "AA")], (t, git(r, "status", "--porcelain")))
    # 5: both added, discarded.
    r = repo(ws + "/b5", {"a.txt": "one\n"}); conflict(r, "new.txt")
    err, t = s.call("git_discard", path=r, paths=["new.txt"])
    report("5 both added and discarded keeps the branch's file", not err and os.path.exists(r + "/new.txt") and open(r + "/new.txt").read() == "mine\n" and git(r, "status", "--porcelain", "--", "new.txt").strip() == "", (t, git(r, "status", "--porcelain")))
    # 7: a pipe named .gitignore, asked once.
    r = repo(ws + "/b7", {"a.txt": "one\n"}); os.mkfifo(r + "/.gitignore"); t0 = time.time()
    try: s.call("git_status", path=r); took = time.time() - t0
    except TimeoutError: took = 999
    report("7 status of a pipe named .gitignore is not asked twice", took < 45, f"{took:.0f} s"); print(f"     it took {took:.0f} s", flush=True); os.remove(r + "/.gitignore")
    # 8: the rename's gaps.
    r = repo(ws + "/b8a", {"doomed.txt": "".join(f"line {i}\n" for i in range(30))}); git(r, "rm", "-q", "doomed.txt"); open(r + "/b.txt", "w").write("".join(f"line {i}\n" for i in range(29)))
    err, t = s.call("git_commit", path=r, message="b", paths=["b.txt"]); report("8 an unrelated deletion stays out of a commit", not err and in_commit(r) == ["b.txt"] and "D" in staged(r), (t, in_commit(r)))
    r = repo(ws + "/b8b", {"f.txt": "f\n"}); os.makedirs(r + "/o"); open(r + "/o/x.txt", "w").write("the text\nof x\n"); git(r, "add", "-A"); git(r, "commit", "-q", "-m", "x"); os.makedirs(r + "/n"); git(r, "mv", "o/x.txt", "n/x.txt")
    err, t = s.call("git_commit", path=r, message="n", paths=["n"]); report("8 a rename committed by its folder", not err and in_commit(r) == ["n/x.txt", "o/x.txt"] and not staged(r), (t, in_commit(r), staged(r)))
    r = repo(ws + "/b8c", {"f.txt": "f\n"}); os.makedirs(r + "/o"); open(r + "/o/x.txt", "w").write("the text\nof x\n"); git(r, "add", "-A"); git(r, "commit", "-q", "-m", "x"); os.makedirs(r + "/n"); git(r, "mv", "o/x.txt", "n/x.txt")
    err, t = s.call("git_unstage", path=r, paths=["n"]); report("8 a rename unstaged by its folder", not err and not staged(r), (t, staged(r)))
    r = repo(ws + "/b8d", {"old.txt": "the text of the file\nas it stood\n"}); git(r, "mv", "old.txt", "new.txt")
    err, t = s.call("git_commit", path=r, message="o", paths=["old.txt"]); report("8 a rename committed by its old name", not err and in_commit(r) == ["new.txt", "old.txt"] and not staged(r), (t, in_commit(r), staged(r)))
    # 9: the smaller ones.
    for i, given in enumerate((["new", "new/x"], [".", "new/x"])):
        r = repo(ws + f"/b9{i}", {"a.txt": "one\n"}); os.makedirs(r + "/new"); open(r + "/new/x", "w").write("x\n"); open(r + "/new/y", "w").write("y\n")
        err, t = s.call("git_discard", path=r, paths=given); report(f"9 discard of {given} takes the folder", not err and not os.path.exists(r + "/new"), (t, os.path.exists(r + "/new/y")))
    r = repo(ws + "/b9u", {"Amp.txt": "a\n"}); open(r + "/Amp.txt", "w").write("b\n"); git(r, "add", "-A")
    err, t = s.call("git_unstage", path=r, paths=["amp.txt"]); report("9 unstage in another case of the name", not err and not staged(r), (t, staged(r)))
    r = repo(ws + "/b9m", {"a.txt": "one\n"}); conflict(r, "a.txt")
    err, t = s.call("git_continue", path=r, skip=True); report("9 a merge has no commit to skip", err and "resolve the files" not in t, t)
    r = repo(ws + "/b9s", {"a.txt": "one\n"}); open(r + "/a.txt", "w").write("two\n")
    err, t = s.call("git_stage", paths=[r + "/a.txt "]); report("9 a name with a space at its end is that name, however given", err and not staged(r), (t, staged(r)))
    r = repo(ws + "/b9t", {"a.txt": "one\n"}); open(r + "/a.txt", "w").write("two\n")
    err, t = s.call("git_stage", path=r, paths=["a.txt "]); report("9 and given from the folder", err and not staged(r), (t, staged(r)))
    s.close()

def worktree_check(ws):
    """The check of New Worktree... at 3f2422ab (2026-11-08-claude-new-worktree-check.md)."""
    s = Server(ws)
    def wait(cond, limit=30):
        end = time.time() + limit
        while time.time() < end:
            if cond(): return True
            time.sleep(0.2)
        return cond()
    def worktrees(r): return [l[9:] for l in git(r, "worktree", "list", "--porcelain").splitlines() if l.startswith("worktree ")]
    def ask(doc, branch, folder=None):
        """New Worktree... on doc's repository: branch, then folder, typed; Make Worktree pressed if it can be.
        Gives whether it could, the dialog's texts, and the answer."""
        err, t = s.call("trigger_action", action="Git > New Worktree...", path=doc)
        if err: return None, [t], t
        # Typed as a user would: set_dialog takes the first item of the list that holds the text.
        x, y, w, h = [c for c in json.loads(s.call("get_dialog")[1])["controls"] if c.get("label") == "Branch"][0]["at"]
        s.call("send_input", target="dialog", click=[x + w / 2, y + h / 2], keys="Ctrl+A", text=branch)
        if folder is not None: s.call("set_dialog", set=[{"control": "Folder", "value": folder}])
        d = json.loads(s.call("get_dialog")[1])
        if [c for c in d["controls"] if c.get("label") == "Branch"][0]["value"] != branch: return None, ["typed " + branch + ", shown otherwise"] + d.get("texts", []), ""
        ok = [c for c in d.get("controls", []) if c.get("label") == "Make Worktree"][0].get("enabled", True)
        answer = s.call("set_dialog", press="Make Worktree")[1] if ok else ""
        time.sleep(0.5)
        for _ in range(3):   # (what is still open, or opened after: read, and closed)
            t = s.call("get_dialog")[1]
            if not t.lstrip().startswith("{"): break
            o = json.loads(t); answer += " | " + o.get("title", "") + ": " + " ".join(o.get("texts", []))
            buttons = [c["label"] for c in o.get("controls", []) if c.get("kind") == "button"]
            s.call("set_dialog", press="Cancel" if "Cancel" in buttons else buttons[0])
        return ok, d.get("texts", []), answer
    # 1. A branch only on a remote.
    o = ws + "/o.git"; other = ws + "/other.git"
    for b in (o, other): subprocess.run(["git", "init", "-q", "--bare", b], env=ENV, check=True)
    p = repo(ws + "/proj", {"a.txt": "a\n"}); git(p, "remote", "add", "origin", o); git(p, "remote", "add", "other", other); git(p, "push", "-q", "origin", "main")
    c = ws + "/c"; subprocess.run(["git", "clone", "-q", o, c], env=ENV, check=True)
    git(c, "checkout", "-q", "-b", "feature"); open(c + "/f.txt", "w").write("f\n"); git(c, "add", "-A"); git(c, "commit", "-q", "-m", "f"); git(c, "push", "-q", "origin", "feature")
    git(c, "checkout", "-q", "-b", "dup"); git(c, "push", "-q", "origin", "dup"); git(c, "push", "-q", other, "dup")
    git(p, "fetch", "-q", "--all"); doc = p + "/a.txt"; s.call("open_document", path=doc)
    err, t = s.call("trigger_action", action="Git > New Worktree...", path=doc); d = json.loads(s.call("get_dialog")[1])
    report("4 the folder starts empty", [x for x in d["controls"] if x["label"] == "Folder"][0]["value"] == "", d); s.call("set_dialog", press="Cancel")
    ok, texts, a = ask(doc, "feature"); wt = ws + "/proj-feature"
    report("1 a remote's branch is made a local branch following it", ok and wait(lambda: os.path.exists(wt + "/f.txt")) and git(p, "rev-parse", "feature") == git(p, "rev-parse", "origin/feature") and git(p, "rev-parse", "--abbrev-ref", "feature@{u}").strip() == "origin/feature", (texts, a))
    git(p, "config", "branch.autoSetupMerge", "false")
    ok, texts, a = ask(doc, "dup"); report("1 a branch on two remotes: nothing made, it says so", not ok and not os.path.exists(ws + "/proj-dup"), texts)
    ok, texts, a = ask(doc, "origin/feature"); report("1 a remote's branch with a local one of its name: nothing made", not ok and len(worktrees(p)) == 2, texts)
    ok, texts, a = ask(doc, "other/dup"); wt = ws + "/proj-dup"
    report("1 other/dup chosen whole, followed though autoSetupMerge is off", ok and wait(lambda: os.path.isdir(wt)) and wait(lambda: git(p, "rev-parse", "--abbrev-ref", "dup@{u}", check=False).strip() == "other/dup"), (texts, a))
    # 2. Inside the repository by another spelling.
    git(p, "branch", "in1"); git(p, "branch", "in2")
    for name, folder in (("another case", os.path.dirname(p) + "/PROJ/wt"), ("the /tmp link", (p[len("/private"):] if p.startswith("/private/") else p) + "/wt2")):
        ok, texts, a = ask(doc, "in1" if "case" in name else "in2", folder); time.sleep(1)
        print("     " + name + ":", a[:200], flush=True)
        report("2 a folder inside the repository by " + name + " is refused", not os.path.exists(p + "/wt") and not os.path.exists(p + "/wt2") and "??" not in git(p, "status", "--porcelain"), (folder, texts, a))
    # 3. ~ is home.
    git(p, "branch", "home")
    ok, texts, a = ask(doc, "home", "~/wt-home")
    report("3 ~/ is the home folder", ok and wait(lambda: os.path.isdir(HOME + "/wt-home")) and not os.path.exists(ws + "/~"), (texts, a))
    # What worked still works.
    ok, texts, a = ask(doc, "main"); report("main, checked out here, can't be made", not ok, texts)
    ok, texts, a = ask(doc, "bad name"); report("a bad name is refused, with why", "cannot be a branch's name" in a and not git(p, "branch", "--list", "bad*").strip() and not os.path.exists(ws + "/proj-bad-name"), (texts, a))
    git(p, "branch", "kept"); ok, texts, a = ask(doc, "kept", "wt-rel")
    report("an existing branch, in a relative folder beside the repository", ok and wait(lambda: os.path.isdir(ws + "/wt-rel")) and git(ws + "/wt-rel", "rev-parse", "--abbrev-ref", "HEAD").strip() == "kept", (texts, a))
    ok, texts, a = ask(doc, "feat/x"); report("a new branch, its folder named for it", ok and wait(lambda: os.path.isdir(ws + "/proj-feat-x")) and git(ws + "/proj-feat-x", "rev-parse", "--abbrev-ref", "HEAD").strip() == "feat/x", (texts, a))
    # The recheck (2026-11-08-claude-new-worktree-recheck.md): set_dialog types into the list; Choose...; the File Browser's Git.
    def control(label):
        return [c for c in json.loads(s.call("get_dialog")[1]).get("controls", []) if c.get("label") == label][0]
    s.call("trigger_action", action="Git > New Worktree...", path=doc)
    s.call("set_dialog", set=[{"control": "Branch", "value": "featur"}]); report("recheck: set_dialog types featur, not feature", control("Branch")["value"] == "featur", control("Branch"))
    s.call("set_dialog", set=[{"control": "Branch", "value": ""}])
    picked = ws + "/picked"; os.makedirs(picked + "/proj-taken"); open(picked + "/proj-taken/x", "w").write("here first\n")
    s.call("set_dialog", press="Choose…"); s.call("set_dialog", set=[{"control": "Directory", "value": picked}], press="Choose")
    report("recheck: Choose... before a branch leaves the folder empty", control("Folder")["value"] == "", control("Folder"))
    s.call("set_dialog", set=[{"control": "Branch", "value": "late"}])
    report("recheck: the folder follows the branch in the folder picked", control("Folder")["value"] == picked + "/proj-late", control("Folder"))
    s.call("set_dialog", set=[{"control": "Branch", "value": "taken"}])
    report("recheck: a name taken in the folder picked: one not there", control("Folder")["value"] == picked + "/proj-taken-2", control("Folder"))
    s.call("set_dialog", press="Cancel")
    err, t = s.call("context_menu", on={"file": doc}); report("recheck: context_menu lists the File Browser's Git > New Worktree", not err and '"Git > New Worktree"' in t, t)
    # Claude's Git menu, as it is now: a repository with no commit in front.
    e = ws + "/empty"; os.makedirs(e); git(e, "init", "-q"); open(e + "/n.txt", "w").write("n\n"); s.call("open_document", path=e + "/n.txt")
    err, t = s.call("list_actions", search="New Worktree"); report("list_actions: New Worktree off before a first commit", '"enabled":false' in t, t)
    err, t = s.call("trigger_action", action="Git > New Worktree...", path=e + "/n.txt"); report("trigger_action: refused before a first commit", err and "cannot be used now" in t, t)
    if not err: s.call("set_dialog", press="Cancel")
    s.close()
    # And the other way: the workspace a repository with no commit, then a repository with commits in front.
    ws2 = ws + "-b"; os.makedirs(ws2); git(ws2, "init", "-q"); s = Server(ws2); s.call("open_document", path=doc)
    err, t = s.call("trigger_action", action="Git > New Worktree...", path=doc); report("trigger_action: allowed with a repository with commits in front", not err and "New Worktree" in t, t)
    if not err: s.call("set_dialog", press="Cancel")
    s.close()

if SECOND:
    ws = wk + "/ws4"; os.makedirs(ws); second_review(ws)
    print(f"{sum(results)} of {len(results)} as they should be", flush=True); sys.exit(0)
if MODE == "worktree":
    ws = wk + "/ws6"; os.makedirs(ws); worktree_check(ws)
    print(f"{sum(results)} of {len(results)} as they should be", flush=True); sys.exit(0)
if MODE == "third":
    ws = wk + "/ws5"; os.makedirs(ws); third_review(ws)
    print(f"{sum(results)} of {len(results)} as they should be", flush=True); sys.exit(0)

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
def marking(name):   # (a program of its own for each check, and its own mark: one that ran is not blamed on the next)
    mark, prog = wk + "/ran-" + name, wk + "/prog-" + name + ".sh"
    open(prog, "w").write('#!/bin/sh\necho "$0 $*" >> "%s"\ncat\n' % mark); os.chmod(prog, 0o755)
    return mark, prog
mark, prog = marking("a=b")
f = repo(ws + "/flt", {"a.flt": "one\n"})
open(f + "/.gitattributes", "w").write("*.flt filter=a=b\n"); git(f, "add", "-A"); git(f, "commit", "-q", "-m", "attr")
open(f + "/.git/config", "a").write('[filter "a=b"]\n\tclean = %s\n' % prog); open(f + "/a.flt", "w").write("TWO\n")
s.call("git_status", path=f); s.call("git_diff", path=f); report("a filter named a=b does not run (fault 11)", not os.path.exists(mark))
g = repo(ws + "/late", {"a.txt": "one\n"}); s.call("git_status", path=g)
mark, prog = marking("late")
open(g + "/.git/config", "a").write('[filter "late"]\n\tclean = %s\n' % prog); open(g + "/.gitattributes", "w").write("* filter=late\n"); open(g + "/a.txt", "w").write("TWO\n")
s.call("git_status", path=g); s.call("git_diff", path=g); report("a filter added just after a look does not run", not os.path.exists(mark))
s.close()
ws = wk + "/ws4"; os.makedirs(ws); second_review(ws)
ws = wk + "/ws5"; os.makedirs(ws); third_review(ws)
ws = wk + "/ws6"; os.makedirs(ws); worktree_check(ws)
print(f"{sum(results)} of {len(results)} as they should be", flush=True)
