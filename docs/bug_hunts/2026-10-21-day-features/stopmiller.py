import sys, os, json, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mq import Q
HERE = os.path.dirname(os.path.abspath(__file__))
path = os.path.join(HERE, 'stop_miller.qfem')
open(path, 'w').write(open(sys.argv[1]).read())
study = sys.argv[2]
q = Q(path)
t = time.time(); r = q.call('fem_solve', {'study': study, 'timeout': 3}, 60); print(f'fem_solve returned after {time.time()-t:.1f} s: {str(r)[:120]}', flush=True)
t = time.time(); st = q.call('fem_stop', {'timeout': 300}, 400); print(f'fem_stop waited {time.time()-t:.1f} s: {str(st)[:200]}', flush=True)
t = time.time(); r = q.call('fem_solve', {'study': study, 'timeout': 3}, 60)
t = time.time(); c = q.call('close_document', {'unsaved': 'discard'}, 400); print(f'close mid-solve waited {time.time()-t:.1f} s', flush=True)
q.close()
