"""p7: library parts with the ground pin (ground_pin true), and of a subcircuit whose ports are numbered in another order than drawn."""
import os, re, json, mcp, lib
s = mcp.Server('p7')
# swapped.sch: P1 (left, on R1's input) is port 2, P2 (the divider's output) is port 1.
swapped = lib.divider('1k', '3k').replace('<Port P1 1 220 100 -23 12 0 0 "1"', '<Port P1 1 220 100 -23 12 0 0 "2"') \
                                   .replace('<Port P2 1 280 100 4 12 1 2 "2"', '<Port P2 1 280 100 4 12 1 2 "1"')
d = lib.project(s, 'pins', {'div.sch': lib.divider('1k', '3k'), 'swapped.sch': swapped})
for sch in ['div', 'swapped']:
    lib.show(f'  {sch}.sch as Sub, pins as numbered (1 = in):', lib.bench(s, {'type': 'Sub', 'properties': {'File': f'{sch}.sch'}}, d + f'/direct_{sch}.sch'))
for gp in [False, True]:
    r = s.call('create_library', {'name': f'Pins{int(gp)}', 'ground_pin': gp})
    text = open(s.ws + f'/user_lib/Pins{int(gp)}.lib').read()
    print(f'ground_pin {gp}:', s.last_error, re.findall(r'\.SUBCKT \S+ [^\n]*', text))
    for p in r.get('parts', []):
        dp = s.call('describe_part', {'library': f'Pins{int(gp)}', 'part': p['part']})
        out = lib.bench(s, p['place'], d + f"/use_{p['part']}_{int(gp)}.sch")
        v = re.search(r'"value": ([-0-9.e]+)', out.get('v(out)', '')) if 'v(out)' in out else None
        print(f"  {p['part']}: pins {[(q['pin'], q['name']) for q in dp.get('pins', [])] if isinstance(dp, dict) else dp}",
              'v(out) =', v.group(1) if v else json.dumps(out)[:500])
s.close()
