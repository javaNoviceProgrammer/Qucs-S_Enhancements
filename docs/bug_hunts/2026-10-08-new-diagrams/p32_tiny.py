"""Every new type at tiny sizes (1x1 to 30x20) and 8 panes in 16 px, exported - the ASan build's reports."""
import mcp
from common import rc, show
s = mcp.Server('p32')
p, sim = rc(s)
V, I = 'tran.v(out)', 'tran.v(in)'
specs = [('stacked', [V, {'variable': I, 'pane': 8}], {'panes': 8}), ('bode', [V], {}), ('nichols', [V], {}), ('pole_zero', [V], {}), ('spectrum', [V], {}),
         ('bathtub', [I], {'bathtub': {'unit_interval': 5e-4}}), ('spectrogram', [V], {}), ('tornado', [V, I], {}), ('box_plot', [V, I], {}),
         ('constellation', [I, V], {'constellation': {'modulation': '64qam'}}), ('smith', ['ac.v(out)'], {}), ('polar', ['ac.v(out)'], {'nyquist': {'marks': True, 'mirror': True}})]
bad = 0
for (w, h) in ((1, 1), (5, 3), (16, 16), (30, 20)):
    for t, tr, kw in specs:
        r = s.call('add_diagram', dict(type=t, traces=tr, width=w, height=h, title='T' * 50, legend='top_right', **kw))
        if s.last_error: print(t, w, h, 'refused:', str(r)[:120]); continue
        s.call('add_marker', {'diagram': r['diagram'], 'at': 0.003, 'trace': 1})
        e = s.call('export_image', {'diagram': r['diagram'], 'save_as': s.root + f'/{t}_{w}x{h}.png'})
        if s.last_error: print(t, w, h, 'export:', str(e)[:150])
        if s.p.poll() is not None: print('SERVER DIED at', t, w, h); break
err = open(s.root + '/server.err', errors='replace').read()
print('sanitizer lines:', sorted({l.split(': runtime error')[0].split('/qucs/')[-1] + ': ' + l.split('runtime error: ')[-1][:90] for l in err.splitlines() if 'runtime error' in l})[:10])
print('alive', s.p.poll() is None, s.root)
s.close()
