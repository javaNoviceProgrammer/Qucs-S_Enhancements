# repro_close.py MODEL SECONDS: opens a model, solves it with that timeout (the solve goes on), then closes
# it; times the close (N3: under the sanitizers it waits for one LU factorization).
#   HUNT_RUNS=<scratch> python3 repro_close.py model.qfem 20
import sys, os, json
HERE = os.path.dirname(os.path.abspath(__file__))
# (fuzz_qfem.py of the hunt of 16 October: its server and sanitizer reading.)
QFEM = os.path.join(HERE, '..', '2026-10-16-fuzz', 'fuzz_qfem.py')
os.environ.setdefault('QUCS', os.path.join(HERE, '..', '..', '..', 'build-asan', 'qucs', 'qucs-s.app', 'Contents', 'MacOS', 'qucs-s'))
os.environ['HUNT_RUNS'] = os.path.join(os.environ['HUNT_RUNS'], 'repro')
MUT, T = sys.argv[1], float(sys.argv[2])
sys.argv = ['fuzz_qfem.py', '9998', '0']
src = open(QFEM).read().split("examples = sorted(")[0]
exec(compile(src, 'fuzz_qfem_head', 'exec'))
path = os.path.abspath(MUT)
s = Server(); pos = os.path.getsize(errpath)
for name_, args, limit in [('open_document', {'path': path}, 120), ('fem_solve', {'path': path, 'timeout': T}, 300),
                           ('close_document', {'path': path, 'unsaved': 'discard'}, 300)]:
    r, took = s.call(name_, args, limit)
    hits, pos = sanitizer(pos)
    status = r if isinstance(r, str) else ('error' if r.get('result', {}).get('isError') else 'ok')
    text = '' if isinstance(r, str) else json.dumps(r.get('result', {}))[:300]
    print(name_, status, round(took, 1), hits[:2], text, flush=True)
    if isinstance(r, str): break
s.kill()
