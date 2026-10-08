"""The constellation's sampling at its edges: an offset past the run, a symbol period longer than the run, a negative offset; get_dataset evm the same (ASan build)."""
import mcp
from common import rc, show
s = mcp.Server('p74')
p, sim = rc(s)
for kw in ({'symbol_period': 5e-4, 'offset': 1}, {'symbol_period': 1}, {'symbol_period': 5e-4, 'offset': -1e-3}, {'symbol_period': 5e-4, 'from': 0.0099}, {'symbol_period': 5e-4, 'modulation': '64qam', 'from': 0.0099}):
    r = s.call('add_diagram', {'type': 'constellation', 'traces': ['tran.v(in)', 'tran.v(out)'], 'constellation': kw})
    show(f'[err={int(s.last_error)}] {kw}', r.get('constellation', {}).get('pairs') if isinstance(r, dict) else r, 300)
g = s.call('get_dataset', {'variables': ['tran.v(in)'], 'measure': ['evm'], 'evm': {'q': 'tran.v(out)', 'symbol_period': 1, 'modulation': 'qpsk'}})
show('get_dataset evm, period 1 s', g['variables'][0]['measurements'] if isinstance(g, dict) and 'variables' in g else g, 400)
err = open(s.root + '/server.err', errors='replace').read()
print('sanitizer:', [l for l in err.splitlines() if 'runtime error' in l][:3], 'alive', s.p.poll() is None)
s.close()
