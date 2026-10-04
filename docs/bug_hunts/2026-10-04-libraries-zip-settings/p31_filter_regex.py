"""p31: the Content panel's Filter by name with regular expressions: an invalid one, an empty match, case, a heavy one."""
import mcp, json, time, os
s = mcp.Server('p31')
s.call('new_project', {'name': 'f'}); s.call('open_project', {'name': 'f'})
p = s.ws + '/f_prj'
for n in ('amp.sch', 'Amp2.sch', 'filter_lp.sch', 'notes.txt', 'a(b).sch', 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.sch'):
    open(f'{p}/{n}', 'w').write('<Qucs Schematic 26.1.5>\n<Components>\n</Components>\n<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n' if n.endswith('.sch') else 'x')
import time as _t
_t.sleep(4)
s.call('set_ui', {'area': 'dock:Content', 'set': [{'control': 'tree view', 'value': 'Schematics', 'action': 'expand'}]})
def rows():
    u = s.call('get_ui', {'area': 'dock:Content'})
    tv = next(c for c in u['controls'] if c.get('kind') == 'tree view')
    return [r['text'] for r in tv.get('rows', [])], next(c for c in u['controls'] if c.get('kind') == 'field').get('value')
for f in ('amp', 'AMP', '^amp', '(', 'a(b', '\\(b\\)', '.*', '', '(a+)+$', '[', 'amp|notes', 're:amp', '/amp/'):
    t = time.time()
    r = s.call('set_ui', {'area': 'dock:Content', 'set': [{'control': 'Filter by name', 'value': f}]})
    took = time.time() - t
    rs, val = rows()
    print(f'{f!r:12} {took:5.2f}s err={s.last_error} rows={rs[:9]}')
s.close()
