from mcp import Server, HERE, REPO as mcp_repo
import json, re, glob, os, time, collections
LIB = os.path.join(mcp_repo, 'qucs-s-26.1.1/library')
s = Server(HERE + '/wsh6')
out = []
t0 = time.time()
for f in sorted(glob.glob(LIB + '/*.lib')):
    lib = os.path.basename(f)[:-4]
    names = re.findall(r'<Component\s+([^>]+)>', open(f, errors='replace').read())
    for part in names:
        try:
            m = s.rpc('tools/call', {'name': 'describe_part', 'arguments': {'library': lib, 'part': part}})
        except Exception as e:
            out.append((lib, part, 'DIED ' + repr(e))); s = Server(HERE + '/wsh6'); continue
        r = m.get('result', m)
        t = '\n'.join(c.get('text', '') for c in r.get('content', []))
        if r.get('isError'): out.append((lib, part, 'ERR ' + t[:150])); continue
        d = json.loads(t)
        pins = d.get('pins', [])
        roles = collections.Counter(p.get('role') for p in pins)
        issue = []
        if not pins: issue.append('no pins')
        if roles.get('output', 0) > 1: issue.append('outputs %d' % roles['output'])
        if 'place' not in d: issue.append('no place')
        names_ = [p.get('name') for p in pins if p.get('name')]
        if len(set(n.lower() for n in names_)) != len(names_): issue.append('dup pin names %s' % names_)
        if issue: out.append((lib, part, '; '.join(issue)))
    if time.time() - t0 > 240: out.append(('...', 'stopped at', lib)); break
s.close()
print(len(out), 'issues', round(time.time() - t0), 's')
c = collections.Counter(o[2].split(' ')[0] + ' ' + (o[2].split(' ')[1] if len(o[2].split(' ')) > 1 else '') for o in out)
print(c.most_common(10))
for o in out[:40]: print(o)
