"""p5: 3000 search path folders (as Add Path With SubFolders makes of a vendor tree), the library in the last;
a schematic of 200 of its parts opened and checked; the same with the library in the first."""
import os, time, shutil, mcp
for where in ('last', 'first'):
    s = mcp.Server('p5' + where)
    tree = s.root + '/vendor'
    paths = []
    for i in range(3000):
        d = f'{tree}/d{i:04d}'; os.makedirs(d); paths.append(d)
    shutil.copy(os.environ['MYLIB'], (paths[-1] if where == 'last' else paths[0]) + '/mylib.lib')
    s.call('set_settings', {'scope': 'app', 'values': {'Locations/Library search paths': paths}})
    parts = ''.join(f'  <Lib OPA{n} 1 {100+(n%20)*200} {100+(n//20)*200} -40 70 0 0 "mylib" 0 "opamp" 0 "1e5" 1 "1e6" 1 "1e6" 0 "75" 0 "0.1" 0 "1e-3" 0>\n' for n in range(200))
    open(s.ws + '/many.sch', 'w').write('<Qucs Schematic 26.1.5>\n<Components>\n' + parts + '</Components>\n<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n')
    t = time.time(); r = s.call('open_document', {'path': s.ws + '/many.sch'}); t_open = time.time() - t
    t = time.time(); c = s.call('check_schematic', {'path': 'many.sch'}); t_check = time.time() - t
    st = s.call('get_schematic', {'path': 'many.sch', 'components': ['OPA0']})
    pins = len(st['components'][0].get('pins', [])) if isinstance(st, dict) and st.get('components') else '?'
    print(f'library in the {where} of 3000 paths: open {t_open:.2f} s, check {t_check:.2f} s, OPA0 pins {pins}')
    s.close()
