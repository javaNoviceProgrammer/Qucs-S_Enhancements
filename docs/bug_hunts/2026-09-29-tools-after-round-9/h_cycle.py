from mcp import Server, HERE
import json, shutil, os, time
WS = HERE + '/wsh30'; shutil.rmtree(WS, ignore_errors=True); os.makedirs(WS)
s = Server(WS)
A = lambda **a: {'tool': 'add_component', 'arguments': a}
C = lambda a, b: {'tool': 'connect', 'arguments': {'from': a, 'to': b}}
def block(me, sub=None):
    calls = [A(type='Port', name='p', x=100, y=100), A(type='R', name='R1', x=250, y=100), C('p.1', 'R1.1'), C('R1.2', 'ground')]
    if sub: calls += [A(type='Sub', name='X1', x=400, y=300, properties={'File': sub}), C('X1.1', 'R1.1')]
    s.call('new_document', {})
    r = s.call('batch', {'atomic': True, 'calls': calls + [{'tool': 'save_document', 'arguments': {'as': WS + '/%s.sch' % me, 'replace': True}}]}, ok=False)
    print(me, 'saved' if not (isinstance(r, str) and 'failed' in r.split('\n')[0]) else r[:200])
    s.call('close_document', {'unsaved': 'discard'}, ok=False)
block('a'); block('b', 'a.sch')
s.call('open_document', {'path': WS + '/a.sch'})
r = s.call('batch', {'atomic': True, 'calls': [A(type='Sub', name='X1', x=400, y=300, properties={'File': 'b.sch'}), C('X1.1', 'R1.1'),
                                              {'tool': 'save_document', 'arguments': {'path': 'a.sch'}}]}, ok=False)
print('a now uses b:', str(r)[:80].replace('\n', ' '))
s.call('new_document', {})
s.call('batch', {'atomic': True, 'calls': [A(type='Sub', name='XT', x=300, y=200, properties={'File': 'a.sch'}), A(type='Vdc', name='V1', x=100, y=200),
  C('V1.1', 'XT.1'), C('V1.2', 'ground'), {'tool': 'add_analysis', 'arguments': {'kind': 'op'}},
  {'tool': 'save_document', 'arguments': {'as': WS + '/top.sch'}}]}, ok=False)
t0 = time.time(); c = s.call('check_schematic', {'path': 'top.sch', 'subcircuits': True}, ok=False); print('check', round(time.time() - t0, 2), (c.get('found') if isinstance(c, dict) else c)[:200])
t0 = time.time()
try:
    n = s.call('get_netlist', {'path': 'top.sch'}, ok=False); print('netlist', round(time.time() - t0, 2), len(n), n[:700].replace('\n', ' | '))
except Exception as e: print('netlist died', repr(e)[:100], round(time.time() - t0, 2))
try: s.close()
except Exception: pass
