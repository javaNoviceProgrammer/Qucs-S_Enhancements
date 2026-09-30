import json, os, sys, glob, time
from mcp import Server, HERE
opts = json.loads(sys.argv[1]); out = sys.argv[2]
files = sorted(glob.glob(HERE + '/exs/**/*.sch', recursive=True))
rows = {}
s = Server(HERE + '/wsh_arr')
t0 = time.time()
for f in files:
    rel = f[len(HERE + '/exs/'):]
    name = os.path.basename(f)
    try:
        s.call('open_document', {'path': f})
        t1 = time.time()
        a = s.call('arrange', dict(path=name, **opts), ok=False)
        dt = time.time() - t1
        if isinstance(a, dict) and 'arranged' in a:
            rows[rel] = {'ok': 'every net as it was' in a['arranged'], 's': round(dt, 2)}
        else:
            rows[rel] = {'ok': None, 'why': str(a)[:160], 's': round(dt, 2)}
        s.call('close_document', {'path': name, 'unsaved': 'discard'}, ok=False)
    except Exception as e:
        rows[rel] = {'ok': False, 'error': repr(e)[:160]}
        try: s.close()
        except Exception: pass
        s = Server(HERE + '/wsh_arr')
json.dump(rows, open(out, 'w'), indent=1)
s.close()
bad = {k: v for k, v in rows.items() if v.get('ok') is False}
refused = {k: v for k, v in rows.items() if v.get('ok') is None}
print(len(rows), 'files', round(time.time() - t0), 's; nets changed/crash:', len(bad), '; refused:', len(refused), '; slowest', max(v.get('s', 0) for v in rows.values()))
for k, v in list(bad.items())[:10]: print('BAD', k, v)
import collections
print(collections.Counter(v['why'][:60] for v in refused.values()).most_common(6))
