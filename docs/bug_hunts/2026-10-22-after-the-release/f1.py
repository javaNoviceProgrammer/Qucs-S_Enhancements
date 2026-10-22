import sys; sys.path.insert(0, '/tmp/qucs-hunt/hunt2')
from probe import run
def model(custom):
    return {"format": "qucs-s multiphysics 1", "parameters": [{"name": "c", "expression": "1"}],
     "components": [{"unit": "um", "geometry": [{"type": "rectangle", "name": "r", "size": ["10", "10"]}],
       "materials": [{"material": "Silicon", "selection": {"all": True}}],
       "physics": [{"type": "electrostatics", "tag": "es", "features": [
           {"type": "potential", "V0": "1[V]", "selection": {"of": ["r"], "side": "top"}},
           {"type": "ground", "selection": {"of": ["r"], "side": "bottom"}}]}],
       "mesh": {"size": "normal", "custom": custom, "hmax": "0.2"}}],
     "studies": [{"type": "study", "tag": "s", "steps": [{"type": "stationary"}]}]}
for custom in (True, "c", "c > 0"):
    print('### custom =', custom)
    run(model(custom), [('fem_build', {'what': 'mesh'}), ('fem_solve', {'study': 's'})], tag='f1', width=400)
