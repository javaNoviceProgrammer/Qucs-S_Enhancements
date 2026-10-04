"""p28: a binary dataset of a project written out by the Content panel's Save as Text: the same as the text run's file?"""
import os, json, mcp
def run(binary):
    s = mcp.Server('p28' + ('b' if binary else 't'))
    if binary: s.call('set_settings', {'scope': 'simulators', 'values': {'Results/Keep large results binary': True, 'Results/Binary above': 0}})
    s.call('new_project', {'name': 'd'}); s.call('open_project', {'name': 'd'})
    p = s.ws + '/d_prj'
    s.call('import_netlist', {'text': 'rc\nV1 in 0 DC 1 AC 1\nR1 in out 1k\nC1 out 0 1n\n.ac dec 5 1k 10meg\n.end', 'save_as': p + '/rc.sch'})
    s.call('add_analysis', {'path': 'rc.sch', 'kind': 'sweep', 'analysis': 'AC1', 'parameter': 'C1', 'from': '1n', 'to': '3n', 'points': 2})
    s.call('simulate', {'path': 'rc.sch'})
    out = None
    if binary:
        m = s.call('context_menu', {'on': {'project_item': 'rc.dat.ngspice'}})
        print('menu:', json.dumps(m)[:400])
        r = s.call('context_menu', {'on': {'project_item': 'rc.dat.ngspice'}, 'choose': 'Save as Text'})
        print('Save as Text:', s.last_error, str(r)[:300])
        d = s.call('get_dialog', {}); print('dialog:', json.dumps(d)[:500])
        files = sorted(os.listdir(p)); print('files:', files)
        cands = [f for f in files if f.startswith('rc.dat') and f != 'rc.dat.ngspice']
        out = open(p + '/' + cands[0]).read() if cands else None
    else:
        out = open(p + '/rc.dat.ngspice').read()
    s.close(); return out
t = run(False); b = run(True)
print('text run:', len(t or ''), 'chars; saved as text:', len(b or '') if b else b)
if b: print('same:', t == b); 
if b and t != b:
    import difflib; print(''.join(list(difflib.unified_diff(t.splitlines(1), b.splitlines(1)))[:30]))
