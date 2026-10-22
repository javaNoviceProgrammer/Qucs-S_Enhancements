import sys, json; sys.path.insert(0, '/tmp/qucs-hunt/hunt2')
from probe import run
exec(open('/tmp/qucs-hunt/hunt2/f1.py').read().split('for custom')[0])
m = model(True)
m["results"] = {"plots": [{"type": "plotgroup1d", "tag": "pg", "study": "s", "label": "x", "ylog": "maybe", "plots": [{"type": "globalgraph", "tag": "g", "expression": "es.W"}]}]}
run(m, [('fem_solve', {'study': 's'})], tag='f14', width=300)
