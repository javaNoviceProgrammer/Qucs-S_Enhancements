# mq.py: the reviewer's models through the MCP server (QUCS: the app; HOME, QUCS_CACHE_DIR set by the caller).
import sys, os, json
HERE = os.path.dirname(os.path.abspath(__file__))
_argv = sys.argv
sys.argv = ['fuzz_qfem.py', '9996', '0']
os.environ.setdefault('HUNT_RUNS', os.path.join(HERE, "runs"))
src = open('/Users/meisam/git/Qucs-S_Enhancements/docs/bug_hunts/2026-10-16-fuzz/fuzz_qfem.py').read().split("examples = sorted(")[0]
exec(compile(src, 'fuzz_qfem_head', 'exec'))
sys.argv = _argv
class Q:
    def __init__(self, model):
        self.path = os.path.join(HERE, model)
        self.s = Server()
        self.call('open_document', {})
    def call(self, name, args, limit=3600):
        r, took = self.s.call(name, dict(args, path=self.path), limit)
        if isinstance(r, str): return {'error': r}
        res = r.get('result', {})
        out = res.get('structuredContent') or {'text': (res.get('content') or [{}])[0].get('text', '')}
        if res.get('isError'): out = {'error': out.get('text', out)}
        return out
    def close(self): self.s.kill()
def lines(r, *keys):
    out = []
    if isinstance(r, dict):
        for l in r.get('log', []) + ['W: ' + w for w in r.get('warnings', [])]:
            if not keys or any(k.lower() in l.lower() for k in keys): out.append(l)
    return out
