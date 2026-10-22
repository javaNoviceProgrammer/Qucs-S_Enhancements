import sys, json; sys.path.insert(0, '/tmp/qucs-hunt/hunt2')
from probe import run
def junction(space, mesh_features, hmax, more):
    ph = {"type": "semiconductor", "tag": "semi", "space": space, "features": [
          {"type": "doping", "dopant": "acceptor", "N0": "1e17[1/cm^3]", "selection": {"only": ["p"]}},
          {"type": "doping", "dopant": "donor", "N0": "1e17[1/cm^3]", "selection": {"only": ["n"]}},
          {"type": "ohmic", "name": "a", "V0": "Va", "selection": {"of": ["p"], "side": "bottom"}},
          {"type": "ohmic", "name": "k", "V0": "0[V]", "selection": {"of": ["n"], "side": "top"}}]}
    ph.update(more)
    return {"format": "qucs-s multiphysics 1", "parameters": [{"name": "Va", "expression": "0.5[V]"}],
      "components": [{"unit": "um",
        "geometry": [{"type": "rectangle", "name": "p", "position": ["-1", "0"], "size": ["2", "0.2"]},
                     {"type": "rectangle", "name": "n", "position": ["-1", "0.2"], "size": ["2", "0.2"]}],
        "materials": [{"material": "Silicon", "selection": {"all": True}}],
        "physics": [ph], "mesh": {"size": "normal", "custom": True, "hmax": hmax, "features": mesh_features}}],
      "studies": [{"type": "study", "tag": "s", "steps": [{"type": "stationary"}]}]}
layers = [{"type": "layered", "hlateral": "0.25", "count": 40, "selection": {"only": ["p", "n"]}}]
ys = [0.0031 + 0.0127 * k for k in range(31)]
def values(m, tag, x):
    calls = [('fem_solve', {'study': 's'})] + [('fem_evaluate', {'study': 's', 'expression': e, 'at': [[x, y]]}) for y in ys for e in ('semi.phin', 'semi.V', 'semi.n', 'semi.Efn', 'semi.Ec')]
    r = run(m, calls, tag=tag, width=0)
    out = {}
    for (err, text), (name, a) in zip(r[1:], calls[1:]):
        try: out[(a['at'][0][1], a['expression'])] = json.loads(text)['values'][0]['SI']
        except Exception: out[(a['at'][0][1], a['expression'])] = text[:80]
    return out
plane = values(junction("2d", layers, "0.5", {}), 'f6p', 0)
coarse = values(junction("1d", [], "0.07", {"h1d": "5[nm]"}), 'f6c', 0.3)
worst = {}
for y in ys:
    for e in ('semi.phin', 'semi.V', 'semi.n', 'semi.Efn', 'semi.Ec'):
        a, b = coarse[(y, e)], plane[(y, e)]
        if not isinstance(a, float) or not isinstance(b, float):
            bad = bad + 1 if 'bad' in dir() else 1
            if bad < 3: print(y, e, str(a)[:120], str(b)[:120])
            continue
        d = abs(a - b) / (abs(b) if e == 'semi.n' else (1.602e-19 if e in ('semi.Efn', 'semi.Ec') else 1))
        if d > worst.get(e, (0,))[0]: worst[e] = (d, y, a, b)
for e, w in worst.items(): print('%-10s worst %.3g at y %.4f: coarse %.6g, 2D %.6g' % (e, *w))
