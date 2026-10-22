# A quantum study stopped and closed mid-solve: how long each waits (Release).
import sys, os, json, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mq import Q
HERE = os.path.dirname(os.path.abspath(__file__))
for ex in ['strained_laser_gain.qfem', 'ingaas_mqw_qcse.qfem', 'gaas_eam_excitons.qfem']:
    path = os.path.join(HERE, 'stop_' + ex)
    open(path, 'w').write(open('/Users/meisam/git/Qucs-S_Enhancements/qucs-s-26.1.1/examples/multiphysics/' + ex).read())
    q = Q(path)
    t = time.time(); r = q.call('fem_solve', {'timeout': 600}, 900); full = time.time() - t
    q.close()
    q = Q(path)
    t = time.time(); r = q.call('fem_solve', {'timeout': 1}, 60); first = time.time() - t
    t = time.time(); st = q.call('fem_stop', {'timeout': 120}, 200); stop = time.time() - t
    q.call('fem_solve', {'timeout': 1}, 60)
    t = time.time(); c = q.call('close_document', {'unsaved': 'discard'}, 200); close = time.time() - t
    print(f'{ex}: whole solve {full:.1f} s; fem_stop after 1 s waited {stop:.1f} s ({str(st)[:90]}); close mid-solve {close:.1f} s', flush=True)
    q.close()
