"""p10: tools that take a name or a path, given "" and " ": refused, or something done?"""
import os, json, mcp
def snapshot(s):
    files = sorted(os.path.relpath(os.path.join(r, f), s.ws) for r, _, fs in os.walk(s.ws) for f in fs)
    st = s.call('get_state', {})
    comps = s.call('get_schematic', {'path': 'a.sch'}) if os.path.isfile(s.ws + '/a.sch') else {}
    names = sorted(c['name'] for c in comps.get('components', [])) if isinstance(comps, dict) else comps
    return files, len(st.get('documents', [])), names, sorted(os.listdir(s.root + '/trash'))
cases = [
    ('delete', {'path': 'a.sch', 'names': ['']}), ('delete', {'path': 'a.sch', 'names': [' ']}),
    ('select', {'path': 'a.sch', 'names': ['']}), ('edit_component', {'path': 'a.sch', 'name': '', 'properties': {'R': '2k'}}),
    ('move', {'path': 'a.sch', 'names': [''], 'dx': 10, 'dy': 0}), ('rename_net', {'path': 'a.sch', 'from': '', 'to': 'q'}),
    ('close_document', {'path': ''}), ('close_document', {'path': ' '}), ('show_document', {'path': ''}),
    ('open_document', {'path': ''}), ('open_document', {'path': ' '}), ('save_document', {'path': '', 'as': ''}),
    ('copy_document', {'path': 'a.sch', 'to': ''}), ('copy_document', {'path': 'a.sch', 'to': ' '}),
    ('rename_file', {'path': '', 'to': 'x'}), ('rename_file', {'path': 'keep.txt', 'to': ''}), ('rename_file', {'path': 'keep.txt', 'to': ' '}),
    ('trash_file', {'path': ''}), ('trash_file', {'path': ' '}), ('trash_file', {'path': '.'}),
    ('clean_scratch', {'path': ''}), ('set_dialog', {'press': ''}), ('context_menu', {'item': ''}),
    ('get_ui', {'area': ''}), ('set_ui', {'area': '', 'shown': False}), ('describe_component_type', {'type': ''}),
    ('read_help', {'topic': ''}), ('ngspice_commands', {'command': ''}), ('new_project', {'name': ''}), ('new_project', {'name': ' '}),
    ('open_project', {'name': ''}), ('open_project', {'name': ' '}), ('import_library', {'path': ''}), ('list_libraries', {'library': ' '}),
    ('add_component', {'path': 'a.sch', 'type': '', 'x': 0, 'y': 0}), ('add_component', {'path': 'a.sch', 'type': 'R', 'name': ' ', 'x': 0, 'y': 300}),
    ('set_label', {'path': 'a.sch', 'at': 'R1.1', 'name': ''}), ('set_label', {'path': 'a.sch', 'at': 'R1.1', 'name': ' '}),
]
s = mcp.Server('p10')
s.call('import_netlist', {'text': 't\nR1 a b 1k\nR2 b 0 2k\nV1 a 0 1\n.op\n.end', 'save_as': s.ws + '/a.sch'})
open(s.ws + '/keep.txt', 'w').write('keep\n')
for tool, args in cases:
    before = snapshot(s)
    r = s.call(tool, args)
    after = snapshot(s)
    changed = before != after
    flag = 'CHANGED' if changed and not s.last_error else ('changed+err' if changed else '')
    print(f'{tool:22s} {json.dumps(args)[:60]:60s} err={str(s.last_error):5s} {flag:12s} {str(r)[:110]!r}')
    if changed:
        for i, lab in enumerate(('files', 'docs', 'components', 'trash')):
            if before[i] != after[i]: print('      ', lab, str(before[i])[:120], '->', str(after[i])[:160])
s.close()
