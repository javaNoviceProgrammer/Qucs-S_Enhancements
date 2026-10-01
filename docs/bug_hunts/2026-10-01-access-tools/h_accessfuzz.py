# Usage: python3 h_accessfuzz.py <qucs-s binary, the ASan build's> <seed> <seconds> <a scratch folder> <the repository>
# then look for "AddressSanitizer" or "runtime error" in <scratch folder>/server.err.
# Random calls with junk arguments to the tools of the access rounds (get_ui, set_ui,
# context_menu, console's reading, the settings, the text tabs, rename_file and trash_file
# with undo, wait_for, background simulations, send_input, read_help), on an ASan build.
# Nothing here runs a command: console never gets 'input' or 'interrupt', send_input's keys
# are a short harmless list, simulate never 'allow_commands'. Files stay in the workspace;
# the trash, settings and HOME are this run's own.
import json, os, sys, random, time, shutil, subprocess
APP = sys.argv[1]; random.seed(int(sys.argv[2])); budget = float(sys.argv[3])
ROOT = os.path.abspath(sys.argv[4]); REPO = sys.argv[5]
WS = ROOT + '/ws'; TRASH = ROOT + '/trash'
for d in (ROOT + '/settings', ROOT + '/home', TRASH): os.makedirs(d, exist_ok=True)
EXAMPLE = REPO + '/qucs-s-26.1.1/examples/ngspice/RF/Miscellaneous/RCL_resonance.sch'

class Server:
    def __init__(self):
        env = dict(os.environ, QT_QPA_PLATFORM='offscreen', QUCS_SETTINGS_DIR=ROOT + '/settings', QUCS_NO_SHELL_ENV='1',
                   HOME=ROOT + '/home', QUCS_TRASH_DIR=TRASH, QUCS_LIBRARY_DIR=REPO + '/qucs-s-26.1.1/library',
                   ASAN_OPTIONS='detect_leaks=0:halt_on_error=1', UBSAN_OPTIONS='print_stacktrace=1')
        self.p = subprocess.Popen([APP, '--mcp-server', '--workspace', WS], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=open(ROOT + '/server.err', 'a'), env=env, text=True)
        self.n = 0
        self.rpc('initialize', {'protocolVersion': '2025-06-18', 'capabilities': {}})
        self.p.stdin.write(json.dumps({'jsonrpc': '2.0', 'method': 'notifications/initialized'}) + '\n'); self.p.stdin.flush()
    def rpc(self, method, params):
        self.n += 1; i = self.n
        self.p.stdin.write(json.dumps({'jsonrpc': '2.0', 'id': i, 'method': method, 'params': params}) + '\n'); self.p.stdin.flush()
        while True:
            line = self.p.stdout.readline()
            if not line: raise RuntimeError('server died')
            m = json.loads(line)
            if m.get('id') == i: return m
    def call(self, tool, args):
        return self.rpc('tools/call', {'name': tool, 'arguments': args})
    def close(self):
        self.p.stdin.close(); self.p.wait(timeout=60)

scalars = [None, True, False, 0, 1, -1, 2, 3, 2**31, -2**31, 1e308, 0.5, '', ' ', 'x', 'X', 'é漢字', '\u0000', 'a' * 3000,
           'c1', 'c2', 'c3', 'c9', 'R1', 'C1', 'L1', 'b.sch', 'notes.txt', 'net.cir', 'sub', 'nothing.txt', '../x.txt']
def junk():
    r = random.random()
    if r < 0.6: return random.choice(scalars)
    if r < 0.8: return [random.choice(scalars) for _ in range(random.randint(0, 3))]
    return {random.choice(['x', 'part', 'canvas', 'control', 'value', 'find', 'replace', 'lines', 'text', 'Settings/Language']):
            random.choice(scalars) for _ in range(random.randint(0, 3))}
files = ['b.sch', 'notes.txt', 'net.cir', 'sub', 'sub/c.txt', 'nothing.txt', 'b2.sch', 'n2.txt']
points = [[100, 100], [0, 0], [-5000, 5000], [1e9, 1], [100], [], 'x', [160, 200], [300, 100]]
tools = {
  'get_ui': {'area': lambda: random.choice(['', 'dock:Content', 'dock:Components', 'dock:Problems', 'dock:Main Dock/Projects', 'statusbar',
                                            'tabs', 'toolbar:File', 'toolbar:Simulate', 'dock:Terminal', 'dock:Claude Code', 'dock:', 'x', junk()])},
  'set_ui': {'area': lambda: random.choice(['dock:Content', 'dock:Components', 'statusbar', 'tabs', 'dock:Problems', 'dock:Terminal', junk()]),
             'set': lambda: random.choice([[{'control': random.choice(['c1', 'c2', 'c3', 'c4', 'x', 1]), 'value': junk()}], [], junk()]),
             'press': lambda: random.choice(['', 'OK', 'c1', junk()])},
  'context_menu': {'on': lambda: random.choice([{'part': 'R1'}, {'canvas': random.choice(points)}, {'diagram': 1}, {'tab': 'b.sch'},
                                                {'file': random.choice(files)}, {'project_item': 'b.sch'}, {}, junk()]),
                   'choose': lambda: random.choice(['', 'Delete', 'Rename', 'Copy', 'Flat', 'zz', junk()]),
                   'path': lambda: random.choice(['', 'b.sch', junk()])},
  'console': {'kind': lambda: random.choice(['octave', 'python', 'terminal', 'x', junk()]), 'lines': lambda: random.choice([1, 0, -1, 2**31, junk()]),
              'wait': lambda: random.choice([1, 0, junk()])},
  'get_settings': {'scope': lambda: random.choice(['app', 'simulators', 'document', 'cdl', 'x', junk()]), 'path': lambda: random.choice(['', 'b.sch', 'net.cir', junk()])},
  'set_settings': {'scope': lambda: random.choice(['document', 'cdl', 'x', junk()]),
                   'values': lambda: random.choice([{random.choice(['Grid/horizontal Grid', 'Grid/vertical Grid', 'horizontal Grid', 'Settings/x', 'x', '']):
                                                     random.choice([10, 20, 'abc', True, None, -5, 2**31])}, {}, junk()]),
                   'path': lambda: random.choice(['', 'b.sch', 'net.cir', junk()])},
  'get_text': {'path': lambda: random.choice(['net.cir', 'notes.txt', 'b.sch', '', junk()]), 'from_line': lambda: random.choice([1, 0, -3, 2**31, junk()]),
               'to_line': lambda: random.choice([1, 2, 0, 2**31, junk()])},
  'edit_text': {'path': lambda: random.choice(['net.cir', 'notes.txt', '', junk()]),
                'edits': lambda: random.choice([[{'find': random.choice(['R1', 'x', '', junk()]), 'replace': junk(), 'all': junk()}],
                                                [{'lines': random.choice([[1, 1], [2, 1], [0, 5], [1, 2**31], junk()]), 'text': junk()}], [], junk()]),
                'revision': lambda: random.choice([0, 1, 2, -1, 1e308, junk()])},
  'goto_line': {'path': lambda: random.choice(['net.cir', 'notes.txt', junk()]), 'line': lambda: random.choice([1, 0, 3, 2**31, junk()]),
                'column': lambda: random.choice([1, 0, 99, junk()])},
  'rename_file': {'path': lambda: random.choice(files + [junk()]), 'to': lambda: random.choice(['n3.txt', 'sub', 'b3.sch', '', '..', '.', 'sub/x', 'a/b', junk()])},
  'trash_file': {'path': lambda: random.choice(files + ['', '.', junk()])},
  'undo': {'files': lambda: random.choice([True, 1, 2, 0, -1, junk()])},
  'wait_for': {'event': lambda: random.choice(['simulation_finished', 'dialog', 'document_changed', 'file_written', 'x', junk()]),
               'id': lambda: random.choice([1, 2, 0, -1, 2**31, junk()]), 'path': lambda: random.choice(files + [junk()]),
               'revision': lambda: random.choice([0, 1, 1e308, junk()]), 'timeout': lambda: random.choice([1, 2])},
  'simulation_status': {'id': lambda: random.choice([1, 2, 3, 0, -1, 2**31, junk()])},
  'stop_simulation': {'id': lambda: random.choice([1, 2, 0, junk()])},
  'simulate': {'path': lambda: random.choice(['b.sch', '', junk()]), 'background': lambda: random.choice([True, False, junk()]),
               'timeout': lambda: random.choice([5, 6, junk()]), 'brief': lambda: True},
  'send_input': {'target': lambda: random.choice(['canvas', 'dock:Content', 'dock:Components', 'statusbar', 'toolbar:File', 'tabs', 'dock:Terminal', '', junk()]),
                 'path': lambda: random.choice(['b.sch', '', junk()]), 'click': lambda: random.choice(points + [junk()]),
                 'drag_to': lambda: random.choice(points + [junk()]), 'button': lambda: random.choice(['left', 'right', 'middle', 'x', junk()]),
                 'double': lambda: random.choice([True, False, junk()]), 'modifiers': lambda: random.choice([['shift'], ['ctrl'], [], ['x'], junk()]),
                 'keys': lambda: random.choice(['Escape', 'Delete', 'Ctrl+Z', 'Ctrl+Y', 'Return', 'Tab', 'x', 'Ctrl+Q', 'Escape, Delete', '', 'Nope+K', junk()]),
                 'text': lambda: random.choice(['ab', '', 'é', junk()]), 'pixels': lambda: random.choice([True, False, junk()])},
  'read_help': {'topic': lambda: random.choice(['', 'tuner', 'delete', 'zz', 'a' * 3000, junk()])},
  'get_state': {},
  'list_documents': {'folder': lambda: random.choice(['', '.', 'sub', WS, junk()])},
}
# Never: a command run (console input or interrupt, allow_commands) - not in the lists above.
def setup(s):
    open(WS + '/notes.txt', 'w').write('one\ntwo\nthree\n')
    open(WS + '/net.cir', 'w').write('* net\nR1 1 0 1k\nV1 1 0 1\n.op\n.end\n')
    os.makedirs(WS + '/sub', exist_ok=True); open(WS + '/sub/c.txt', 'w').write('c\n')
    if not os.path.exists(WS + '/b.sch'): shutil.copy(EXAMPLE, WS + '/b.sch')
    for f in ('b.sch', 'net.cir', 'notes.txt'): s.call('open_document', {'path': f})
    s.call('get_state', {})
shutil.rmtree(WS, ignore_errors=True); os.makedirs(WS)
s = Server(); setup(s); t0 = time.time(); n = 0; deaths = []; counts = {}
while time.time() - t0 < budget:
    # (A dialog left open: closed, as the user would.)
    tool = random.choice(list(tools)); args = {}
    keys = list(tools[tool])
    for k in random.sample(keys, random.randint(0, len(keys))): args[k] = tools[tool][k]()
    if random.random() < 0.1: args[random.choice(['zz', 'max_chars'])] = junk()
    n += 1; counts[tool] = counts.get(tool, 0) + 1
    try:
        r = s.call(tool, args)
        if 'waits for an answer' in json.dumps(r) or 'is open in Qucs-S and waits' in json.dumps(r):
            s.call('set_dialog', {'press': 'Cancel'})
        if n % 50 == 0: setup(s)
    except Exception as e:
        deaths.append((tool, json.dumps(args, default=str)[:300], repr(e)[:80]))
        try: s.p.kill()
        except Exception: pass
        s = Server(); setup(s)
try: s.close()
except Exception: pass
print(n, 'calls,', len(deaths), 'server deaths'); print(json.dumps(counts))
for d in deaths[:10]: print(d)
