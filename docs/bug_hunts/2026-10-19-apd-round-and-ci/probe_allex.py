import sys, time, re, glob, shutil, os, json
sys.path.insert(0, sys.argv[1])
from mcp_client import Client
H = sys.argv[2]
out = open(H + '/allex/results.txt', 'w')
for f in sorted(glob.glob('/Users/meisam/git/Qucs-S_Enhancements/qucs-s-26.1.1/examples/multiphysics/*.qfem')):
    name = os.path.basename(f)
    dst = H + '/allex/' + name
    shutil.copy(f, dst)
    d = json.load(open(dst))
    for comp in d.get('components', []):
        for ex in comp.get('exports', []) if isinstance(comp, dict) else []:
            if isinstance(ex, dict): ex.pop('file', None)
    json.dump(d, open(dst, 'w'), indent=1)
    c = Client(H + '/allex/w')
    c.call('open_document', {'path': dst})
    t = time.time(); err, text = c.call('fem_solve', {'path': dst, 'timeout': 30}); a = time.time() - t
    e2, desc = c.call('fem_describe', {'path': dst})
    stopped = re.findall(r'"stopped":"([^"]*)"', desc)
    warn = re.findall(r'"warnings":\[([^\]]*)\]', text)
    infs = len(re.findall(r'"inf|: inf|"nan|: nan', text))
    line = f'{name}: {round(a,1)} s {"ERR " + text[:200] if err else "ok"} | stopped {stopped} | warnings {warn[:1]} | inf/nan {infs}'
    print(line, flush=True); out.write(line + '\n'); out.flush()
    if err and text.startswith('Still solving'): c.call('fem_stop', {'path': dst})
    c.close()
