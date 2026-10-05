#!/usr/bin/env python3
"""End-to-end scenarios of Qucs-S's MCP tools, as an agent chains them.

Each scenario drives `qucs-s --mcp-server --workspace <root>/ws` through a
sequence of tools and checks the final netlist and measured numbers, not the
answers along the way. Written by the reviewer of the tools (rounds 4 to 8,
docs/feature_gaps/); s8 is round 8's: the 741 bench, from wiring by pin name
to a subcircuit, checked and simulated at each step.

    scripts/mcp-e2e-scenarios.py [s1 s2 ... s12]

QUCS names the qucs-s binary (the installed app by default). A packaged
app is tested with its own library (share/qucs-s/library beside its
binary); the build tree's has none, so QUCS_LIBRARY_DIR is set to the
source's - unless it is set already. Each run has a folder of its own under
the root (QUCS_E2E_ROOT, /tmp/e2e by default), <date>-<time>-<pid>, with
the server's workspace, settings and HOME in it: nothing is written to the
user's workspace or caches, and a run does not meet the files of the last
(save_document refused to write over them). The simulator is the first
ngspice on PATH. s7 copies a project (QUCS_E2E_PROJECT,
~/QucsWorkspace/project1_prj by default) and is skipped when there is none.
QUCS_E2E_DATASET_LIMIT_MB=<n> keeps every run's dataset of more than n MB
binary (0: every one) - binary datasets end to end. s12 needs OpenVAF
(QUCS_OPENVAF, else openvaf-r or openvaf on PATH) and is skipped without.
"""
import json, subprocess, os, sys, time, math, shutil, glob

APP = os.environ.get('QUCS', '/Applications/qucs-s.app/Contents/MacOS/qucs-s')
ROOT = os.path.join(os.environ.get('QUCS_E2E_ROOT', '/tmp/e2e'), time.strftime('%Y%m%d-%H%M%S') + '-%d' % os.getpid())
WS = ROOT + '/ws'; SETTINGS = ROOT + '/settings'; HOME = ROOT + '/home'

def own_library(app):
    """The library an installed qucs-s has beside its binary (a packaged app's
    Contents/MacOS/share/qucs-s/library, an installation's share/qucs-s/library),
    or None (the build tree's)."""
    here = os.path.dirname(os.path.realpath(app))
    for rel in ('share/qucs-s/library', '../share/qucs-s/library'):
        lib = os.path.normpath(os.path.join(here, rel))
        if os.path.isfile(os.path.join(lib, 'OpAmps.lib')): return lib
    return None

SOURCE_LIBRARY = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'qucs-s-26.1.1', 'library')
LIBRARY = os.environ.get('QUCS_LIBRARY_DIR') or (None if own_library(APP) else SOURCE_LIBRARY)
PROJECT = os.environ.get('QUCS_E2E_PROJECT', os.path.expanduser('~/QucsWorkspace/project1_prj'))
DATASET_LIMIT = os.environ.get('QUCS_E2E_DATASET_LIMIT_MB')
for d in (WS, SETTINGS, HOME):
    os.makedirs(d, exist_ok=True)

class Server:
    def __init__(self):
        env = dict(os.environ, QT_QPA_PLATFORM='offscreen', QUCS_SETTINGS_DIR=SETTINGS, QUCS_NO_SHELL_ENV='1', HOME=HOME)
        if LIBRARY and os.path.isfile(os.path.join(LIBRARY, 'OpAmps.lib')): env['QUCS_LIBRARY_DIR'] = os.path.abspath(LIBRARY)
        self.p = subprocess.Popen([APP, '--mcp-server', '--workspace', WS], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=open(ROOT + '/server.err', 'a'), env=env, text=True)
        self.n = 0
        self.rpc('initialize', {'protocolVersion': '2025-06-18', 'capabilities': {}})
        self.send({'jsonrpc': '2.0', 'method': 'notifications/initialized'})
        st = self.call('get_state', {})
        assert os.path.realpath(st['workspace']) == os.path.realpath(WS), st
        if DATASET_LIMIT is not None:
            self.call('set_settings', {'scope': 'simulators', 'values': {'Results/Keep large results binary': True,
                                                                         'Results/Binary above': int(DATASET_LIMIT)}})
    def send(self, o):
        self.p.stdin.write(json.dumps(o) + '\n'); self.p.stdin.flush()
    def rpc(self, method, params):
        self.n += 1; i = self.n
        self.send({'jsonrpc': '2.0', 'id': i, 'method': method, 'params': params})
        while True:
            line = self.p.stdout.readline()
            if not line: raise RuntimeError('server died')
            m = json.loads(line)
            if m.get('id') == i: return m
    def call(self, tool, args, ok=True):
        t0 = time.time()
        m = self.rpc('tools/call', {'name': tool, 'arguments': args})
        r = m.get('result', m)
        text = '\n'.join(c.get('text', '') for c in r.get('content', [])) if isinstance(r, dict) else str(r)
        err = isinstance(r, dict) and r.get('isError')
        LOG.append((tool, args, err, text, time.time() - t0))
        if err and ok: raise ToolError(f'{tool} {json.dumps(args)[:200]} -> {text[:600]}')
        try: return json.loads(text)
        except Exception: return text
    def close(self):
        self.p.stdin.close(); self.p.wait(timeout=30)

class ToolError(Exception): pass
LOG = []
RESULTS = []   # (scenario, check, ok, evidence)

def check(scn, name, cond, evidence=''):
    RESULTS.append((scn, name, bool(cond), str(evidence)[:400]))
    print(('  ok   ' if cond else '  FAIL ') + name + ('' if cond else '  <- ' + str(evidence)[:300]))

def near(a, b, tol): return abs(a - b) <= tol * abs(b)

def netlist_nodes(s, path):
    m = s.call('get_netlist', {'path': path, 'map': True})
    return {k: sorted(v) for k, v in m['nodes'].items()}, m['netlist']

def scenario(fn):
    def run():
        print('==', fn.__name__, fn.__doc__ or '')
        s = Server()
        try: fn(s)
        except Exception as e: check(fn.__name__, 'ran to the end', False, repr(e))
        finally:
            try: s.close()
            except Exception: pass
    return run

# ---------------------------------------------------------------------------
@scenario
def s1_import_arrange_ac_tune_export(s):
    """import a netlist, arrange, AC with a plot, simulate, measure the bandwidth, tune it, export"""
    r = s.call('import_netlist', {'text': 'lowpass\nV1 in 0 DC 0 AC 1\nR1 in out 1k\nC1 out 0 100n\n.ac dec 20 10 1meg\n.end',
                                  'save_as': WS + '/s1.sch'})
    before, _ = netlist_nodes(s, 's1.sch')
    a = s.call('arrange', {'path': 's1.sch', 'wire_labels': True})
    after, _ = netlist_nodes(s, 's1.sch')
    check('s1', 'arrange kept every net', before == after, (before, after))
    chk = s.call('check_schematic', {'path': 's1.sch'})
    check('s1', 'check_schematic clean after arrange', chk['errors'] == [] and chk['warnings'] == [], chk)
    # the imported .ac is there; add a plot of the gain in dB via add_analysis? The analysis exists: add a diagram instead
    d = s.call('add_diagram', {'path': 's1.sch', 'traces': [{'variable': 'ac.v(out)'}]})
    sim = s.call('simulate', {'path': 's1.sch'})
    check('s1', 'simulate succeeded', sim.get('succeeded'), sim.get('errors'))
    ds = s.call('get_dataset', {'path': 's1.sch', 'variables': ['ac.v(out)'], 'measure': ['bandwidth']})
    bw = ds['variables'][0]['measurements']['bandwidth']
    fc = 1 / (2 * math.pi * 1e3 * 100e-9)
    check('s1', 'bandwidth within 1%% of 1/(2piRC) = %.1f Hz' % fc, near(bw['value'], fc, 0.01), bw)
    check('s1', 'no stale note right after the run', 'stale' not in ds, ds.get('stale'))
    # tune C1 so the bandwidth is 1 kHz: C = 159.15 nF
    t = s.call('tune', {'path': 's1.sch', 'component': 'C1', 'measure': {'variable': 'ac.v(out)', 'what': 'bandwidth'},
                        'target': 1000, 'range': ['10n', '1u']})
    check('s1', 'tune reached the target', t.get('within tolerance'), t)
    check('s1', 'tune took few runs (<= 6)', len(t.get('runs', [])) <= 6, len(t.get('runs', [])))
    got = s.call('get_schematic', {'path': 's1.sch', 'components': ['C1']})
    cval = [p['value'] for p in got['components'][0]['properties'] if p['name'] == 'C'][0]
    check('s1', 'C1 carries the tuned value in the schematic', cval == t.get('value'), (cval, t.get('value')))
    ds2 = s.call('get_dataset', {'path': 's1.sch', 'variables': ['ac.v(out)'], 'measure': ['bandwidth']})
    bw2 = ds2['variables'][0]['measurements']['bandwidth']['value']
    check('s1', 'dataset after tune shows the new bandwidth (1 kHz +-1%)', near(bw2, 1000, 0.01), bw2)
    check('s1', 'dataset after tune is not stale', 'stale' not in ds2, ds2.get('stale'))
    e = s.call('export_netlist', {'path': 's1.sch', 'save_as': WS + '/s1.cir'})
    txt = open(WS + '/s1.cir').read()
    check('s1', 'exported netlist carries the tuned value', 'C1' in txt and t.get('value', '@@') .replace(' ', '')[:4].lower() in txt.replace(' ', '').lower(), txt)
    img = s.call('export_image', {'path': 's1.sch', 'save_as': WS + '/s1.png'})
    check('s1', 'export_image wrote a PNG', os.path.getsize(WS + '/s1.png') > 5000, img)
    s.call('save_document', {'path': 's1.sch'})
    # undo everything back to the import and compare with the file? Instead: diff against file says same
    df = s.call('diff', {'path': 's1.sch'})
    check('s1', 'diff against the saved file: same', df.get('same'), df)

# ---------------------------------------------------------------------------
def build_divider(s, path, cap=True):
    s.call('new_document', {})
    calls = [
        {'tool': 'add_component', 'arguments': {'type': 'Vdc', 'name': 'V1', 'x': 100, 'y': 200, 'properties': {'U': '5 V'}}},
        {'tool': 'add_component', 'arguments': {'type': 'R', 'name': 'R1', 'x': 220, 'y': 100, 'properties': {'R': '1k'}}},
        {'tool': 'add_component', 'arguments': {'type': 'R', 'name': 'R2', 'x': 340, 'y': 200, 'rotation': 1, 'properties': {'R': '1k'}}},
        {'tool': 'add_component', 'arguments': {'type': '.DC', 'name': 'DC1', 'x': 120, 'y': 400}},
        {'tool': 'connect', 'arguments': {'from': 'V1.1', 'to': 'R1.1'}},
        {'tool': 'connect', 'arguments': {'from': 'R1.2', 'to': 'R2.2'}},
        {'tool': 'connect', 'arguments': {'from': 'R2.1', 'to': 'ground'}},
        {'tool': 'connect', 'arguments': {'from': 'V1.2', 'to': 'ground'}},
        {'tool': 'set_label', 'arguments': {'at': 'R1.2', 'name': 'mid'}},
        {'tool': 'set_label', 'arguments': {'at': 'V1.1', 'name': 'vin'}},
    ]
    if cap:
        calls += [{'tool': 'add_component', 'arguments': {'type': 'C', 'name': 'C1', 'x': 460, 'y': 200, 'rotation': 1, 'properties': {'C': '1u'}}},
                  {'tool': 'connect', 'arguments': {'from': 'C1.2', 'to': 'R2.2'}},
                  {'tool': 'connect', 'arguments': {'from': 'C1.1', 'to': 'ground'}}]
    calls.append({'tool': 'save_document', 'arguments': {'as': path}})
    return s.call('batch', {'calls': calls, 'atomic': True})

@scenario
def s2_build_subcircuit_symbol_replace(s):
    """build a divider, simulate, turn it into a subcircuit, draw the symbol, re-simulate, replace a part"""
    build_divider(s, WS + '/s2.sch')
    sim = s.call('simulate', {'path': 's2.sch'})
    op = s.call('get_dataset', {'path': 's2.sch', 'operating_point': True})
    check('s2', 'v(mid) = 2.5 V before', near(op['operating point']['nodes']['v(mid)'], 2.5, 1e-3), op['operating point']['nodes'])
    before, _ = netlist_nodes(s, 's2.sch')
    sub = s.call('create_subcircuit', {'path': 's2.sch', 'names': ['R1', 'R2'], 'save_as': 'div.sch'})
    check('s2', 'two ports (vin, mid) and one ground inside', len(sub['ports']) == 2 and sub.get('grounds inside') == 1, sub)
    s.call('open_document', {'path': WS + '/div.sch'})
    s.call('make_symbol', {'path': 'div.sch'})
    sv = s.call('save_document', {'path': 'div.sch'})
    check('s2', 'save says every net is as it was', 'as it was' in str(sv) or 'where they were' in str(sv), sv)
    chk = s.call('check_schematic', {'path': 's2.sch'})
    check('s2', 'parent check clean', chk['errors'] == [] and chk['warnings'] == [], chk)
    nodes, nl = netlist_nodes(s, 's2.sch')
    x = [l for l in nl if l.startswith('XSUB1')]
    check('s2', 'instance on vin and mid', x and 'vin' in x[0] and 'mid' in x[0], x)
    sim = s.call('simulate', {'path': 's2.sch'})
    op = s.call('get_dataset', {'path': 's2.sch', 'operating_point': True})
    check('s2', 'v(mid) = 2.5 V after', sim.get('succeeded') and near(op['operating point']['nodes']['v(mid)'], 2.5, 1e-3), op.get('operating point', op))
    # replace the cap with an inductor: same nets
    rp = s.call('replace_component', {'path': 's2.sch', 'name': 'C1', 'type': 'L', 'properties': {'L': '1 mH'}})
    nodes2, nl2 = netlist_nodes(s, 's2.sch')
    l = [ln for ln in nl2 if ln.startswith('L')]
    check('s2', 'replaced part keeps its nets (mid, 0)', l and 'mid' in l[0].split() and '0' in l[0].split(), (l, rp))
    ds = s.call('get_dataset', {'path': 's2.sch', 'operating_point': True})
    check('s2', 'dataset stale after the replacement', 'stale' in ds, ds.get('stale'))
    u = s.call('undo', {'path': 's2.sch'})
    nodes3, _ = netlist_nodes(s, 's2.sch')
    check('s2', 'undo of replace restores the netlist nodes', nodes3 == nodes, (nodes3, nodes))
    s.call('save_document', {'path': 's2.sch'})

# ---------------------------------------------------------------------------
@scenario
def s3_parameter_sweep(s):
    """add a sweep of the DC analysis over R2, simulate, read the curve"""
    build_divider(s, WS + '/s3.sch', cap=False)
    sw = s.call('add_analysis', {'path': 's3.sch', 'kind': 'sweep', 'analysis': 'DC1', 'parameter': 'R2', 'from': 500, 'to': 2000, 'points': 4, 'plot': ['mid']})
    sim = s.call('simulate', {'path': 's3.sch'})
    check('s3', 'sweep simulated', sim.get('succeeded'), sim.get('errors'))
    ds = s.call('get_dataset', {'path': 's3.sch', 'variables': ['sw1.v(mid)'], 'at': [500, 1000, 1500, 2000]})
    names = [v['name'] for v in ds.get('variables', [])]
    v = [x for x in ds.get('variables', []) if 'mid' in x['name']]
    want = [5 * r / (1000 + r) for r in (500, 1000, 1500, 2000)]
    ys = [pt[1] for pt in v[0]['at']] if v and 'at' in v[0] else None
    ok = bool(ys) and len(ys) == 4 and all(near(a, b, 1e-3) for a, b in zip(ys, want))
    got = (ys, want)
    check('s3', 'v(mid) over R2 = 5*R2/(1k+R2) at 4 points', ok, (names, got))

# ---------------------------------------------------------------------------
@scenario
def s4_transient_marker_display_export(s):
    """RLC step: transient with a plot, measure, marker at the peak, a data display, export a picture"""
    s.call('new_document', {})
    calls = [
        {'tool': 'add_component', 'arguments': {'type': 'Vpulse', 'name': 'V1', 'x': 100, 'y': 200, 'properties': {'U1': '0 V', 'U2': '1 V', 'T1': '0', 'T2': '100 ms', 'Tr': '1 ns', 'Tf': '1 ns'}}},
        {'tool': 'add_component', 'arguments': {'type': 'R', 'name': 'R1', 'x': 220, 'y': 100, 'properties': {'R': '10'}}},
        {'tool': 'add_component', 'arguments': {'type': 'L', 'name': 'L1', 'x': 340, 'y': 100, 'properties': {'L': '1 mH'}}},
        {'tool': 'add_component', 'arguments': {'type': 'C', 'name': 'C1', 'x': 460, 'y': 200, 'rotation': 1, 'properties': {'C': '1 uF'}}},
        {'tool': 'connect', 'arguments': {'from': 'V1.1', 'to': 'R1.1'}}, {'tool': 'connect', 'arguments': {'from': 'R1.2', 'to': 'L1.1'}},
        {'tool': 'connect', 'arguments': {'from': 'L1.2', 'to': 'C1.2'}}, {'tool': 'connect', 'arguments': {'from': 'C1.1', 'to': 'ground'}},
        {'tool': 'connect', 'arguments': {'from': 'V1.2', 'to': 'ground'}}, {'tool': 'set_label', 'arguments': {'at': 'L1.2', 'name': 'out'}},
        {'tool': 'add_analysis', 'arguments': {'kind': 'tran', 'stop': '3 ms', 'points': 3001, 'plot': ['out']}},
        {'tool': 'save_document', 'arguments': {'as': WS + '/s4.sch'}}]
    s.call('batch', {'calls': calls, 'atomic': True})
    sim = s.call('simulate', {'path': 's4.sch'})
    ds = s.call('get_dataset', {'path': 's4.sch', 'variables': ['tran.v(out)'], 'measure': ['overshoot', 'frequency', 'settling_time']})
    m = ds['variables'][0]['measurements']
    zeta = 10 / 2 * math.sqrt(1e-6 / 1e-3); w0 = 1 / math.sqrt(1e-3 * 1e-6)
    os_theory = 100 * math.exp(-math.pi * zeta / math.sqrt(1 - zeta * zeta)); fd = w0 * math.sqrt(1 - zeta * zeta) / (2 * math.pi)
    check('s4', 'overshoot within 1%% of theory %.1f%%' % os_theory, near(m['overshoot']['value'], os_theory, 0.01), m['overshoot'])
    check('s4', 'damped frequency within 0.5%% of %.0f Hz' % fd, near(m['frequency']['value'], fd, 0.005), m['frequency'])
    mk = s.call('add_marker', {'path': 's4.sch', 'diagram': 1, 'trace': 1, 'at': 'peak'})
    mv = mk.get('value', mk.get('y', mk))
    check('s4', 'marker at the peak reads the overshoot peak', isinstance(mk, dict) and any(near(float(x), 1.6047, 0.01) for x in _numbers(mk)), mk)
    dd = s.call('new_document', {'kind': 'data_display', 'path': 's4.sch'})
    dg = s.call('add_diagram', {'path': 's4.dpl', 'traces': ['tran.v(out)'], 'title': 'RLC step'})
    check('s4', 'data display diagram shows the trace', dg['traces'][0].get('points', 0) > 100, dg['traces'][0])
    img = s.call('export_image', {'path': 's4.dpl', 'save_as': WS + '/s4.png'})
    check('s4', 'data display exported as PNG', os.path.getsize(WS + '/s4.png') > 5000, img)
    s.call('save_document', {'path': 's4.dpl'}); s.call('save_document', {'path': 's4.sch'})
    # reopen from disk: the marker and the display are still there
    s.call('close_document', {'path': 's4.dpl'}); s.call('close_document', {'path': 's4.sch'})
    s.call('open_document', {'path': WS + '/s4.sch'})
    sch = s.call('get_schematic', {'path': 's4.sch'})
    mks = sch['diagrams'][0].get('markers', sch['diagrams'][0].get('traces', [{}])[0].get('markers', []))
    check('s4', 'marker survives save and reload', bool(mks), sch['diagrams'][0])

def _numbers(o):
    out = []
    if isinstance(o, dict):
        for v in o.values(): out += _numbers(v)
    elif isinstance(o, list):
        for v in o: out += _numbers(v)
    elif isinstance(o, (int, float)): out.append(o)
    return out

# ---------------------------------------------------------------------------
@scenario
def s5_copy_rename_lifecycle(s):
    """copy a simulated schematic, run the copy, rename a net, and see traces, netlist and dataset follow"""
    s.call('open_document', {'path': WS + '/s4.sch'})
    cp = s.call('copy_document', {'path': 's4.sch', 'to': 's5'})
    check('s5', 'copy wrote schematic and dataset', 's5.sch' in str(cp) and 's5.dat.ngspice' in str(cp), cp)
    s.call('open_document', {'path': WS + '/s5.sch'})
    ds = s.call('get_dataset', {'path': 's5.sch', 'variables': ['tran.v(out)'], 'points': 0})
    check('s5', 'copy reads its own (copied) dataset', ds['dataset'].endswith('s5.dat.ngspice'), ds['dataset'])
    df = s.call('diff', {'path': 's5.sch', 'against': 's4.sch'})
    check('s5', 'diff copy vs original: same circuit', df.get('same') or df.get('changes') == [], df)
    rn = s.call('rename_net', {'path': 's5.sch', 'from': 'out', 'to': 'vout'})
    nodes, nl = netlist_nodes(s, 's5.sch')
    check('s5', 'netlist uses vout', 'vout' in nodes and 'out' not in nodes, list(nodes))
    sch = s.call('get_schematic', {'path': 's5.sch'})
    tv = sch['diagrams'][0]['traces'][0]['variable']
    check('s5', 'trace renamed to v(vout)', 'vout' in tv, tv)
    sim = s.call('simulate', {'path': 's5.sch'})
    ds = s.call('get_dataset', {'path': 's5.sch', 'variables': ['tran.v(vout)'], 'measure': ['frequency']})
    check('s5', 'renamed node measured after the run', near(ds['variables'][0]['measurements']['frequency']['value'], 4970, 0.005), ds['variables'][0].get('measurements'))
    check('s5', 'not stale after the run', 'stale' not in ds, ds.get('stale'))
    s.call('save_document', {'path': 's5.sch'})
    s.call('close_document', {'path': 's5.sch'})
    s.call('open_document', {'path': WS + '/s5.sch'})
    ds = s.call('get_dataset', {'path': 's5.sch', 'variables': ['tran.v(vout)'], 'points': 0})
    check('s5', 'after close and reopen, the dataset is not called stale', 'stale' not in ds, ds.get('stale'))
    lst = s.call('list_documents', {'kind': 'dataset'})
    own = [f for f in lst.get('files', []) if 's5.dat' in f.get('path', '')]
    check('s5', 'list_documents ties s5.dat to s5.sch', own and 's5.sch' in json.dumps(own[0]), own)

# ---------------------------------------------------------------------------
@scenario
def s6_error_recovery(s):
    """failures: atomic batch, preview, a bad value, a failed run - and what state the document is in after each"""
    build_divider(s, WS + '/s6.sch')
    r = s.call('batch', {'atomic': True, 'calls': [
        {'tool': 'add_component', 'arguments': {'type': 'R', 'name': 'R9', 'x': 600, 'y': 100}},
        {'tool': 'edit_component', 'arguments': {'name': 'R77', 'properties': {'R': '1'}}}]}, ok=False)
    df = s.call('diff', {'path': 's6.sch'})
    check('s6', 'atomic batch that fails leaves the file state (diff: same)', df.get('same'), (r if isinstance(r, str) else json.dumps(r)[:300], df))
    pv = s.call('create_subcircuit', {'path': 's6.sch', 'names': ['R1', 'R2'], 'save_as': 'pv.sch', 'preview': True})
    check('s6', 'create_subcircuit preview writes no file', not os.path.exists(WS + '/pv.sch'), pv.get('files it would write', pv))
    df = s.call('diff', {'path': 's6.sch'})
    check('s6', 'preview left the schematic unchanged', df.get('same'), df)
    s.call('edit_component', {'path': 's6.sch', 'name': 'R1', 'properties': {'R': 'kk'}}, ok=False)
    e = s.call('edit_component', {'path': 's6.sch', 'name': 'R1', 'properties': {'R': 'rload'}})
    check('s6', 'a bare name is applied with a note', any('rload' in str(v) for v in e.get('values', [])), e.get('values'))
    sim = s.call('simulate', {'path': 's6.sch'})
    first = (sim.get('errors') or [{}])[0]
    check('s6', 'failed run names the part first', not sim.get('succeeded') and first.get('component') == 'R1', sim.get('errors'))
    ds = s.call('get_dataset', {'path': 's6.sch', 'operating_point': True}, ok=False)
    check('s6', 'no dataset yet is said (no run succeeded)', isinstance(ds, str) or 'stale' in ds or 'no dataset' in json.dumps(ds).lower(), str(ds)[:200])
    s.call('edit_component', {'path': 's6.sch', 'name': 'R1', 'properties': {'R': '1k'}})
    sim = s.call('simulate', {'path': 's6.sch'})
    ds = s.call('get_dataset', {'path': 's6.sch', 'operating_point': True})
    check('s6', 'after the fix: v(mid) = 2.5 and not stale', sim.get('succeeded') and near(ds['operating point']['nodes']['v(mid)'], 2.5, 1e-3) and 'stale' not in ds, ds.get('stale', ds.get('operating point', {}).get('nodes')))
    t = s.call('tune', {'path': 's6.sch', 'component': 'R2', 'measure': {'operating_point': 'mid'}, 'target': 4.9, 'range': [100, 1000]})
    check('s6', 'tune that cannot reach 4.9 V with R2 <= 1k says so and leaves R2', not t.get('within tolerance') and 'as it was' in str(t.get('set', '')), t)
    hist = s.call('undo_history', {'path': 's6.sch', 'steps': 3})
    check('s6', 'the missed tune added no undo step of its own', all('R2' not in st['change'] for st in hist['steps']), hist['steps'])

# ---------------------------------------------------------------------------
@scenario
def s7_real_project(s):
    """the user's project1 (copied): open each schematic, check, netlist, arrange preview, simulate"""
    src = PROJECT; dst = WS + '/p1_prj'
    if not os.path.isdir(src) and not os.path.isdir(dst):
        print('  (skipped: no project at %s - QUCS_E2E_PROJECT names one)' % src)
        return
    if not os.path.isdir(dst):
        os.makedirs(dst)
        for f in os.listdir(src):
            if f.endswith(('.sch', '.va', '.osdi', '.cir', '.lib', '.sp', '.dpl', '.sym', '.txt')):
                shutil.copy(os.path.join(src, f), dst)
        if os.path.isdir(src + '/va'): shutil.copytree(src + '/va', dst + '/va')
    s.call('open_project', {'name': 'p1'})
    files = ['rc_lowpass.sch', 'diffpair.sch', 'emitter_follower.sch', 'opamp_inverter_ideal.sch', 'opamp_inverter.sch', 'jfet_pushpull.sch',
             'feedback_loop.sch', 'lc_lowpass_sp.sch', 'audio_power_amp.sch', 'gray_meyer_ec_pair_widlar.sch', 'lm386_amp.sch', 'hs_driver_test.sch', 'pd_testbench.sch']
    rows = []
    for f in files:
        if not os.path.exists(dst + '/' + f): continue
        row = {'file': f}
        try:
            s.call('open_document', {'path': dst + '/' + f})
            ov = s.call('get_schematic', {'path': f, 'format': 'overview'})
            row['parts'] = ov.get('components')
            chk = s.call('check_schematic', {'path': f})
            row['check'] = chk.get('found')
            before, _ = netlist_nodes(s, f)
            t0 = time.time(); ar = s.call('arrange', {'path': f, 'preview': True}, ok=False); row['arrange s'] = round(time.time() - t0, 2)
            ans = ar.get('its answer', ar) if isinstance(ar, dict) else ar
            row['arrange'] = (ans.get('arranged', ans) if isinstance(ans, dict) else str(ans))[:90]
            t0 = time.time(); sim = s.call('simulate', {'path': f, 'timeout': 90}, ok=False); row['sim s'] = round(time.time() - t0, 1)
            if isinstance(sim, dict):
                row['sim'] = 'ok' if sim.get('succeeded') else 'FAILED: ' + '; '.join(e.get('message', '')[:80] for e in sim.get('errors', [])[:2])
                row['vars'] = len(sim.get('variables', []))
            else: row['sim'] = str(sim)[:120]
            after, _ = netlist_nodes(s, f)
            row['nets kept'] = before == after
            s.call('close_document', {'path': f, 'unsaved': 'discard'})
        except Exception as e:
            row['error'] = repr(e)[:200]
        rows.append(row); print('  ', row)
    json.dump(rows, open(ROOT + '/s7.json', 'w'), indent=1)
    check('s7', 'every schematic opened, checked and netlisted', all('error' not in r for r in rows), [r for r in rows if 'error' in r])

# ---------------------------------------------------------------------------
def _stats(ds, name):
    """A variable's statistics in a get_dataset answer: {min, max, ...}."""
    for v in ds.get('variables', []):
        if v.get('name') == name: return v.get('statistics', v)
    return {}

@scenario
def s8_741_bench(s):
    """the 741 bench: described, wired by pin name, checked, arranged with its feedback below, simulated, kept and
    compared, made a subcircuit with a symbol, checked with its subcircuits, arranged and simulated again"""
    d = s.call('describe_part', {'library': 'OpAmps', 'part': 'ua741(TI)'})
    names = [p['name'] for p in d['pins']]
    check('s8', 'describe_part: pins by name, the bench with its expected values',
          names == ['INN', 'INP', 'OUT', 'VCC', 'VEE'] and 'its bench passes' in d['ngspice'] and '(expected 5.5 V)' in d['ngspice'],
          (names, d.get('ngspice')))
    s.call('new_document', {})
    add = lambda **a: {'tool': 'add_component', 'arguments': a}
    con = lambda a, b: {'tool': 'connect', 'arguments': {'from': a, 'to': b}}
    calls = [add(**dict(d['place'], name='U1', x=400, y=300)),
             add(type='Vac', name='Vin', x=150, y=350, properties={'U': '0.1 V', 'f': '1 kHz'}),
             add(type='R', name='Rg', x=300, y=450, rotation=1, properties={'R': '1k'}),
             add(type='R', name='Rf', x=450, y=150, properties={'R': '10k'}),
             add(type='R', name='RL', x=600, y=400, rotation=1, properties={'R': '10k'}),
             add(type='Vdc', name='VCC', x=50, y=150, properties={'U': '15 V'}),
             add(type='Vdc', name='VEE', x=50, y=500, properties={'U': '15 V'}),
             con('Vin.1', 'U1.INP'), con('Vin.2', 'ground'), con('Rg.2', 'U1.inn'), con('Rg.1', 'ground'),
             con('Rf.1', 'U1.inn'), con('Rf.2', 'U1.out'), con('RL.1', 'U1.out'), con('RL.2', 'ground'),
             con('VCC.1', 'U1.vcc'), con('VCC.2', 'ground'), con('VEE.1', 'ground'), con('VEE.2', 'U1.vee'),
             {'tool': 'set_label', 'arguments': {'at': 'U1.out', 'name': 'out'}},
             {'tool': 'add_analysis', 'arguments': {'kind': 'tran', 'stop': '3 ms', 'points': 3001, 'plot': ['out']}},
             {'tool': 'save_document', 'arguments': {'as': WS + '/s8.sch'}}]
    s.call('batch', {'calls': calls, 'atomic': True, 'brief': True})
    chk = s.call('check_schematic', {'path': 's8.sch'})
    check('s8', 'check: nothing wrong, and no note on the negative supply', chk['errors'] == [] and chk['warnings'] == []
          and not any('negative supply' in n['message'] for n in chk.get('notes', [])), chk)
    # A supply the wrong way round (VCC at -15 V): a warning, until undone.
    s.call('edit_component', {'path': 's8.sch', 'name': 'VCC', 'properties': {'U': '-15 V'}})
    chk = s.call('check_schematic', {'path': 's8.sch'})
    check('s8', 'VCC at -15 V: warned, its pin named', any('wrong way round' in w['message'] and 'U1.4 (VCC)' in w['message'] for w in chk['warnings']), chk['warnings'])
    s.call('undo', {'path': 's8.sch'})
    before, _ = netlist_nodes(s, 's8.sch')
    a = s.call('arrange', {'path': 's8.sch', 'feedback': 'below', 'straighten': True})
    after, _ = netlist_nodes(s, 's8.sch')
    cols = a.get('columns', [])
    check('s8', 'arrange: every net kept, Rg in U1\'s column with Rf (not the load\'s)', before == after
          and any('U1' in c and 'Rf' in c and 'Rg' in c for c in cols) and not any('RL' in c and 'Rg' in c for c in cols), cols)
    sim = s.call('simulate', {'path': 's8.sch', 'keep_as': 'gain11', 'brief': True})
    ds = s.call('get_dataset', {'path': 's8.sch', 'variables': ['tran.v(out)']})
    peak = _stats(ds, 'tran.v(out)').get('max')
    check('s8', 'simulated: the output peaks at 1.1 V (a gain of 11 of 0.1 V) within 2 %', sim.get('succeeded') and peak and near(peak, 1.1, 0.02), (sim.get('errors'), _stats(ds, 'tran.v(out)')))
    s.call('edit_component', {'path': 's8.sch', 'name': 'Rf', 'properties': {'R': '20k'}})
    sim = s.call('simulate', {'path': 's8.sch', 'brief': True})
    ds = s.call('get_dataset', {'path': 's8.sch', 'variables': ['tran.v(out)'], 'compare': 'gain11'})
    peak = _stats(ds, 'tran.v(out)').get('max')
    check('s8', 'Rf 20k: 2.1 V, compared with the run kept', sim.get('succeeded') and peak and near(peak, 2.1, 0.02) and 'gain11' in json.dumps(ds), json.dumps(ds)[:600])
    sub = s.call('create_subcircuit', {'path': 's8.sch', 'names': ['U1'], 'save_as': 'u741.sch', 'name': 'U1'})
    ports = sorted(p['net'] for p in sub['ports'])
    check('s8', 'create_subcircuit: the ports named after the 741\'s pins', ports == sorted(['INN', 'INP', 'out', 'VCC', 'VEE']), sub['ports'])
    s.call('open_document', {'path': WS + '/u741.sch'})
    ms = s.call('make_symbol', {'path': 'u741.sch'})
    sides = {p.split(':')[0]: p.split(': ')[1].split(',')[0] for p in ms['pins']}
    check('s8', 'make_symbol: inputs left, out right, VCC top, VEE bottom',
          sides == {'INN': 'left', 'INP': 'left', 'out': 'right', 'VCC': 'top', 'VEE': 'bottom'}, ms['pins'])
    s.call('save_document', {'path': 'u741.sch'})
    s.call('show_document', {'path': 's8.sch'})
    chk = s.call('check_schematic', {'path': 's8.sch', 'subcircuits': True})
    check('s8', 'check with its subcircuits: nothing wrong in either', chk['errors'] == [] and chk['warnings'] == []
          and 'in its subcircuits 0 errors, 0 warnings' in chk.get('found', ''), chk.get('found'))
    before, _ = netlist_nodes(s, 's8.sch')
    a = s.call('arrange', {'path': 's8.sch', 'feedback': 'below', 'wire_labels': True})
    after, _ = netlist_nodes(s, 's8.sch')
    cols = a.get('columns', [])
    check('s8', 'arranged again: nets kept, Rg with the subcircuit and Rf', before == after
          and any('U1' in c and 'Rf' in c and 'Rg' in c for c in cols), cols)
    sim = s.call('simulate', {'path': 's8.sch', 'brief': True})
    ds = s.call('get_dataset', {'path': 's8.sch', 'variables': ['tran.v(out)']})
    peak = _stats(ds, 'tran.v(out)').get('max')
    check('s8', 'the subcircuit simulates as the part did (2.1 V)', sim.get('succeeded') and peak and near(peak, 2.1, 0.02), (sim.get('errors'), _stats(ds, 'tran.v(out)')))
    s.call('save_document', {'path': 's8.sch'})

@scenario
def s9_dialogs(s):
    """the window side: Document Settings and Find and Replace opened by their menu actions, read, answered - Cancel
    changes nothing, OK does - a tool refused while a dialog waits, a search's rows checked and unchecked, the
    schematic, its netlist and a simulation checked after each"""
    s.call('import_netlist', {'text': 'lowpass\nV1 in 0 DC 0 AC 1\nR1 in out 1k\nC1 out 0 100n\n.ac dec 20 10 1meg\n.end',
                              'save_as': WS + '/s9.sch'})
    settings = lambda: s.call('get_schematic', {'path': 's9.sch'})['settings']
    rvalue = lambda: [p['value'] for c in s.call('get_schematic', {'path': 's9.sch', 'components': ['R1']})['components']
                      for p in c['properties'] if p['name'] == 'R'][0]
    s.call('trigger_action', {'action': 'File > Document Settings...', 'path': 's9.sch'})
    d = s.call('get_dialog', {})
    field = {c['label']: c for c in d['controls']}
    check('s9', 'Document Settings: read, its Data Set field the schematic\'s', d['title'] == 'Edit File Properties'
          and field.get('Data Set', {}).get('value') == 's9.dat', [(c['label'], c.get('value')) for c in d['controls']][:14])
    parts = len(s.call('get_schematic', {'path': 's9.sch'})['components'])
    refused = s.call('add_component', {'path': 's9.sch', 'type': 'R', 'x': 600, 'y': 600}, ok=False)
    check('s9', 'while it waits: another tool refused, the schematic as it was', isinstance(refused, str) and 'waits for an answer' in refused
          and len(s.call('get_schematic', {'path': 's9.sch'})['components']) == parts, refused)
    s.call('set_dialog', {'set': [{'control': 'Data Set', 'value': 's9_zzz.dat'}], 'press': 'Cancel'})
    check('s9', 'Cancel: the dataset setting unchanged', settings().get('dataset') == 's9.dat', settings())
    s.call('trigger_action', {'action': 'File > Document Settings...', 'path': 's9.sch'})
    s.call('set_dialog', {'set': [{'control': 'Data Set', 'value': 's9_run.dat'}], 'press': 'OK'})
    check('s9', 'OK: the dataset is s9_run.dat', settings().get('dataset') == 's9_run.dat', settings())
    sim = s.call('simulate', {'path': 's9.sch', 'brief': True})
    fc = 1 / (2 * math.pi * 1e3 * 100e-9)
    bw = lambda: s.call('get_dataset', {'path': 's9.sch', 'variables': ['ac.v(out)'], 'measure': ['bandwidth']})
    ds = bw()
    check('s9', 'simulated into s9_run.dat.ngspice: the bandwidth 1/(2 pi RC) within 1 %', sim.get('succeeded')
          and os.path.isfile(WS + '/s9_run.dat.ngspice') and ds['dataset'].endswith('s9_run.dat.ngspice')
          and near(ds['variables'][0]['measurements']['bandwidth']['value'], fc, 0.01), (sim.get('errors'), ds.get('dataset')))
    # Find and Replace: its results a tree of rows, each checked or not.
    s.call('trigger_action', {'action': 'Edit > Replace...', 'path': 's9.sch'})
    s.call('set_dialog', {'set': [{'control': 'Find', 'value': '1k'}, {'control': 'Replace with', 'value': '2k'},
                                  {'control': 'Component type', 'value': 'R_SPICE'}], 'press': 'Find'})
    tree = [c for c in s.call('get_dialog', {})['controls'] if c['kind'] == 'tree']
    check('s9', 'Find: R1 found, a row of the tree, checked', len(tree) == 1 and tree[0]['rows'] == [['s9.sch', 'R1', 'R_SPICE', 'R', '1k', '2k']]
          and tree[0].get('checked') == [True], tree)
    s.call('set_dialog', {'set': [{'control': tree[0]['id'], 'value': [0, 0, False]}], 'press': 'Replace Checked'})
    check('s9', 'the row unchecked: Replace Checked replaces nothing', rvalue() == '1k', rvalue())
    s.call('set_dialog', {'set': [{'control': tree[0]['id'], 'value': [0, 0, True]}], 'press': 'Replace Checked'})
    s.call('set_dialog', {'press': 'Close'})
    _, net = netlist_nodes(s, 's9.sch')
    net = '\n'.join(net) if isinstance(net, list) else net
    check('s9', 'checked: R1 is 2k, in the netlist too', rvalue() == '2k' and any(l.startswith('R1 ') and '2K' in l.upper() for l in net.splitlines()),
          [l for l in net.splitlines() if l.startswith('R1')])
    ds = bw()
    check('s9', 'the dataset (of 1k) is stale, and certainly', ds.get('stale certain') is True, {k: ds.get(k) for k in ('stale', 'stale certain')})
    s.call('simulate', {'path': 's9.sch', 'brief': True})
    ds = bw()
    check('s9', 'simulated again: half the bandwidth, and not stale', near(ds['variables'][0]['measurements']['bandwidth']['value'], fc / 2, 0.01)
          and 'stale' not in ds, (ds['variables'][0]['measurements']['bandwidth'].get('value'), ds.get('stale')))
    s.call('undo', {'path': 's9.sch'})
    check('s9', 'undo takes the replace back: R1 is 1k', rvalue() == '1k', rvalue())
    s.call('save_document', {'path': 's9.sch'})

# ---------------------------------------------------------------------------
@scenario
def s10_csv_plot(s):
    """a data file plotted with no circuit and no simulation (qucs-s-csv-import-mcp-fix): a CSV imported beside a
    saved schematic, its three variables in a diagram with their points and no simulator's prefix; a trace added
    before the import told how to read it; the file changed and read again, the diagram with it"""
    s.call('new_document', {})
    s.call('save_document', {'as': WS + '/s10.sch'})
    rows = lambda n: 'time,v1,v2,v3\n' + ''.join('%g,%g,%g,%g\n' % (i * 1e-5, math.sin(i / 8), 0.8 * math.cos(i / 8), math.exp(-i / 30))
                                                  for i in range(n))
    open(WS + '/dummy_data.csv', 'w').write(rows(101))
    early = s.call('add_diagram', {'path': 's10.sch', 'traces': ['dummy_data:v1']})
    imp = s.call('import_data', {'path': 's10.sch', 'file': 'dummy_data.csv'})
    check('s10', 'imported: dummy_data, x time, three traces name:variable', imp['dataset'] == 'dummy_data' and imp['x'] == 'time'
          and imp['traces'] == ['dummy_data:v1', 'dummy_data:v2', 'dummy_data:v3'] and os.path.isfile(WS + '/dummy_data.dat'), imp)
    prefixed = (imp.get('diagrams') or [{}])[0].get("with a simulator's prefix", [])
    check('s10', 'the trace added before it: its prefix said, and the name that reads it', early['traces'][0]['variable'] == 'ngspice/dummy_data:v1'
          and prefixed and prefixed[0]['to read it'] == 'dummy_data:v1', imp.get('diagrams'))
    d = s.call('add_diagram', {'path': 's10.sch', 'traces': imp['traces']})
    check('s10', 'plotted: each trace without a prefix, 101 points', [t['variable'] for t in d['traces']] == imp['traces']
          and all(t.get('points') == 101 for t in d['traces']), d['traces'])
    fixed = s.call('edit_trace', {'path': 's10.sch', 'diagram': 1, 'trace': 1, 'variable': 'dummy_data:v1'})
    check('s10', 'edit_trace takes the early trace\'s prefix off', fixed['variable'] == 'dummy_data:v1' and fixed.get('points') == 101, fixed)
    ds = s.call('get_dataset', {'path': 'dummy_data.dat'})
    check('s10', 'get_dataset: imported from the CSV, each variable with its trace', ds.get('imported from', {}).get('file', '').endswith('dummy_data.csv')
          and [v.get('trace') for v in ds['variables']] == imp['traces'], (ds.get('imported from'), [v.get('trace') for v in ds['variables']]))
    open(WS + '/dummy_data.csv', 'w').write(rows(201))
    again = s.call('import_data', {'path': 's10.sch', 'name': 'dummy_data', 'reload': True})
    points = [t.get('points') for t in s.call('get_schematic', {'path': 's10.sch'})['diagrams'][1]['traces']]
    check('s10', 'the file changed: read again, and the diagram shows its 201 points', again.get('read again') and points == [201, 201, 201], points)
    s.call('save_document', {'path': 's10.sch'})

# ---------------------------------------------------------------------------
def _pin(described, wanted, fallback):
    """The number of a library part's pin by its name, as describe_part gives the pins."""
    for i, p in enumerate(described.get('pins', []), 1):
        if str(p.get('name', '')).lower() == wanted: return str(p.get('pin', i))
    return fallback

@scenario
def s11_libraries(s):
    """libraries by the tools: a subcircuit made a library (create_library), listed and described, its part placed and
    simulated; the library taken away and brought back (import_library) into a folder of the library search paths, the
    placed part found there by its name and simulated again; a SPICE library of subcircuits brought in and its part
    simulated"""
    s.call('new_project', {'name': 's11'})
    proj = WS + '/s11_prj'
    build_divider(s, proj + '/s11.sch')   # (the capacitor outside: mid a port too)
    s.call('create_subcircuit', {'path': 's11.sch', 'names': ['R1', 'R2'], 'save_as': 'div.sch'})
    s.call('open_document', {'path': 'div.sch'})
    s.call('make_symbol', {'path': 'div.sch'})
    s.call('save_document', {'path': 'div.sch'})
    made = s.call('create_library', {'name': 'S11Divs', 'subcircuits': ['div'], 'descriptions': {'div': 'A divider by two'}})
    check('s11', 'create_library: S11Divs.lib in user_lib, its part div with its description',
          os.path.isfile(WS + '/user_lib/S11Divs.lib') and [p['part'] for p in made['parts']] == ['div']
          and made['parts'][0].get('description') == 'A divider by two', made)
    listed = s.call('list_libraries', {'library': 'S11Divs'})
    described = s.call('describe_part', {'library': 'S11Divs', 'part': 'div'})
    check('s11', 'list_libraries and describe_part know it: two pins', listed['section'] == 'user' and len(described.get('pins', [])) == 2,
          (listed.get('section'), described.get('pins')))
    vin, mid = _pin(described, 'vin', '1'), _pin(described, 'mid', '2')
    place = made['parts'][0]['place']
    s.call('new_document', {})
    s.call('batch', {'calls': [
        {'tool': 'add_component', 'arguments': {'type': 'Vdc', 'name': 'V1', 'x': 100, 'y': 200, 'properties': {'U': '5 V'}}},
        {'tool': 'add_component', 'arguments': {'type': place['type'], 'name': 'X1', 'x': 300, 'y': 200, 'properties': place['properties']}},
        {'tool': 'add_component', 'arguments': {'type': '.DC', 'name': 'DC1', 'x': 120, 'y': 400}},
        {'tool': 'connect', 'arguments': {'from': 'V1.1', 'to': 'X1.' + vin}},
        {'tool': 'connect', 'arguments': {'from': 'V1.2', 'to': 'ground'}},
        {'tool': 'set_label', 'arguments': {'at': 'X1.' + mid, 'name': 'mid'}},
        {'tool': 'save_document', 'arguments': {'as': proj + '/s11_top.sch'}}], 'atomic': True})
    measure = lambda: s.call('get_dataset', {'path': 's11_top.sch', 'operating_point': True})['operating point']['nodes'].get('v(mid)')
    sim = s.call('simulate', {'path': 's11_top.sch', 'brief': True})
    check('s11', 'its part simulated: v(mid) = 2.5 V', sim.get('succeeded') and near(measure() or 0, 2.5, 1e-3), sim.get('errors'))
    # Taken away, and brought back elsewhere: a folder of the search paths.
    os.makedirs(ROOT + '/elsewhere', exist_ok=True)
    os.makedirs(ROOT + '/teamlibs', exist_ok=True)
    shutil.copy(WS + '/user_lib/S11Divs.lib', ROOT + '/elsewhere/S11Divs.lib')
    s.call('trash_file', {'path': WS + '/user_lib/S11Divs.lib'})
    s.call('set_settings', {'scope': 'app', 'values': {'Locations/Library search paths': [ROOT + '/teamlibs']}})
    imp = s.call('import_library', {'path': ROOT + '/elsewhere/S11Divs.lib', 'destination': ROOT + '/teamlibs'})
    sections = [x['section'] for x in s.call('list_libraries', {})['sections']]
    check('s11', 'import_library: in the search path folder, a section of its own', os.path.isfile(ROOT + '/teamlibs/S11Divs.lib')
          and imp['kind'] == 'qucs' and 'search path' in sections, (imp, sections))
    s.call('close_document', {'path': 's11_top.sch'})
    s.call('open_document', {'path': 's11_top.sch'})
    sim = s.call('simulate', {'path': 's11_top.sch', 'brief': True})
    check('s11', 'the placed part found there by its name: v(mid) = 2.5 V again', sim.get('succeeded') and near(measure() or 0, 2.5, 1e-3),
          sim.get('errors'))
    # A SPICE library of subcircuits.
    open(ROOT + '/elsewhere/Halves.lib', 'w').write('* halves\n.subckt half in out\nR1 in out 1k\nR2 out 0 1k\n.ends\n')
    spice = s.call('import_library', {'path': ROOT + '/elsewhere/Halves.lib'})
    sp = spice['parts'][0]['place']
    s.call('new_document', {})
    s.call('batch', {'calls': [
        {'tool': 'add_component', 'arguments': {'type': 'Vdc', 'name': 'V1', 'x': 100, 'y': 200, 'properties': {'U': '4 V'}}},
        {'tool': 'add_component', 'arguments': {'type': sp['type'], 'name': 'H1', 'x': 300, 'y': 200, 'properties': sp['properties']}},
        {'tool': 'add_component', 'arguments': {'type': '.DC', 'name': 'DC1', 'x': 120, 'y': 400}},
        {'tool': 'connect', 'arguments': {'from': 'V1.1', 'to': 'H1.1'}},
        {'tool': 'connect', 'arguments': {'from': 'V1.2', 'to': 'ground'}},
        {'tool': 'set_label', 'arguments': {'at': 'H1.2', 'name': 'o'}},
        {'tool': 'save_document', 'arguments': {'as': proj + '/s11_spice.sch'}}], 'atomic': True})
    sim = s.call('simulate', {'path': 's11_spice.sch', 'brief': True})
    op = s.call('get_dataset', {'path': 's11_spice.sch', 'operating_point': True})['operating point']['nodes']
    check('s11', 'a SPICE library brought in (spice, in user_lib), its subcircuit simulated: v(o) = 2 V', spice['kind'] == 'spice'
          and os.path.isfile(WS + '/user_lib/Halves.lib') and sim.get('succeeded') and near(op.get('v(o)', 0), 2.0, 1e-3), (spice.get('kind'), op))

@scenario
def s12_verilog_a_library_read_only(s):
    """a library whose subcircuit has a Verilog-A model, made into a folder of the library search paths, the folder
    read-only (a team's share): build_verilog_a compiles it into the cache; its part, placed in a project's schematic
    by its library's name and saved, has the source linked into the project (Libraries/VaRes/vres.va, a relative link),
    compiled there before the simulation - its `include of a file of the library's folder found - and simulated with it,
    nothing written into the library's folder; a library of the name made in the project, without the part, does not take
    it (reopened, it still simulates); the part taken away and saved, the link, its model and Libraries/ go. Needs OpenVAF (QUCS_OPENVAF, else openvaf-r or openvaf on PATH): skipped without"""
    openvaf = os.environ.get('QUCS_OPENVAF') or shutil.which('openvaf-r') or shutil.which('openvaf')
    if not openvaf:
        print('  skipped: no OpenVAF'); return
    team = ROOT + '/teamva'
    os.makedirs(team, exist_ok=True)
    s.call('set_settings', {'scope': 'app', 'values': {'Locations/OpenVAF Path': openvaf, 'Locations/Library search paths': [team]}})
    s.call('new_project', {'name': 's12'})
    proj = WS + '/s12_prj'
    open(proj + '/vres_params.vams', 'w').write('`define VRES_DEFAULT 1k\n')
    open(proj + '/vres.va', 'w').write('`include "disciplines.vams"\n`include "vres_params.vams"\nmodule vres(p, n);\n  inout p, n;\n'
                                       '  electrical p, n;\n  parameter real r = `VRES_DEFAULT from (0:inf);\n  analog I(p, n) <+ V(p, n) / r;\nendmodule\n')
    # The subcircuit: the Verilog-A resistor, 2k, between its two ports (nodes a and b).
    open(proj + '/vres.sch', 'w').write(
        '<Qucs Schematic 26.1.5>\n<Components>\n'
        '  <Port P1 1 220 100 -23 12 0 0 "1" 1 "analog" 0>\n  <Port P2 1 280 100 4 12 1 2 "2" 1 "analog" 0>\n'
        '  <SpiceModel SpiceModel1 1 120 300 -27 16 0 0 ".model m1 vres r=2k" 1 "N1 a b m1" 0 "" 0 "" 0 "" 0>\n'
        '</Components>\n<Wires>\n  <220 100 220 100 "a" 250 70 0 "">\n  <280 100 280 100 "b" 310 70 0 "">\n</Wires>\n'
        '<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n')
    s.call('open_project', {'name': 's12'})
    made = s.call('create_library', {'name': 'VaRes', 'subcircuits': ['vres'], 'destination': team})
    lib = team + '/VaRes'
    check('s12', 'create_library: VaRes in the search path folder, the Verilog-A source and its include in its folder',
          os.path.isfile(lib + '/vres.va') and sorted(made.get('models', [])) == ['VaRes/vres.va', 'VaRes/vres_params.vams'], made)
    cache = os.path.realpath(os.environ.get('QUCS_CACHE_DIR') or WS + '/spice4qucs')
    os.chmod(lib, 0o555)
    try:
        built = s.call('build_verilog_a', {'file': lib + '/vres.va'})
        osdi = os.path.realpath(built.get('osdi', '')) if built.get('osdi') else ''
        check('s12', 'build_verilog_a: the folder read-only, compiled into the cache (said)', built.get('compiled')
              and osdi.startswith(cache + '/osdi/vres-') and 'into the cache' in built and sorted(os.listdir(lib)) == ['vres.va', 'vres_params.vams'], built)
        place = s.call('describe_part', {'library': 'VaRes', 'part': 'vres'})['place']
        s.call('new_document', {})
        s.call('batch', {'calls': [
            {'tool': 'add_component', 'arguments': {'type': 'Vdc', 'name': 'V1', 'x': 100, 'y': 200, 'properties': {'U': '1 V'}}},
            {'tool': 'add_component', 'arguments': {'type': 'R', 'name': 'R2', 'x': 220, 'y': 100, 'properties': {'R': '1k'}}},
            {'tool': 'add_component', 'arguments': {'type': place['type'], 'name': 'X1', 'x': 340, 'y': 200, 'properties': place['properties']}},
            {'tool': 'add_component', 'arguments': {'type': '.DC', 'name': 'DC1', 'x': 120, 'y': 400}},
            {'tool': 'connect', 'arguments': {'from': 'V1.1', 'to': 'R2.1'}},
            {'tool': 'connect', 'arguments': {'from': 'R2.2', 'to': 'X1.1'}},
            {'tool': 'connect', 'arguments': {'from': 'X1.2', 'to': 'ground'}},
            {'tool': 'connect', 'arguments': {'from': 'V1.2', 'to': 'ground'}},
            {'tool': 'set_label', 'arguments': {'at': 'R2.2', 'name': 'out'}}], 'atomic': True})
        saved = s.call('save_document', {'as': proj + '/s12_top.sch'})
        link = proj + '/Libraries/VaRes/vres.va'
        check('s12', 'saved: the part\'s Verilog-A linked into the project (Libraries/VaRes, relative), said', os.path.islink(link)
              and not os.readlink(link).startswith('/') and os.path.realpath(link) == os.path.realpath(lib + '/vres.va')
              and 'Libraries/VaRes/vres.va' in str(saved), saved)
        sim = s.call('simulate', {'path': 's12_top.sch', 'brief': True})
        op = s.call('get_dataset', {'path': 's12_top.sch', 'operating_point': True})['operating point']['nodes']
        check('s12', 'the part placed by its name ("%s") and simulated: compiled beside the link first, its include found, v(out) = 2/3 V'
              % place['properties'].get('Lib'), sim.get('succeeded') and place['properties'].get('Lib') == 'VaRes'
              and near(op.get('v(out)', 0), 2 / 3, 1e-3) and os.path.isfile(proj + '/Libraries/VaRes/vres.osdi')
              and sorted(os.listdir(lib)) == ['vres.va', 'vres_params.vams'],
              (sim.get('errors'), op, os.listdir(proj + '/Libraries/VaRes') if os.path.isdir(proj + '/Libraries/VaRes') else None, os.listdir(lib)))
        # The project gets a library of the same name without the part (the review's case): reopened, the part is
        # still the search path's - its pins, its link and model kept - and simulates.
        open(proj + '/rc.sch', 'w').write(
            '<Qucs Schematic 26.1.5>\n<Components>\n'
            '  <Port P1 1 220 100 -23 12 0 0 "1" 1 "analog" 0>\n  <Port P2 1 280 100 4 12 1 2 "2" 1 "analog" 0>\n'
            '  <R R1 1 250 100 -26 15 0 0 "1 kOhm" 1 "26.85" 0 "0.0" 0 "0.0" 0 "26.85" 0 "european" 0>\n'
            '</Components>\n<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n')
        shadow = s.call('create_library', {'name': 'VaRes', 'subcircuits': ['rc'], 'destination': 'project'})
        check('s12', 'create_library: the project\'s own VaRes, without the part - the search path\'s of the name said (also_named)',
              os.path.isfile(proj + '/VaRes.lib') and [os.path.realpath(p) for p in shadow.get('also_named', [])]
              == [os.path.realpath(team + '/VaRes.lib')], shadow)
        s.call('close_document', {'path': 's12_top.sch'})
        s.call('open_document', {'path': proj + '/s12_top.sch'})
        sim = s.call('simulate', {'path': 's12_top.sch', 'brief': True})
        op = s.call('get_dataset', {'path': 's12_top.sch', 'operating_point': True})['operating point']['nodes']
        pins = [c for c in s.call('get_schematic', {'path': 's12_top.sch', 'components': ['X1']})['components']]
        check('s12', 'reopened beside it: the part still the search path\'s (2 pins), its link and model kept, v(out) = 2/3 V',
              sim.get('succeeded') and near(op.get('v(out)', 0), 2 / 3, 1e-3) and len(pins) == 1 and len(pins[0].get('pins', [])) == 2
              and os.path.islink(link) and os.path.isfile(proj + '/Libraries/VaRes/vres.osdi'), (sim.get('errors'), op, pins))
        os.remove(proj + '/VaRes.lib')
        s.call('delete', {'path': 's12_top.sch', 'names': ['X1']})
        saved = s.call('save_document', {'path': 's12_top.sch'})
        check('s12', 'the part taken away and saved: the link, its model and Libraries/ go, said', not os.path.lexists(link)
              and not os.path.exists(proj + '/Libraries') and 'taken away' in str(saved), (saved, os.listdir(proj)))
    finally:
        os.chmod(lib, 0o755)

# ---------------------------------------------------------------------------
@scenario
def s13_control_check(s):
    """the check of 3 October (Qucs-S_control_check_2026-10-03.md), its RC low-pass: add_analysis with properties;
    R1 changed in its Properties dialog by set_dialog, and then by send_input into the dialog - each simulated, the
    bandwidth following; a -3dB marker on the trace drawn in dB at the bandwidth get_dataset measures; Diagram
    Properties' tables read, a variable activated taking a trace; the Frame tab read and set by get_settings and
    set_settings"""
    s.call('new_document', {})
    s.call('batch', {'calls': [
        {'tool': 'add_component', 'arguments': {'type': 'Vac', 'name': 'V1', 'x': 100, 'y': 200, 'properties': {'U': '1 V'}}},
        {'tool': 'add_component', 'arguments': {'type': 'R', 'name': 'R1', 'x': 200, 'y': 100, 'properties': {'R': '1k'}}},
        {'tool': 'add_component', 'arguments': {'type': 'C', 'name': 'C1', 'x': 300, 'y': 200, 'properties': {'C': '159n'}}},
        {'tool': 'connect', 'arguments': {'from': 'V1.1', 'to': 'R1.1'}}, {'tool': 'connect', 'arguments': {'from': 'R1.2', 'to': 'C1.1'}},
        {'tool': 'connect', 'arguments': {'from': 'V1.2', 'to': 'ground'}}, {'tool': 'connect', 'arguments': {'from': 'C1.2', 'to': 'ground'}},
        {'tool': 'set_label', 'arguments': {'at': 'R1.2', 'name': 'out'}}]})
    ac = s.call('add_analysis', {'kind': 'ac', 'properties': {'Start': '10 Hz', 'Stop': '100 kHz', 'Points': '201'}})
    props = {p['name']: p['value'] for p in ac['analysis']['properties']}
    check('s13', 'add_analysis takes properties: AC from 10 Hz to 100 kHz, 201 points', (props.get('Start'), props.get('Stop'), props.get('Points'))
          == ('10 Hz', '100 kHz', '201'), props)
    s.call('save_document', {'as': WS + '/s13.sch'})
    def bandwidth():
        sim = s.call('simulate', {'path': 's13.sch', 'brief': True})
        ds = s.call('get_dataset', {'path': 's13.sch', 'variables': ['ac.v(out)'], 'measure': ['bandwidth']})
        return sim.get('succeeded'), ds['variables'][0]['measurements']['bandwidth']['value']
    rc = lambda r: 1 / (2 * math.pi * r * 159e-9)
    ok, bw = bandwidth()
    check('s13', 'simulated: the bandwidth 1/(2 pi RC), 1001 Hz, within 1 %', ok and near(bw, rc(1e3), 0.01), bw)
    # R1 in its Properties dialog: set_dialog by the field's label, then OK.
    s.call('context_menu', {'path': 's13.sch', 'on': {'part': 'R1'}, 'choose': 'Edit Properties'})
    d = s.call('get_dialog', {})
    labels = [c['label'] for c in d['controls']]
    said = s.call('set_dialog', {'set': [{'control': 'R (Value)', 'value': '2k'}], 'press': 'OK'})
    ok, bw2 = bandwidth()
    check('s13', 'set_dialog: R (Value) 2k applied on OK, said by name - the bandwidth halved', 'R (Value)' in labels
          and str(said).startswith('Set: R (Value).') and ok and near(bw2, rc(2e3), 0.01), (said, bw2))
    # ... and by send_input: clicked, typed, Return, OK.
    s.call('context_menu', {'path': 's13.sch', 'on': {'part': 'R1'}, 'choose': 'Edit Properties'})
    at = {c['label']: c.get('at') for c in s.call('get_dialog', {})['controls']}
    field = at['R (Value)']
    s.call('send_input', {'target': 'dialog', 'click': [field[0] + 5, field[1] + field[3] // 2], 'keys': 'Ctrl+A', 'text': '1k'})
    s.call('send_input', {'target': 'dialog', 'keys': 'Return'})
    s.call('set_dialog', {'press': 'OK'})
    ok, bw3 = bandwidth()
    check('s13', 'send_input into the dialog: 1k typed, Return, OK - the bandwidth back', ok and near(bw3, rc(1e3), 0.01), bw3)
    # The -3dB marker on the trace in dB, where get_dataset measures the bandwidth.
    dg = s.call('add_diagram', {'path': 's13.sch', 'traces': [{'variable': 'ac.v(out)', 'part': 'db'}], 'x_axis': {'log': True}})
    m = s.call('add_marker', {'path': 's13.sch', 'diagram': dg['diagram'], 'at': '-3dB'})
    check('s13', 'add_marker -3dB on the dB trace: crossing at the bandwidth within 1 %', near(m['found']['crossing'], bw3, 0.01)
          and m['found']['measured on'].startswith('dB'), m.get('found'))
    # Diagram Properties: the tables' texts; a variable activated takes a trace.
    s.call('context_menu', {'path': 's13.sch', 'on': {'diagram': dg['diagram']}, 'choose': 'Edit Properties'})
    tables = {c['label']: c for c in s.call('get_dialog', {})['controls'] if c['kind'] == 'table'}
    variables = tables.get('diagramVariables', {}).get('rows', [])
    s.call('set_dialog', {'set': [{'control': 'diagramVariables', 'value': 'ac.v(out)', 'action': 'activate'}], 'press': 'OK'})
    traces = s.call('get_schematic', {'path': 's13.sch'})['diagrams'][dg['diagram'] - 1]['traces']
    check('s13', 'Diagram Properties: its variables read as text; one activated, a second trace', ['ac.v(out)', 'dep', 'frequency'] in variables
          and len(traces) == 2, (variables, len(traces)))
    # The Frame tab.
    keys = [e['key'] for e in s.call('get_settings', {'path': 's13.sch', 'scope': 'document'})['settings']]
    s.call('set_settings', {'path': 's13.sch', 'scope': 'document', 'values': {'Frame/Size': 'DIN A4 landscape', 'Frame/Title': 'RC low-pass'}})
    after = {e['key']: e['value'] for e in s.call('get_settings', {'path': 's13.sch', 'scope': 'document'})['settings']}
    check('s13', 'the Frame tab: its five keys read, Size and Title set', all(k in keys for k in ('Frame/Size', 'Frame/Title', 'Frame/Drawn by', 'Frame/Date', 'Frame/Revision'))
          and after.get('Frame/Title') == 'RC low-pass' and after.get('Frame/Size') == 'DIN A4 landscape', keys)
    s.call('save_document', {'path': 's13.sch'})

# ---------------------------------------------------------------------------
if __name__ == '__main__':
    which = sys.argv[1:] or ['s1', 's2', 's3', 's4', 's5', 's6', 's7', 's8', 's9', 's10', 's11', 's12', 's13']
    print('files in', ROOT, '- library:', os.path.abspath(LIBRARY) if LIBRARY else own_library(APP) + " (the app's own)")
    for name, fn in list(globals().items()):
        if name.split('_')[0] in which and name[:1] == 's' and '_' in name and callable(fn): fn()
    json.dump(LOG, open(ROOT + '/log.json', 'w'), indent=1, default=str)
    bad = [r for r in RESULTS if not r[2]]
    print(f'\n{len(RESULTS)} checks, {len(bad)} failed')
    for r in bad: print('  FAIL', r[0], r[1], '<-', r[3])
