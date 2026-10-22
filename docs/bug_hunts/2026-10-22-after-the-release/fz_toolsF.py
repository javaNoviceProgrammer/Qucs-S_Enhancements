"""Fuzz every Qucs-S tool's arguments (type-confused, extreme, nested) on the ASan app, in a sandbox."""
import json, os, random, sys, threading, time
S = '/tmp/qucs-hunt'
sys.path.insert(0, S)
from mcp_client import Client
H = S + '/hunt2'
BOX = H + '/fztf'
APP = '/Users/meisam/git/Qucs-S_Enhancements/build-asan/qucs/qucs-s.app/Contents/MacOS/qucs-s'
seed, per = int(sys.argv[1]), int(sys.argv[2])
rnd = random.Random(seed)
SKIP = {'console', 'run_script', 'send_input', 'build_verilog_a', 'trigger_action', 'git_remote', 'git_clone', 'git_commit', 'git_commit_action',
        'git_discard', 'git_stash', 'git_stage', 'git_unstage', 'git_branch', 'git_tag', 'git_abort', 'git_resolve', 'git_init', 'git_ignore',
        'move_to_pane', 'set_ui', 'context_menu', 'set_dialog'}
tools = [t for t in json.load(open(H + '/tools.json')) if t['name'].startswith('fem_')]
PATHS = ['amp.sch', 'model.qfem', 'notes.md', BOX + '/amp.sch', BOX + '/new.sch', 'sub/x.sch', '../fzt_escape.txt', '', 'nope.qfem', BOX + '/out.png', 'x' * 300 + '.sch']
STR = ['', 'R1', 'net1', 'x' * 20000, '\u0000', 'nan', '-1', '1e309', '{', '$(echo hi)', 'é€😀', 'semi.V', 'std1', 'pg1', '1/0', 'linspace(0,1,1e9)', 'a' * 3]
def value(schema, depth=0):
    t = schema.get('type') if isinstance(schema, dict) else None
    if isinstance(t, list): t = rnd.choice(t)
    r = rnd.random()
    if 'enum' in schema and r < 0.5: return rnd.choice(schema['enum'])
    if r < 0.15:   # any type at all
        return rnd.choice([None, True, 0, -1, 1e308, 2 ** 63, 1.5, '', 'x', [], {}, [[[[]]]], {'a': {'b': {}}}, [1] * 5000])
    if t == 'string':
        return rnd.choice(PATHS) if rnd.random() < 0.3 else rnd.choice(STR)
    if t in ('number', 'integer'):
        return rnd.choice([0, 1, -1, 2, 10, 1e308, -1e308, 2 ** 31, 2 ** 53, 1e-300, 0.5, 100000, -2 ** 31])
    if t == 'boolean': return rnd.choice([True, False])
    if t == 'array':
        items = schema.get('items', {})
        n = rnd.choice([0, 1, 2, 3, 50, 2000]) if depth < 2 else rnd.choice([0, 1])
        return [value(items, depth + 1) for _ in range(n)]
    if t == 'object':
        props = schema.get('properties', {})
        return {k: value(v, depth + 1) for k, v in props.items() if rnd.random() < 0.6} if props and depth < 3 else rnd.choice([{}, {'x': 1}, {'R': '50'}])
    return rnd.choice(STR + [0, None, []])
def plausible(name, schema):
    t = schema.get('type') if isinstance(schema, dict) else None
    if isinstance(t, list): t = t[0]
    if 'enum' in schema: return rnd.choice(schema['enum'])
    n = name.lower()
    if t == 'string':
        if n in ('path', 'file', 'document', 'from', 'to', 'target', 'source', 'as', 'dataset', 'schematic', 'model', 'project', 'into', 'output'):
            return rnd.choice(['junction.qfem', 'well.qfem', 'flow.qfem', BOX + '/junction.qfem', BOX + '/well.qfem', BOX + '/flow.qfem'])
        if 'expr' in n: return rnd.choice(['semi.V', 'semi.n', 'semi.I_a', 'qw.alphaTE', 'qw.dnTMloc', 'x*2', 'semi.phin'])
        if n in ('name', 'component', 'label', 'ref', 'id', 'part'): return rnd.choice(['R1', 'C1', 'Q1', 'P1', 'SP1', 'net1'])
        if n in ('net', 'node'): return rnd.choice(['net1', 'gnd', 'out', 'in'])
        if n in ('study', 'tag', 'plot', 'export', 'node', 'parent'): return rnd.choice(['s', 'std1', 'pg1', 'semi', 'qw', 'pf1', 'anl', 'sp'])
        if n in ('value', 'text', 'content', 'equation'): return rnd.choice(['50', '1k', 'hello', 'x = 1'])
        if n in ('type', 'kind'): return rnd.choice(['R', 'C', 'L', 'rectangle', 'stationary'])
        return rnd.choice(['R1', 'x', '1', 'out', 'std1'])
    if t in ('number', 'integer'): return rnd.choice([0, 1, 2, 3, 10, 50, 100])
    if t == 'boolean': return rnd.choice([True, False])
    if t == 'array':
        items = schema.get('items', {})
        return [plausible(name, items) for _ in range(rnd.choice([0, 1, 2]))] if isinstance(items, dict) else []
    if t == 'object':
        props = schema.get('properties', {})
        return {k: plausible(k, v) for k, v in props.items() if rnd.random() < 0.5} if props else {}
    return 'x'
def argsFor(tool):
    props = tool.get('inputSchema', {}).get('properties', {})
    req = set(tool.get('inputSchema', {}).get('required', []))
    a = {k: plausible(k, v) for k, v in props.items() if k in req or rnd.random() < 0.4}
    if a and rnd.random() < 0.7:
        k = rnd.choice(list(a))
        a[k] = value(props[k])
    return a
log = open(H + '/fz_toolsF_%d.log' % seed, 'a')
def fresh(i):
    os.environ['MCP_STDERR'] = H + '/fztF_err_%d_%d.txt' % (seed, i)
    c = Client(H + '/fztF.w', app=APP, env_extra={'ASAN_OPTIONS': 'detect_leaks=0:detect_stack_use_after_return=1', 'QUCS_TOOL_FUZZ': '1'})
    for p in ('junction.qfem', 'well.qfem', 'flow.qfem'):
        c.call('open_document', {'path': BOX + '/' + p})
    return c
session = 0
c = fresh(session)
calls = 0
order = [t for t in tools for _ in range(per)]
rnd.shuffle(order)
for tool in order:
    a = argsFor(tool)
    res = {}
    def go():
        try: res['r'] = c.call(tool['name'], a)
        except Exception as e: res['exc'] = repr(e)
    th = threading.Thread(target=go); th.start(); th.join(90)
    calls += 1
    verdict = None
    if th.is_alive():
        verdict = 'HANG'
    elif 'exc' in res or res.get('r') is None or (res['r'][0] and res['r'][1] == '' and c.p.poll() is not None):
        verdict = 'DIED'
    elif c.p.poll() is not None:
        verdict = 'DIED'
    if verdict:
        errf = os.environ['MCP_STDERR']
        err = open(errf, errors='replace').read() if os.path.exists(errf) else ''
        kind = 'SANITIZER' if ('ERROR: AddressSanitizer' in err or 'runtime error' in err) else ('ASSERT' if 'ASSERT' in err else verdict)
        lines = [l for l in err.splitlines() if 'ERROR:' in l or 'runtime error' in l or 'ASSERT' in l or 'SUMMARY' in l][:3]
        log.write('%s %s %s %s\n' % (kind, tool['name'], json.dumps(a)[:1500], lines)); log.flush()
        try: c.p.kill()
        except Exception: pass
        session += 1
        c = fresh(session)
    else:
        e, text = res['r']
        log.write('ok %s %s %s\n' % (tool['name'], 'error' if e else 'answer', text[:120].replace('\n', ' '))); log.flush()
    # (Sanitizer reports that did not kill it.)
    errf = os.environ['MCP_STDERR']
    if os.path.exists(errf) and ('runtime error' in open(errf, errors='replace').read()):
        pass
print('done', calls, 'sessions', session)
