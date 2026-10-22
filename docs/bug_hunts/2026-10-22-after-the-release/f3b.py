import sys, json; sys.path.insert(0, '/tmp/qucs-hunt/hunt2')
from probe import run
from f2 import well, spec
m = well([], [dict(spec, tag="sp", epsr="12.9")])
r = run(m, [('fem_edit', {'action': 'set', 'tag': 'sp', 'props': {'excitons': 1}}), ('fem_model', {'action': 'save'}), ('fem_solve', {'study': 's'})], tag='f3b', width=300)
print('B number 1 -> excitons on?', 'qw.Eb' in r[2][1])
print([s for s in json.load(open(sys.path[0] + '/f3b.qfem'))['studies'][0]['steps']][0].get('excitons'))
m = well([], [dict(spec, tag="sp", epsr="12.9")])
r = run(m, [('fem_edit', {'action': 'set', 'tag': 'sp', 'props': {'excitons': "yes"}}), ('fem_solve', {'study': 's'})], tag='f3c', width=300)
