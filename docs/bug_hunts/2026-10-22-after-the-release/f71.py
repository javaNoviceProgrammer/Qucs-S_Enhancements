# the ASan spectrum fuzzer's NONFINITE cases, in Release: where the "nan" is
import sys, json, re
sys.path.insert(0, '/tmp/qucs-hunt/hunt2')
from probe import run
for tag in sys.argv[1:]:
    m = json.load(open('fz/%s.qfem' % tag))
    r = run(m, [('fem_solve', {'study': 's'}), ('fem_evaluate', {'study': 's', 'expression': 'qw.alphaTE', 'all': True})], tag='f71' + tag, width=100000)
    for (e, text) in r:
        for mt in re.finditer(r'nan|inf', text, re.I):
            print(tag, 'error' if e else 'answer', '...' + text[max(0, mt.start() - 160):mt.end() + 40].replace('\n', ' ') + '...')
            break
