import sys, json; sys.path.insert(0, '/tmp/qucs-hunt/hunt2')
from probe import run
bar = {"format": "qucs-s multiphysics 1",
  "components": [{"unit": "um", "geometry": [{"type": "rectangle", "label": "bar", "size": ["2", "0.2"]}],
    "materials": [{"material": "Silicon", "selection": {"all": True}}],
    "physics": [{"type": "semiconductor", "tag": "semi", "features": [
      {"type": "doping", "dopant": "donor", "N0": "1e17[1/cm^3]", "selection": {"all": True}},
      {"type": "ohmic", "name": "1", "V0": "0[V]", "selection": {"of": ["bar"], "side": "left"}},
      {"type": "ohmic", "name": "2", "V0": "10[mV]*sin(2*pi*1[GHz]*t) + 2[mV]*sin(2*pi*2[GHz]*t)", "selection": {"of": ["bar"], "side": "right"}}]}],
    "mesh": {"size": "normal", "custom": True, "hmax": "0.1"}}],
  "studies": [{"type": "study", "tag": "s", "steps": [{"type": "stationary"}, {"type": "transient", "times": "range(0, 25[ps], 2[ns])", "stepping": "fixed", "dt": "5[ps]"}]}],
  "results": {"derived": [
     {"type": "harmonics", "tag": "h30", "study": "s", "expression": "semi.I_2", "f0": "1[GHz]", "count": 30, "periods": 1},
     {"type": "harmonics", "tag": "p5", "study": "s", "expression": "semi.I_2", "f0": "1[GHz]", "count": 3, "periods": 5},
     {"type": "harmonics", "tag": "f07", "study": "s", "expression": "semi.I_2", "f0": "0.7[GHz]", "count": 3, "periods": 1},
     {"type": "harmonics", "tag": "c0", "study": "s", "expression": "semi.I_2", "f0": "1[GHz]", "count": 0, "periods": 1},
     {"type": "harmonics", "tag": "cneg", "study": "s", "expression": "semi.I_2", "f0": "1[GHz]", "count": -3, "periods": 0},
     {"type": "harmonics", "tag": "f0big", "study": "s", "expression": "semi.I_2", "f0": "50[GHz]", "count": 3, "periods": 1}]}}
r = run(bar, [('fem_solve', {'study': 's'})], tag='f24', width=0)
j = json.loads(r[0][1]) if not r[0][0] else {'error': r[0][1]}
dv = j.get('derived values')
print(json.dumps(dv)[:4000] if dv else j)
