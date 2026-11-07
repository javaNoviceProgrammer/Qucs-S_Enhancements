#!/usr/bin/env python3
"""A repository to measure git's integration on (the write-up's scale subject):

    make_big_repo.py <folder> [commits] [files] [branches] [tags]

git fast-import writes COMMITS commits over FILES files in nested folders,
some .sch among them, on main with a merge every 25 commits and BRANCHES side
branches off it, TAGS tags; then 200 files changed, 40 staged, 200 new files
and a large ignored folder. Seeded: the same repository each time. Made in
<folder>, which must not exist yet: nothing else is touched.
"""
import os, random, subprocess, sys

folder = sys.argv[1]
commits = int(sys.argv[2]) if len(sys.argv) > 2 else 50000
files = int(sys.argv[3]) if len(sys.argv) > 3 else 20000
branches = int(sys.argv[4]) if len(sys.argv) > 4 else 200
tags = int(sys.argv[5]) if len(sys.argv) > 5 else 100
rnd = random.Random(1)

if os.path.exists(folder):
    sys.exit(f"{folder} is there already")
os.makedirs(folder)
env = dict(os.environ, GIT_CONFIG_GLOBAL="/dev/null", GIT_CONFIG_NOSYSTEM="1")
def git(*args, **kw):
    return subprocess.run(["git", *args], cwd=folder, env=env, check=True, **kw)
git("init", "-q", "-b", "main")

def path(i):
    return f"d{i % 40}/s{i % 317}/f{i}." + ("sch" if i % 10 == 0 else "txt")
def blob(i, rev):
    return f"file {i} revision {rev}\n" + "".join(f"line {k} of {i}\n" for k in range(10 + i % 40))

fi = subprocess.Popen(["git", "fast-import", "--quiet", "--done"], cwd=folder, env=env, stdin=subprocess.PIPE, text=True)
w = fi.stdin.write
def commit(ref, mark, parents, changes, when):
    who = f"Author {mark % 37} <author{mark % 37}@example.com> {when} +0000"
    msg = f"Change {mark}: some subject line about this\n"
    w(f"commit {ref}\nmark :{mark}\nauthor {who}\ncommitter {who}\ndata {len(msg)}\n{msg}")
    if parents:
        w(f"from :{parents[0]}\n")
        for p in parents[1:]:
            w(f"merge :{p}\n")
    for i, rev in changes:
        data = blob(i, rev)
        w(f"M 100644 inline {path(i)}\ndata {len(data)}\n{data}")
    w("\n")

when = 1500000000
commit("refs/heads/main", 1, [], [(i, 0) for i in range(files)], when)
mark, tip = 2, 1
side_tips = []
for n in range(commits - 1):
    when += 600
    changes = [(rnd.randrange(files), n) for _ in range(1 + rnd.randrange(3))]
    if n % 25 == 24:
        # A side commit, and its merge.
        commit("refs/heads/main", mark, [tip], changes, when)
        side = mark
        mark += 1
        commit("refs/heads/main", mark, [tip, side], changes, when + 1)
        tip = mark
    else:
        commit("refs/heads/main", mark, [tip], changes, when)
        tip = mark
    mark += 1
    if n and n % max(1, commits // max(1, branches)) == 0 and len(side_tips) < branches:
        side_tips.append(tip)
for k, m in enumerate(side_tips):
    w(f"reset refs/heads/feature/{k}\nfrom :{m}\n\n")
for k in range(tags):
    w(f"reset refs/tags/v{k}\nfrom :{1 + (mark - 2) * k // max(1, tags)}\n\n")
w("done\n")
fi.stdin.close()
if fi.wait() != 0:
    sys.exit("git fast-import failed")
git("reset", "-q", "--hard", "main")

for i in rnd.sample(range(files), 200):
    with open(os.path.join(folder, path(i)), "a") as f:
        f.write("an edit\n")
staged = rnd.sample(range(files), 40)
for i in staged:
    with open(os.path.join(folder, path(i)), "a") as f:
        f.write("a staged edit\n")
git("add", "--", *[path(i) for i in staged])
for i in range(200):
    p = os.path.join(folder, f"new/n{i % 7}/new{i}.txt")
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w") as f:
        f.write("a new file\n" * (1 + i % 20))
with open(os.path.join(folder, ".gitignore"), "w") as f:
    f.write("build/\n*.dat\n")
for i in range(5000):
    p = os.path.join(folder, f"build/o{i % 50}/o{i}.dat")
    os.makedirs(os.path.dirname(p), exist_ok=True)
    open(p, "w").close()
print(subprocess.run(["git", "count-objects", "-v", "-H"], cwd=folder, env=env, capture_output=True, text=True).stdout)
print(f"{commits} commits, {files} files, {len(side_tips)} branches, {tags} tags in {folder}")
