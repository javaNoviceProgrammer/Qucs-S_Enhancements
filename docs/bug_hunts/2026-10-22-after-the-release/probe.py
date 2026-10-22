"""Run a model (JSON file) through qucs-s --mcp-server: open, then each call given as JSON on argv."""
import sys, json, time
S = '/tmp/qucs-hunt'
sys.path.insert(0, S)
from mcp_client import Client
H = S + '/hunt2'

def run(model, calls, tag='p', width=1500, env=None):
    path = H + '/' + tag + '.qfem'
    json.dump(model, open(path, 'w'), indent=1)
    c = Client(H + '/' + tag + '.w', env_extra=env or {})
    out = []
    try:
        r = c.call('fem_model', {'action': 'open', 'path': path})
        if r[0]: print('open error', r[1][:width])
        for name, args in calls:
            a = dict(args); a.setdefault('path', path)
            t = time.time()
            err, text = c.call(name, a)
            out.append((err, text))
            print('==', name, json.dumps({k: v for k, v in args.items()})[:150], 'error' if err else 'ok', '%.1f s' % (time.time() - t))
            print(text[:width])
    finally:
        c.close()
    return out
