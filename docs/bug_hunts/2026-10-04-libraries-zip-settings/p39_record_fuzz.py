"""Damaged .qucs-library.json records through open_project's sync (ASan): no crash, no report, nothing outside
Libraries/ touched, the team library unchanged."""
import os, json, random, hashlib, libsetup
os.environ['QUCS'] = '<repo>/build-asan/qucs/qucs-s.app/Contents/MacOS/qucs-s'
import mcp; mcp.APP = os.environ['QUCS']
s, team, proj, made, saved = libsetup.setup('p39')
s.call('simulate', {'path': 'top.sch', 'brief': True})       # the link made
rec = proj + '/Libraries/VaRes/.qucs-library.json'
good = open(rec).read() if os.path.exists(rec) else None
print('record there:', good is not None)
open(proj + '/keep.txt', 'w').write('keep')
def digest(d):
    h = hashlib.sha1()
    for root, _, files in sorted(os.walk(d)):
        for f in sorted(files):
            p = os.path.join(root, f); h.update(p.encode()); h.update(open(p, 'rb').read() if not os.path.islink(p) else os.readlink(p).encode())
    return h.hexdigest()
team0 = digest(team)
rng = random.Random(39)
g = json.loads(good)
muts = ['', '{', 'null', '[]', '"x"', '{"files": 5}', '{"files": [5, null, "x"]}', '{"files": [{}]}',
        json.dumps(dict(g, files=[{'kind': 'link', 'path': None, 'original': 7}])), json.dumps(dict(g, files=[{'kind': 'zzz', 'path': 'keep.txt'}])),
        json.dumps(dict(g, files=[{'kind': 'copy', 'path': 'vres.va'}] * 5000)), json.dumps(dict(g, library=None, folder=12)),
        json.dumps(dict(g, files=[{'kind': 'link', 'path': 'a' * 5000, 'original': '/' + 'b' * 5000}])),
        json.dumps(dict(g, files=[{'kind': 'copy', 'path': 'sub/../vres.va'}])), json.dumps(dict(g, files=[{'kind': 'copy', 'path': '.'}])),
        json.dumps(dict(g, files=[{'kind': 'copy', 'path': ''}])), json.dumps(dict(g, files=[{'kind': 'copy', 'path': '.qucs-library.json'}])),
        '\x00\xff\xfe' * 100, good[:len(good) // 2]]
for _ in range(15):
    b = bytearray(good.encode()); 
    for _ in range(rng.randint(1, 8)): b[rng.randrange(len(b))] = rng.randrange(256)
    muts.append(b.decode('latin-1'))
dead = None
for i, m in enumerate(muts):
    os.makedirs(proj + '/Libraries/VaRes', exist_ok=True)
    open(rec, 'w', encoding='latin-1').write(m)
    try:
        s.call('close_project', {}); s.call('open_project', {'name': 'use'})
        s.call('check_schematic', {'path': 'top.sch'})
    except Exception as e:
        dead = (i, m[:80], str(e)[-400:]); break
    if not os.path.exists(proj + '/keep.txt') or not os.path.exists(proj + '/top.sch'):
        print('LOST a project file at mutant', i, repr(m[:80])); open(proj + '/keep.txt', 'w').write('keep')
print('mutants:', len(muts), '| dead:', dead)
print('team library unchanged:', digest(team) == team0)
err = open(s.root + '/server.err').read()
print('sanitizer reports:', err.count('ERROR: AddressSanitizer') + err.count('runtime error:'))
s.close()
