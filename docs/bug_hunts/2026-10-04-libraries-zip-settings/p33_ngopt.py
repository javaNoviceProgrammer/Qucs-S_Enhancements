"""p33: NgOpt's new options through edit_component - CMA-ES, Starts, Polish, a Constraint - in the netlist; bad values."""
import mcp, json, re
s = mcp.Server('p33')
s.call('import_netlist', {'text': 't\nV1 in 0 DC 1\nR1 in out 1k\nR2 out 0 1k\n.op\n.end', 'save_as': s.ws + '/o.sch'})
s.call('add_component', {'path': 'o.sch', 'type': '.NGOPT', 'name': 'NgOpt1', 'x': 400, 'y': 400})
def line():
    n = s.call('get_netlist', {'path': 'o.sch'})
    t = n.get('netlist', str(n)) if isinstance(n, dict) else str(n)
    m = [l for l in t.split('\n') if 'optimize' in l.lower()]
    return m[:2] if m else t[-300:]
base = {'Analysis': 'OP1', 'Method': 'cmaes', 'Minimize': 'abs(v(out)-0.3)', 'Knob': ['param|R2|1k|100|10k'], 'Starts': '3', 'Polish': 'yes', 'Constraint': ['OP1|v(out)|0.25|']}
r = s.call('edit_component', {'path': 'o.sch', 'name': 'NgOpt1', 'properties': base})
print('set:', s.last_error, str(r)[:200]); print('  netlist:', line())
for bad in ({'Method': 'nosuch'}, {'Starts': '0'}, {'Starts': '-2'}, {'Starts': '2.5'}, {'Constraint': ['OP1|v(out)||']}, {'Constraint': ['OP1|v(out)|5|1']},
            {'Constraint': ['OP1||0|1']}, {'Polish': 'maybe'}, {'Method': 'nm', 'Polish': 'yes'}, {'Constraint': ['OP1|v(out)|1k|']}):
    r = s.call('edit_component', {'path': 'o.sch', 'name': 'NgOpt1', 'properties': bad})
    print(json.dumps(bad)[:60].ljust(60), 'edit err', s.last_error, '|', str(r)[:90].replace('\n', ' '))
    print('   netlist:', str(line())[:220])
    s.call('edit_component', {'path': 'o.sch', 'name': 'NgOpt1', 'properties': base})
s.close()
s2 = mcp.Server('p33c')
s2.call('import_netlist', {'text': 't\nV1 in 0 DC 1\nR1 in out 1k\nR2 out 0 1k\n.op\n.end', 'save_as': s2.ws + '/o.sch'})
s2.call('add_component', {'path': 'o.sch', 'type': '.NGOPT', 'name': 'NgOpt1', 'x': 400, 'y': 400})
for bad in ({}, {'Method': 'nosuch'}, {'Starts': '0'}, {'Starts': '2.5'}, {'Polish': 'maybe'}, {'Seed': '-1.5'}, {'Size': 'abc'}):
    props = dict(base); props.update(bad)
    s2.call('edit_component', {'path': 'o.sch', 'name': 'NgOpt1', 'properties': props})
    c = s2.call('check_schematic', {'path': 'o.sch'})
    msgs = [i['message'] for i in c.get('errors', []) + c.get('warnings', []) if 'NgOpt' in i['message'] or 'optim' in i['message'].lower()] if isinstance(c, dict) else c
    n = s2.call('get_netlist', {'path': 'o.sch'}); t = n.get('netlist', str(n)) if isinstance(n, dict) else str(n)
    opt = [l for l in t.split('\n') if l.lower().startswith('optimize') or 'Error:' in l]
    print('CHECK', json.dumps(bad).ljust(24), '| check says:', msgs[:2], '| netlist:', [o[:150] for o in opt][:2])
s2.close()
