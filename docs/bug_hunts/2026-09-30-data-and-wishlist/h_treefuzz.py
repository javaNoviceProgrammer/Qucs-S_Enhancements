# set_dialog on Find and Replace's tree of results: rows, columns and values out of range or of the wrong kind.
import os, sys, json, shutil, random
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp import Server, HERE
ws = HERE + '/ws_tree'; shutil.rmtree(ws, ignore_errors=True); os.makedirs(ws)
open(HERE + '/settings/qucs/qucs_s.ini', 'w').write('[General]\nDefaultSimulator=1\n')
s = Server(ws)
s.call('import_netlist', {'text': 'lp\nV1 in 0 DC 0 AC 1\nR1 in out 1k\nR2 out 0 1k\nC1 out 0 100n\n.ac dec 20 10 1meg\n.end', 'save_as': ws + '/t.sch'})
random.seed(3); deaths = 0; answers = {}
vals = [[0, 0, True], [0, 0, False], [-1, 0, True], [99, 0, True], [0, 99, True], [0, -5, 'x'], [0, 1, 'text'], [0, 0, 'yes'], [0], [], [None, None, None],
        [1e9, 0, True], ['0', '0', True], [0, 0, {}], 'R1', 3, None, [0, 0, True, 5], [2**31, 2**31, False]]
for k in range(60):
    try:
        s.call('trigger_action', {'action': 'Edit > Replace...', 'path': 't.sch'}, ok=False)
        s.call('set_dialog', {'set': [{'control': 'Find', 'value': random.choice(['1k', 'R', '', 'zzz', '100n'])},
                                      {'control': 'Replace with', 'value': '2k'}, {'control': 'Component type', 'value': 'R_SPICE'}], 'press': 'Find'}, ok=False)
        d = s.call('get_dialog', {}, ok=False)
        tree = [c for c in d.get('controls', []) if c.get('kind') == 'tree'] if isinstance(d, dict) else []
        if not tree: print('no tree:', str(d)[:200]); s.call('set_dialog', {'press': 'Close'}, ok=False); continue
        v = random.choice(vals)
        r = s.call('set_dialog', {'set': [{'control': tree[0]['id'], 'value': v}], 'press': random.choice(['Replace Checked', 'Find', 'Close'])}, ok=False)
        key = json.dumps(v)
        answers.setdefault(key, (r if isinstance(r, str) else json.dumps(r))[:160])
        s.call('set_dialog', {'press': 'Close'}, ok=False)
    except Exception as e:
        deaths += 1; print('death', k, repr(e)[:100]); s = Server(ws)
for k, a in answers.items(): print(k, '->', a)
print('deaths', deaths)
s.close()
