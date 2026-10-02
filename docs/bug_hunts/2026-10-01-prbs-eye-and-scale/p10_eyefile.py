import sys, json, shutil, os; sys.path.insert(0, sys.argv[1])
from probe import Probe, sch
p = Probe(sys.argv[1] + '/p10')
shutil.copy(sys.argv[1] + '/p4/ws/p/e.dat.ngspice', sys.argv[1] + '/p10/ws/p/base.dat.ngspice')
head = '<Eye 100 720 420 260 3 #c0c0c0 1 00 1 0 1 1 1 0 1 1 1 -1 0.5 1 315 0 225 1 0 0 0 -1 '
tail = ' "" "" "" "eye">\n\t<"ngspice/tran.v(out)" #0000ff 1 3 0 0 0>\n  </Eye>\n'
cases = {
 'ok': '- 2 0 2 - 0 1 0.3 0.2', 'uineg': '-1 2 0 2 - 0 1 0.3 0.2', 'uinan': 'nan 2 0 2 - 0 1 0.3 0.2', 'uiinf': '1e400 2 0 2 - 0 1 0.3 0.2',
 'uiword': 'abc 2 0 2 - 0 1 0.3 0.2', 'span0': '- 0 0 2 - 0 1 0.3 0.2', 'span100': '- 100 0 2 - 0 1 0.3 0.2', 'spanneg': '- -3 0 2 - 0 1 0.3 0.2',
 'fromnan': '- 2 nan 2 - 0 1 0.3 0.2', 'fromneg': '- 2 -1 2 - 0 1 0.3 0.2', 'levels3': '- 2 0 3 - 0 1 0.3 0.2', 'levels0': '- 2 0 0 - 0 1 0.3 0.2',
 'thrnan': '- 2 0 2 nan 0 1 0.3 0.2', 'drawn7': '- 2 0 2 - 7 1 0.3 0.2', 'mask5': '- 2 0 2 - 0 1 5 0.2', 'maskneg': '- 2 0 2 - 0 1 -1 -1',
 'masknan': '- 2 0 2 - 0 1 nan nan', 'short': '- 2', 'none': '', 'extra': '- 2 0 2 - 0 1 0.3 0.2 9 9 9 9',
}
for name, fields in cases.items():
    text = sch('  <GND * 1 0 0 0 0 0 0>\n').replace('<Diagrams>\n', '<Diagrams>\n  ' + head + fields + tail)
    shutil.copy(sys.argv[1] + '/p10/ws/p/base.dat.ngspice', sys.argv[1] + '/p10/ws/p/%s.dat.ngspice' % name)
    e, o = p.open(name + '.sch', text)
    if e: print('%-8s OPEN %s' % (name, o[-60:])); continue
    e2, shot = p.call('screenshot', {})
    e3, js = p.call('get_schematic', {'format': 'summary'})
    eye = ''
    try:
        d = json.loads(js).get('diagrams', [])
        eye = json.dumps(d[0].get('eye', d[0]))[:220] if d else 'no diagram'
    except Exception: eye = js[:150]
    print('%-8s shot=%s %s' % (name, 'ok' if not e2 else shot[:60], eye))
    p.call('close_document', {'unsaved': 'discard'})
p.close()
