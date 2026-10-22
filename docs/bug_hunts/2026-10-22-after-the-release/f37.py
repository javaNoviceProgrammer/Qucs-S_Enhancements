import sys, json; sys.path.insert(0, '/tmp/qucs-hunt/hunt2')
from probe import run
def hj(space, mesh_features, hmax, more):
    ph = {"type": "semiconductor", "tag": "semi", "space": space, "features": [
          {"type": "doping", "dopant": "donor", "N0": "1e17[1/cm^3]", "selection": {"all": True}},
          {"type": "ohmic", "name": "a", "V0": "Va", "selection": {"of": ["g"], "side": "bottom"}},
          {"type": "ohmic", "name": "k", "V0": "0[V]", "selection": {"of": ["b"], "side": "top"}}]}
    ph.update(more)
    return {"format": "qucs-s multiphysics 1", "parameters": [{"name": "Va", "expression": "0.1[V]"}],
      "components": [{"unit": "um",
        "geometry": [{"type": "rectangle", "name": "g", "position": ["-1", "0"], "size": ["2", "0.2"]},
                     {"type": "rectangle", "name": "b", "position": ["-1", "0.2"], "size": ["2", "0.2"]}],
        "materials": [{"material": "GaAs", "compound": "GaAs", "x": "0", "selection": {"only": ["g"]}},
                      {"material": "AlGaAs", "compound": "AlGaAs", "x": "0.3", "selection": {"only": ["b"]}}],
        "physics": [ph], "mesh": {"size": "normal", "custom": True, "hmax": hmax, "features": mesh_features}}],
      "studies": [{"type": "study", "tag": "s", "steps": [{"type": "stationary"}]}]}
layers = [{"type": "layered", "hlateral": "0.25", "count": 80, "selection": {"only": ["g", "b"]}}]
ys = [0.17, 0.19, 0.195, 0.1985, 0.2015, 0.205, 0.21, 0.23, 0.25]
def values(m, tag, x):
    calls = [('fem_solve', {'study': 's'})] + [('fem_evaluate', {'study': 's', 'expression': e, 'at': [[x, y]]}) for y in ys for e in ('semi.V', 'semi.n', 'semi.Ec')]
    r = run(m, calls, tag=tag, width=0)
    if r[0][0]: print(tag, r[0][1][:300]); return {}
    out = {}
    for (err, text), (name, a) in zip(r[1:], calls[1:]):
        try: out[(a['at'][0][1], a['expression'])] = float(json.loads(text)['values'][0]['SI'])
        except Exception: out[(a['at'][0][1], a['expression'])] = text[:80]
    return out
plane = values(hj("2d", layers, "0.5", {}), 'f37p', 0)
coarse = values(hj("1d", [], "0.07", {"h1d": "2.5[nm]"}), 'f37c', 0.3)
for y in ys:
    print('y %.4f: n 1D %-12.5g 2D %-12.5g (x%.3g)   Ec 1D %.4f 2D %.4f eV' % (y, coarse[(y, 'semi.n')], plane[(y, 'semi.n')], coarse[(y, 'semi.n')] / plane[(y, 'semi.n')], coarse[(y, 'semi.Ec')] / 1.602e-19, plane[(y, 'semi.Ec')] / 1.602e-19))
