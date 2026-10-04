"""p8: one schematic (RC, .ac swept over C1, .tran) simulated text and binary: the tools' answers the same?"""
import json, os, mcp
def run(binary):
    s = mcp.Server('p8' + ('bin' if binary else 'txt'))
    if binary:
        s.call('set_settings', {'scope': 'simulators', 'values': {'Results/Keep large results binary': True, 'Results/Binary above': 0}})
    s.call('import_netlist', {'text': 'rc\nV1 in 0 DC 1 AC 1 PULSE(0 1 0 1n 1n 5u 10u)\nR1 in out 1k\nC1 out 0 1n\n.ac dec 10 1k 10meg\n.tran 10n 20u\n.end',
                              'save_as': s.ws + '/rc.sch'})
    sw = s.call('add_analysis', {'path': 'rc.sch', 'kind': 'sweep', 'analysis': 'AC1', 'parameter': 'C1', 'from': '1n', 'to': '4n', 'points': 3})
    sim = s.call('simulate', {'path': 'rc.sch'})
    out = {'sim': (sim.get('succeeded'), sim.get('errors')), 'sweep': str(sw)[:100]}
    files = sorted(f for f in os.listdir(s.ws) if f.startswith('rc.dat'))
    out['files'] = files
    out['binary head'] = [open(s.ws + '/' + f, 'rb').read(40) for f in files]
    ds = s.call('get_dataset', {'path': 'rc.sch'})
    out['list'] = json.dumps(ds, sort_keys=True)[:3000]
    names = [v['name'] for v in ds.get('variables', [])] if isinstance(ds, dict) else []
    out['names'] = names
    for v in names:
        q = s.call('get_dataset', {'path': 'rc.sch', 'variables': [v], 'points': 5})
        out['v ' + v] = json.dumps(q, sort_keys=True)
    q = s.call('get_dataset', {'path': 'rc.sch', 'variables': [n for n in names if 'v(out)' in n][:2], 'measure': ['bandwidth', 'max', 'min', 'rise_time']})
    out['measure'] = json.dumps(q, sort_keys=True)
    e = s.call('export_data', {'path': 'rc.sch', 'format': 'csv', 'save_as': s.root + '/out.csv'})
    out['export'] = (s.last_error, str(e)[:200], open(s.root + '/out.csv').read() if os.path.isfile(s.root + '/out.csv') else None)
    s.close()
    return out
t, b = run(False), run(True)
print('text files', t['files'], 'binary files', b['files'], b['binary head'])
print('sim', t['sim'], b['sim'])
for k in sorted(set(t) | set(b)):
    if k in ('files', 'binary head', 'sim'): continue
    if t.get(k) != b.get(k):
        print('DIFFERS:', k); print('  text  :', str(t.get(k))[:700]); print('  binary:', str(b.get(k))[:700])
print('compared', len(set(t) | set(b)), 'answers')
