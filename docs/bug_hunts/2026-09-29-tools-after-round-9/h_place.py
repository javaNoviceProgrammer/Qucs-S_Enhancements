from mcp import Server, HERE, REPO as mcp_repo
import json, re, glob, os, time
LIB = os.path.join(mcp_repo, 'qucs-s-26.1.1/library')
s = Server(HERE + '/wsh26'); s.call('new_document', {})
bad = []; n = 0; t0 = time.time()
for f in sorted(glob.glob(LIB + '/*.lib')):
    lib = os.path.basename(f)[:-4]
    for part in re.findall(r'<Component\s+([^>]+)>', open(f, errors='replace').read()):
        d = s.call('describe_part', {'library': lib, 'part': part}, ok=False)
        if not isinstance(d, dict) or 'place' not in d: continue
        args = dict(d['place']); args.update({'x': 300, 'y': 300})
        r = s.call('add_component', args, ok=False); n += 1
        if isinstance(r, str): bad.append((lib, part, 'add failed: ' + r[:100])); continue
        if len(r.get('pins', [])) != len(d.get('pins', [])):
            bad.append((lib, part, 'pins: describe %d, placed %d' % (len(d.get('pins', [])), len(r.get('pins', [])))))
        names_d = [p.get('name') for p in d.get('pins', [])]; names_a = [p.get('name') for p in r.get('pins', [])]
        if names_d != names_a: bad.append((lib, part, 'names differ %s vs %s' % (names_d[:4], names_a[:4])))
        s.call('delete', {'names': [r['name']]}, ok=False)
        if n % 300 == 0: s.call('close_document', {'unsaved': 'discard'}, ok=False); s.call('new_document', {})
s.close()
print(n, 'placed', len(bad), 'mismatches', round(time.time() - t0), 's')
import collections
print(collections.Counter(b[2].split(':')[0] for b in bad).most_common(5))
for b in bad[:15]: print(b)
