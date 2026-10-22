import sys, json; sys.path.insert(0, '/tmp/qucs-hunt/hunt2')
from probe import run
def well(params, steps, physics_extra=None):
    ph = {"type": "quantum", "tag": "qw", "h1d": "0.25[nm]"}
    ph.update(physics_extra or {})
    return {"format": "qucs-s multiphysics 1",
     "parameters": params,
     "components": [{"unit": "nm",
        "geometry": [{"type": "rectangle", "name": "below", "size": ["10", "8"]},
                     {"type": "rectangle", "name": "well", "position": ["0", "8"], "size": ["10", "9.4"]},
                     {"type": "rectangle", "name": "above", "position": ["0", "17.4"], "size": ["10", "8"]}],
        "materials": [{"material": "barrier", "compound": "AlGaAs", "x": "0.3", "selection": {"only": ["below", "above"]}},
                      {"material": "well", "compound": "GaAs", "x": "0", "selection": {"only": ["well"]}}],
        "physics": [ph], "mesh": {"size": "normal", "custom": True, "hmax": "2"}}],
     "studies": [{"type": "study", "tag": "s", "steps": steps}]}
which = sys.argv[1] if len(sys.argv) > 1 and __name__ == "__main__" else ""
spec = {"type": "spectrum", "energies": "linspace(1.38[eV], 1.95[eV], 58)", "states": 8, "linewidth": "5.5[meV]", "lineshape": "gaussian", "continuum": "bound"}
if which == 'density':
    # (P1) a sweep of the carriers, no field: the sum-rule warning?
    m = well([{"name": "N", "expression": "0[1/cm^3]"}], [{"type": "sweep", "parameters": [["N", "0, 3e18", "1/cm^3"]]}, dict(spec, density="N")])
    r = run(m, [('fem_solve', {'study': 's'})], tag='f2a', width=300)
    j = json.loads(r[0][1]); print('WARNINGS:', [w[:250] for w in j.get('warnings', [])])
if which == 'nosweep':
    m = well([], [dict(spec, dnwindow=["1.38[eV]", "1.50[eV]"], conservearea=True)])
    r = run(m, [('fem_solve', {'study': 's'}), ('fem_evaluate', {'study': 's', 'expression': 'qw.dnTMloc'})], tag='f2b', width=300)
    j = json.loads(r[0][1]); print('WARNINGS:', [w[:250] for w in j.get('warnings', [])], [l[:200] for l in j['log'] if 'dnwindow' in l or 'local' in l])
if which == 'nm':
    m = well([{"name": "F", "expression": "0[V/m]"}], [{"type": "sweep", "parameters": [["F", "0, 1e7", "V/m"]]}, dict(spec, dnwindow=["850[nm]", "880[nm]"])], {"F": "F"})
    r = run(m, [('fem_solve', {'study': 's'}), ('fem_evaluate', {'study': 's', 'expression': 'qw.dnTMloc', 'point': 2})], tag='f2c', width=300)
    j = json.loads(r[0][1]); print('WARNINGS:', [w[:250] for w in j.get('warnings', [])])
if which == 'moving':
    # energies of the swept parameter: Δα compared at other energies
    m = well([{"name": "F", "expression": "0[V/m]"}, {"name": "E0", "expression": "1.38[eV]"}],
             [{"type": "sweep", "combination": "specified", "parameters": [["F", "0, 1e7", "V/m"], ["E0", "1.38, 1.48", "eV"]]},
              dict(spec, energies="linspace(E0, E0 + 0.57[eV], 58)", dnwindow=["1.38[eV]", "1.50[eV]"])], {"F": "F"})
    r = run(m, [('fem_solve', {'study': 's'}), ('fem_evaluate', {'study': 's', 'expression': 'qw.dnTMloc', 'all': True})], tag='f2d', width=200)
    j = json.loads(r[0][1]); print('WARNINGS:', [w[:300] for w in j.get('warnings', [])])
    v = json.loads(r[1][1]); print([ (x['name'], x['value']) for x in v['values'][:3]], [ (x['name'], x['value']) for x in v['values'][58:62]])
