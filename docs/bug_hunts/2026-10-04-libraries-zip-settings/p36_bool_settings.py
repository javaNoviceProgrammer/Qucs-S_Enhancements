"""set_settings: a yes/no key given what is not true/false; the console's options (one of three)."""
import sys, json; sys.path.insert(0, __file__.rsplit('/', 1)[0]); from mcp import Server
s = Server('p36')
def table(scope): return {e['key']: e for e in s.call('get_settings', {'scope': scope})['settings']}
def show(r):
    if not isinstance(r, dict): return str(r)[:150]
    return 'err=%s changed=%s %s' % (s.last_error, [(c['key'][-25:], c.get('now')) for c in r.get('changed', [])], json.dumps({k: v for k, v in r.items() if k not in ('changed', 'note', 'scope')})[:200])
B = 'Netlist/Include ngspice_mathfunc.inc (limexp, step, stp)'
for v in ['yes', 'false', 1, 0, 2, None, [], {}, 'true', 1.0]:
    r = s.call('set_settings', {'scope': 'simulators', 'values': {B: v}})
    print('bool %-7r -> %s | now %r' % (v, show(r), table('simulators')[B]['value']))
s.call('set_settings', {'scope': 'simulators', 'values': {B: True}})
O = [k for k in table('simulators') if k.startswith('Simulation console/')]
print('options:', [(k[19:40], table('simulators')[k]['value']) for k in O])
for vals in [{O[1]: True, O[2]: True}, {O[0]: False}, {O[0]: False, O[1]: False, O[2]: False}, {O[1]: True}, {O[1]: False}]:
    r = s.call('set_settings', {'scope': 'simulators', 'values': vals})
    print('opt', [(k[19:30], v) for k, v in vals.items()], '->', show(r), '| now', [table('simulators')[k]['value'] for k in O])
s.close()
