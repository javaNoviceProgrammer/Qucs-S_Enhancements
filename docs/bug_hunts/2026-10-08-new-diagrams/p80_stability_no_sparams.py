"""Stability and Smith circles asked of a run with no S-parameters (the RC's transient and FFT): the answers."""
import mcp
from common import rc, show
s = mcp.Server('p80')
p, sim = rc(s)
g = s.call('get_dataset', {'variables': ['ac.v(out)'], 'measure': ['stability']})
show('get_dataset stability of ac.v(out)', g['variables'][0].get('measurements') if isinstance(g, dict) and 'variables' in g else g, 400)
r = s.call('add_diagram', {'type': 'smith', 'traces': ['ac.v(out)'], 'smith_circles': {'circles': [{'kind': 'stability_in'}, {'kind': 'gain', 'level': 3}]}})
show('smith circles', r.get('smith_circles') if isinstance(r, dict) else r, 400)
s.close()
