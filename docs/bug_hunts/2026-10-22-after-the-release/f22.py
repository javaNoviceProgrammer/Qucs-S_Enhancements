import sys, json; sys.path.insert(0, '/tmp/qucs-hunt/hunt2')
from probe import run
from f2 import well, spec
m = well([], [dict(spec, energies="1.45[eV]"), dict(spec, energies="1.5[eV]", tag="off", enabled=False, excitons="maybe")])
run(m, [('fem_solve', {'study': 's'})], tag='f22', width=250)
m = well([], [dict(spec, energies="1.45[eV]")])
m["components"][0]["physics"].append({"type": "quantum", "tag": "q2", "enabled": False, "light": "maybe"})
run(m, [('fem_solve', {'study': 's'})], tag='f22b', width=250)
