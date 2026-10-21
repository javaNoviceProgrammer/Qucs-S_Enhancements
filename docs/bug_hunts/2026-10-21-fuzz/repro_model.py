# repro_model.py MODEL: opens a model, builds and solves it through a fresh server (QUCS, the ASan build's
# by default), as the fuzzers do; says what each call did, and any sanitizer report.
#   HUNT_RUNS=<scratch> python3 repro_model.py model.qfem
import sys, os, json
HERE = os.path.dirname(os.path.abspath(__file__))
# (fuzz_qfem.py of the hunt of 16 October: its server and sanitizer reading.)
QFEM = os.path.join(HERE, '..', '2026-10-16-fuzz', 'fuzz_qfem.py')
os.environ.setdefault('QUCS', os.path.join(HERE, '..', '..', '..', 'build-asan', 'qucs', 'qucs-s.app', 'Contents', 'MacOS', 'qucs-s'))
os.environ['HUNT_RUNS'] = os.path.join(os.environ['HUNT_RUNS'], 'repro')
MUT = sys.argv[1]
sys.argv = ['fuzz_qfem.py', '9999', '0']
src = open(QFEM).read().split("examples = sorted(")[0]
exec(compile(src, 'fuzz_qfem_head', 'exec'))


path = os.path.abspath(MUT)
s = Server(); pos = os.path.getsize(errpath)
for name_, args, limit in [('open_document', {'path': path}, 120), ('fem_build', {'path': path, 'timeout': 60}, 200), ('fem_solve', {'path': path, 'timeout': 120}, 300)]:
    r, took = s.call(name_, args, limit)
    hits, pos = sanitizer(pos)
    status = r if isinstance(r, str) else ('error' if r.get('result', {}).get('isError') else 'ok')
    text = '' if isinstance(r, str) else json.dumps(r.get('result', {}))[:200]
    print(name_, status, round(took, 1), hits[:2], text)
    if isinstance(r, str): break
s.kill()
