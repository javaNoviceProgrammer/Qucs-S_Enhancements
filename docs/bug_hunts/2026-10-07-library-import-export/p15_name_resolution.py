"""p15: create_library's 'subcircuits' by name when two match (div.sch and sub/div.sch), and 'descriptions' by name."""
import os, re, json, mcp, lib
s = mcp.Server('p15')
d = lib.project(s, 'nr', {'div.sch': lib.divider('1k', '3k'), 'sub/div.sch': lib.divider('3k', '1k'), 'zz/div.sch': lib.divider('1k', '1k')})
s.call('get_state', {})
for ask in [['div'], ['div.sch'], ['sub/div']]:
    r = s.call('create_library', {'name': 'Pick', 'subcircuits': ask, 'replace': True, 'descriptions': {'div': 'the top one'}})
    text = open(s.ws + '/user_lib/Pick.lib').read() if os.path.isfile(s.ws + '/user_lib/Pick.lib') else ''
    print(f'asked {ask}:', s.last_error, 'loaded', [m for m in r.get('messages', []) if m.startswith('Loading')] if isinstance(r, dict) else r,
          '| R2 values:', re.findall(r'R2 0 P2\s+(\S+)', text), '| description:', re.findall(r'<Description>\n(.*?)\n', text))
s.close()
