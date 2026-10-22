import sys, json; sys.path.insert(0, '/tmp/qucs-hunt/hunt2')
from probe import run
def bar(contact2):
    c2 = {"type": "ohmic", "name": "2", "V0": "10[mV]", "selection": {"of": ["bar"], "side": "right"}}; c2.update(contact2)
    return {"format": "qucs-s multiphysics 1",
      "components": [{"unit": "um", "geometry": [{"type": "rectangle", "label": "bar", "size": ["2", "0.2"]}],
        "materials": [{"material": "Silicon", "selection": {"all": True}}],
        "physics": [{"type": "semiconductor", "tag": "semi", "features": [
          {"type": "doping", "dopant": "donor", "N0": "1e17[1/cm^3]", "selection": {"all": True}},
          {"type": "ohmic", "name": "1", "V0": "0[V]", "selection": {"of": ["bar"], "side": "left"}}, c2]}],
        "mesh": {"size": "normal", "custom": True, "hmax": "0.1"}}],
      "studies": [{"type": "study", "tag": "s", "steps": [{"type": "stationary"}]}]}
def I(c):
    r = run(bar(c), [('fem_solve', {'study': 's'}), ('fem_evaluate', {'study': 's', 'expression': 'semi.I_2'}), ('fem_evaluate', {'study': 's', 'expression': 'semi.V_2'})], tag='f26', width=0)
    if r[0][0]: return r[0][1][:200]
    return json.loads(r[1][1])['values'][0]['SI'], json.loads(r[2][1])['values'][0]['SI'] if not r[2][0] else r[2][1][:100], json.loads(r[0][1]).get('warnings')
I0 = I({}); print('none', I0)
R = 0.01 / I0[0]; print('R bar %.6g Ohm' % R)
for c in ({"circuit": "series", "Rs": "%.9g[Ohm]" % R, "RL": "0[Ohm]"}, {"circuit": "series", "Rs": "0[Ohm]", "RL": "%.9g[Ohm]" % (3 * R)},
          {"circuit": "biastee", "Rs": "%.9g[Ohm]" % R, "RL": "50[Ohm]"}, {"circuit": "series", "Rs": "0[Ohm]", "RL": "0[Ohm]"}, {"circuit": "series", "Rs": "-%.9g[Ohm]" % (0.5 * R), "RL": "0[Ohm]"}):
    v = I(c); print(c, '->', v, ' ratio to none: %.6f' % (v[0] / I0[0]) if isinstance(v, tuple) else '')
