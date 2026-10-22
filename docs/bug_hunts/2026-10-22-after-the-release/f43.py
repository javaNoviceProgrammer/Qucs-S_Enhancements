import sys, json, time, os
S = '/tmp/qucs-hunt'
sys.path.insert(0, S)
from mcp_client import Client
D = S + '/hunt2/kfr'; os.makedirs(D, exist_ok=True)
model = {"format": "qucs-s multiphysics 1", "components": [{"unit": "um",
  "geometry": [{"type": "process", "tag": "pf1", "width": "2", "dx": "20[nm]", "dy": "1[nm]", "depth": "1", "features": [
      {"type": "substrate", "tag": "sub", "label": "si", "material": "Silicon", "thickness": "2", "dopant": "P", "concentration": "1e15"},
      {"type": "implant", "tag": "imp", "species": "B", "energy": "40", "dose": "1e15"},
      {"type": "diffuse", "tag": "anl", "temperature": "1000[degC]", "time": "1200[min]"}]},
    {"type": "form union", "tag": "fin"}],
  "materials": [{"tag": "m1", "material": "Silicon", "selection": {"all": True}}],
  "physics": [{"type": "electrostatics", "tag": "es", "features": [
      {"type": "terminal", "name": "1", "V0": "1[V]", "selection": {"of": ["si"], "side": "top"}},
      {"type": "ground", "selection": {"of": ["si"], "side": "bottom"}}]}],
  "mesh": {"size": "normal", "custom": True, "hmax": "0.2"}}],
  "studies": [{"type": "study", "tag": "std1", "steps": [{"type": "stationary"}]}]}
json.dump(model, open(D + '/flow.qfem', 'w'))
c = Client(D + '/w2')
for name, args in (('open_document', {'path': D + '/flow.qfem'}), ('fem_build', {'path': D + '/flow.qfem', 'timeout': 1}), ('fem_describe', {'path': D + '/flow.qfem'}),
                   ('fem_describe', {'path': D + '/flow.qfem', 'what': 'geometry'}), ('fem_evaluate', {'path': D + '/flow.qfem', 'expression': '1+1'}), ('get_state', {}), ('fem_stop', {'path': D + '/flow.qfem'})):
    t = time.time(); e, r = c.call(name, args); print(name, json.dumps(args)[-40:], e, '%.2f s' % (time.time() - t), r[:150].replace('\n', ' '))
c.close()
