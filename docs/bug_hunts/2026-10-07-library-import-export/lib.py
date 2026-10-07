"""Helpers of the probes: a divider subcircuit, a project of files, a bench that places a library part and reads its output."""
import os, json, mcp

# P1 -R1- P2, R2 from P2 to ground: v(P2) = v(P1) * r2 / (r1 + r2).
SUB = '''<Qucs Schematic 26.1.6>
<Components>
  <Port P1 1 220 100 -23 12 0 0 "1" 1 "analog" 0>
  <Port P2 1 280 100 4 12 1 2 "2" 1 "analog" 0>
  <R R1 1 250 100 -26 15 0 0 "{r1}" 1 "26.85" 0 "0.0" 0 "0.0" 0 "26.85" 0 "european" 0>
  <R R2 1 280 130 15 -26 0 1 "{r2}" 1 "26.85" 0 "0.0" 0 "0.0" 0 "26.85" 0 "european" 0>
  <GND * 1 280 160 0 0 0 0>
</Components>
<Wires>
</Wires>
<Diagrams>
</Diagrams>
<Paintings>
</Paintings>
'''


def divider(r1='1k', r2='1k'):
    return SUB.replace('{r1}', r1).replace('{r2}', r2)


def project(s, name, files):
    """A project NAME of FILES (relative path -> text), opened."""
    s.call('new_project', {'name': name})
    d = s.ws + f'/{name}_prj'
    for rel, text in files.items():
        p = os.path.join(d, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, 'w').write(text)
    s.call('open_project', {'name': name})
    return d


def bench(s, place, save_as, pins=2, extra=None):
    """A schematic: V1 (1 V) into the part's pin 1, its pin 2 labelled out; saved, simulated (op); v(out), or why not."""
    s.call('new_document', {})
    s.call('add_component', {'type': 'Vdc', 'name': 'V1', 'x': 100, 'y': 200, 'properties': {'U': '1 V'}})
    r = s.call('add_component', {'type': place['type'], 'name': 'X1', 'x': 320, 'y': 120, 'properties': place['properties']})
    if s.last_error:
        return {'placed': r}
    s.call('connect', {'from': 'V1.1', 'to': 'X1.1'})
    s.call('connect', {'from': 'V1.2', 'to': 'ground'})
    s.call('set_label', {'at': 'X1.2', 'name': 'out'})
    for p in range(3, pins + 1):
        s.call('connect', {'from': f'X1.{p}', 'to': 'ground'})
    if extra:
        extra(s)
    s.call('add_analysis', {'kind': 'op'})
    s.call('save_document', {'as': save_as})
    sim = s.call('simulate', {})
    out = {'ok': isinstance(sim, dict) and sim.get('succeeded')}
    if not out['ok']:
        out['sim'] = json.dumps(sim) if isinstance(sim, dict) else str(sim)
        return out
    d = s.call('get_dataset', {'variables': ['out']})
    try:
        out['v(out)'] = d['variables'][0]['final'] if 'variables' in d else d
    except Exception:
        out['v(out)'] = json.dumps(d)[:400]
    return out


def show(label, o):
    print(label, json.dumps(o)[:1500] if not isinstance(o, str) else o[:1500])
