import sys, json, time, os
sys.path.insert(0, '/private/tmp/claude-501/-Users-meisam-git-Qucs-S-Enhancements/ff1f864c-ed6a-4c31-a788-5998e059b547/scratchpad/repro')
from client import Server
root, app, listing, out = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
s = Server(root, '/Users/meisam/git/Qucs-S_Enhancements', app)
res = {}
for rel in open(listing).read().split('\n'):
    if not rel: continue
    path = root + '/ws/ex/' + rel[len('ngspice/'):]
    e, t = s.call('open_document', {'path': path})
    if e: res[rel] = {'open': t[0][:120]}; continue
    t0 = time.time()
    e, t = s.call('simulate', {'timeout': 60})
    try:
        j = json.loads(t[0])
        res[rel] = {'ok': j.get('succeeded'), 'n': j.get('variable count'), 'err': [x.get('message', '')[:140] for x in j.get('errors', [])][:3],
                    's': round(time.time() - t0, 1), 'written': j.get('dataset written')}
    except Exception:
        res[rel] = {'text': t[0][:200]}
    s.call('close_document', {'unsaved': 'discard'})
s.close()
json.dump(res, open(out, 'w'), indent=1)
bad = {k: v for k, v in res.items() if not v.get('ok')}
print(len(res), 'run;', len(bad), 'not succeeded')
for k, v in bad.items(): print(' ', k, json.dumps(v)[:300])
