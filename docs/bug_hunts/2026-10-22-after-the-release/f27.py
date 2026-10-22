import sys, json; sys.path.insert(0, '/tmp/qucs-hunt/hunt2')
from probe import run
def bar(c2extra):
    c2 = {"type": "ohmic", "name": "2", "V0": "0[V]", "selection": {"of": ["bar"], "side": "right"}}; c2.update(c2extra)
    return {"format": "qucs-s multiphysics 1",
      "components": [{"unit": "um", "geometry": [{"type": "rectangle", "label": "bar", "size": ["2", "0.2"]}],
        "materials": [{"material": "Silicon", "selection": {"all": True}}],
        "physics": [{"type": "semiconductor", "tag": "semi", "noise": True, "features": [
          {"type": "doping", "dopant": "donor", "N0": "1e17[1/cm^3]", "selection": {"all": True}},
          {"type": "ohmic", "name": "1", "V0": "0[V]", "selection": {"of": ["bar"], "side": "left"}}, c2]}],
        "mesh": {"size": "normal", "custom": True, "hmax": "0.05"}}],
      "studies": [{"type": "study", "tag": "s", "steps": [{"type": "stationary"}, {"type": "frequency", "frequencies": "1[MHz]"}]}]}
kT = 1.380649e-23 * 300
def g(c):
    r = run(bar(c), [('fem_solve', {'study': 's'})], tag='f27', width=0)
    if r[0][0]: return r[0][1][:300]
    return {k: v for k, v in json.loads(r[0][1])['globals'].items() if k.startswith('semi.Sn') or k.startswith('semi.G2') or k.startswith('semi.in')}
a = g({}); print('alone', a)
R = 0.00445822077
for c in ({"circuit": "series", "Rs": "%.9g[Ohm]" % R, "RL": "0[Ohm]"}, {"circuit": "series", "Rs": "0[Ohm]", "RL": "%.9g[Ohm]" % R}):
    print(c, g(c))
print('4kT/R %.4g, 4kT/(2R) %.4g, 4kT/R/4 %.4g A^2/Hz' % (4 * kT / R, 4 * kT / (2 * R), 4 * kT / R / 4))
