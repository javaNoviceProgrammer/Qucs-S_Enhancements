# C2: an atomic batch put back under the user's dialog. The batch's second
# call opens the wire label's dialog (a double click on it); the third is
# refused (a dialog waits); the batch fails and puts the schematic back as it
# was, its labels made anew - the dialog's label freed under it; OK reads it.
#   QUCS=<qucs-s> python3 repro_c2.py SCRATCH
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp import Server
s = Server(os.environ['QUCS'], sys.argv[1], sys.argv[1] + '.stderr')
def c(n, a):
    r = s.call(n, a, 60); print(n, '->', s.text(r)[:160].replace('\n', ' '), flush=True); return r
c('new_document', {})
c('save_document', {'as': s.ws + '/a.sch'})
c('add_wire', {'points': [[100, 100], [300, 100]]})
c('set_label', {'at': [200, 100], 'name': 'lab'})
c('batch', {'atomic': True, 'calls': [{'tool': 'add_component', 'arguments': {'type': 'R', 'x': 100, 'y': 300}},
                                      {'tool': 'send_input', 'arguments': {'click': [200, 100], 'double': True}},
                                      {'tool': 'add_component', 'arguments': {'type': 'C', 'x': 300, 'y': 300}}]})
c('set_dialog', {'set': [{'control': 'c1', 'value': 'other'}], 'press': 'Ok'})
r = c('get_state', {})
print('CRASHED' if r == 'EOF' else 'survived')
s.kill()
