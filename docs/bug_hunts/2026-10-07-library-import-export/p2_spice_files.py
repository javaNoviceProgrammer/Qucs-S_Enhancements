"""p2: create_library of subcircuits using SPICE files: two of one name in two folders, a .inc, a .mod, a .INCLUDE part; each part simulated."""
import os, re, json, mcp, lib

def dev(name, r1, r2):
    return f'* {name}\n.subckt {name} 1 2\nR1 1 2 {r1}\nR2 2 0 {r2}\n.ends {name}\n'

FILES = {
    'models/dev.lib': dev('DEVA', '1k', '1k'),     # 0.5
    'other/dev.lib': dev('DEVB', '1k', '3k'),      # 0.75
    'vendor.inc': dev('DEVC', '3k', '1k'),         # 0.25
    'vendor.mod': dev('DEVD', '1k', '4k'),         # 0.8
}

def make_sub(s, d, sch, file, device, kind='SpLib'):
    """A subcircuit SCH: P1 and P2 on the pins of a SPICE part DEVICE of FILE (SpLib), or of a raw X line with a .INCLUDE part."""
    s.call('new_document', {})
    s.call('add_component', {'type': 'Port', 'name': 'P1', 'x': 100, 'y': 100})
    s.call('add_component', {'type': 'Port', 'name': 'P2', 'x': 500, 'y': 100})
    if kind == 'SpLib':
        r = s.call('add_component', {'type': 'SpLib', 'name': 'X1', 'x': 300, 'y': 100, 'properties': {'File': file, 'Device': device}})
        if s.last_error: print('  add SpLib', file, r)
        print('   ', sch, 'connect', s.call('connect', {'from': 'P1.1', 'to': 'X1.1'}) and '', s.call('connect', {'from': 'X1.2', 'to': 'P2.1'}) and '')
    else:
        s.call('add_component', {'type': 'SpiceInclude', 'name': 'SpiceInclude1', 'x': 300, 'y': 300, 'properties': {'File': file}})
        s.call('add_component', {'type': 'SpiceModel', 'name': 'SpiceModel1', 'x': 300, 'y': 400, 'properties': {'Line_1': f'XU a b {device}'}})
        s.call('set_label', {'at': 'P1.1', 'name': 'a'})
        s.call('set_label', {'at': 'P2.1', 'name': 'b'})
    s.call('save_document', {'as': d + '/' + sch})
    s.call('close_document', {})

if __name__ == '__main__':
    s = mcp.Server('p2')
    d = lib.project(s, 'sp', FILES)
    make_sub(s, d, 'a.sch', 'models/dev.lib', 'DEVA')
    make_sub(s, d, 'b.sch', 'other/dev.lib', 'DEVB')
    make_sub(s, d, 'c.sch', 'vendor.inc', 'DEVC')
    make_sub(s, d, 'e.sch', 'vendor.mod', 'DEVD', kind='include')
    # Each subcircuit simulated as it is, first: what the library parts should give.
    for sch, want in [('a', 0.5), ('b', 0.75), ('c', 0.25), ('e', 0.8)]:
        out = lib.bench(s, {'type': 'Sub', 'properties': {'File': f'{sch}.sch'}}, d + f'/direct_{sch}.sch')
        lib.show(f'  {sch}.sch itself (want {want}):', out)
    r = s.call('create_library', {'name': 'Sp'})
    lib.show('create:', {'error': s.last_error, 'parts': [p.get('part') for p in r.get('parts', [])] if isinstance(r, dict) else r,
                         'messages': [m for m in r.get('messages', []) if not m.startswith(('Loading', 'Creating', '====='))] if isinstance(r, dict) else None,
                         'models': r.get('models') if isinstance(r, dict) else None})
    text = open(s.ws + '/user_lib/Sp.lib').read() if os.path.isfile(s.ws + '/user_lib/Sp.lib') else ''
    print('attach:', re.findall(r'<SpiceAttach ([^>]*)>', text))
    for root, _, names in sorted(os.walk(s.ws + '/user_lib/Sp')):
        for f in sorted(names):
            rel = os.path.relpath(os.path.join(root, f), s.ws + '/user_lib')
            print('  ' + rel, '->', open(os.path.join(root, f)).read().split('\n')[1])
    if isinstance(r, dict):
        for p, want in zip(r.get('parts', []), [0.5, 0.75, 0.25, 0.8]):
            out = lib.bench(s, p['place'], d + f"/use_{p['part']}.sch")
            lib.show(f"  part {p['part']} (want {want}):", out)
    s.close()
