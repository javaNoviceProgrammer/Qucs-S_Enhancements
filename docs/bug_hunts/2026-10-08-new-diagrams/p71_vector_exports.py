"""Every new type exported as SVG and PDF (other paint devices than the PNG's), the whole schematic too: ASan reports, file sizes."""
import os, mcp
from common import rc, show
s = mcp.Server('p71')
p, sim = rc(s)
T, F = 'tran.v(out)', 'ac.v(out)'
specs = [('stacked', [T, {'variable': 'tran.v(in)', 'pane': 2}], {'limits': [{'upper': 0.9, 'pane': 2}]}), ('bode', [F], {}), ('nichols', [F], {}), ('pole_zero', [F], {}),
         ('spectrum', [T], {}), ('bathtub', ['tran.v(in)'], {'bathtub': {'unit_interval': 5e-4}}), ('contour', [T], {}), ('spectrogram', [T], {}),
         ('tornado', [T, 'tran.v(in)'], {'tornado': {'mode': 'spread'}}), ('box_plot', [T], {}), ('constellation', ['tran.v(in)', T], {}), ('polar', [F], {'nyquist': {'marks': True, 'mirror': True}})]
for t, tr, kw in specs:
    r = s.call('add_diagram', dict(type=t, traces=tr, **kw))
    out = []
    for fmt in ('svg', 'pdf'):
        e = s.call('export_image', {'diagram': r['diagram'], 'save_as': s.root + f'/{t}.{fmt}'})
        out.append(f'{fmt} ' + ('err ' + str(e)[:60] if s.last_error else str(os.path.getsize(s.root + f'/{t}.{fmt}'))))
    print(f'{t:14s}', ', '.join(out), flush=True)
    if s.p.poll() is not None: print('SERVER DIED'); break
e = s.call('export_image', {'save_as': s.root + '/all.pdf'}); print('whole schematic pdf:', 'err' if s.last_error else os.path.getsize(s.root + '/all.pdf'))
err = open(s.root + '/server.err', errors='replace').read()
print('sanitizer:', [l for l in err.splitlines() if 'runtime error' in l or 'Sanitizer' in l][:3], 'alive', s.p.poll() is None)
s.close()
