#!/usr/bin/env python3
"""The plot digitizer's three tools - find_plots, digitize_plot, get_digitized -
called over `qucs-s --mcp-server` with arguments from their schemas and past
them: wrong types, numbers huge, negative, fractional and out of range, pages
and plots that are not there, regions inside out, references on one point,
colours that are not colours, outputs that cannot be written, PDFs damaged,
empty, not PDFs, not files, kept records damaged.

    python3 fuzz_digitizer_tools.py <qucs-s> <folder of PDFs> <scratch> [calls] [seed]

Every answer must come (no death, no hang past 180 s), be a result or an
error, and leave the sanitizers quiet. Findings go to <scratch>/findings.txt
with the call that made them; the server is started again after a death.
"""
import collections, json, os, random, re, shutil, sys, time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '2026-11-09-crash-hardening'))
from mcp import Server, sanitizer_lines  # noqa: E402

HOSTILE_NUMBERS = [0, -0.0, -1, 1, 2, 3, 0.5, -0.5, 1e-300, 1e300, -1e300, 1e15, 2**31, 2**31 - 1, -2**31, 2**53, 2**63, 10**30, 7, 100, 0.999]
HOSTILE_STRINGS = ['', ' ', 'linear', 'log', 'auto', 'LOG', 'projective', 'affine', 'vector', 'raster', 'polar', '#ff0000', '#000', 'red', '#gggggg',
                   'png', 'jpeg', 'gif', 'all', '1-3', '3-1', '0', '-1', '1,2', '2-999999999', 'x' * 5000, '日本語', '\u0000', '../', '1e999']


def any_value(r, depth=0):
    k = r.randrange(9)
    if k == 0: return r.choice(HOSTILE_NUMBERS)
    if k == 1: return r.choice(HOSTILE_STRINGS)
    if k == 2: return r.choice([True, False, None])
    if k == 3 and depth < 3: return [any_value(r, depth + 1) for _ in range(r.randrange(6))]
    if k == 4 and depth < 3: return {r.choice(['at', 'x', 'y', 'r', 'theta', 'a']): any_value(r, depth + 1) for _ in range(r.randrange(4))}
    if k == 5: return [r.choice(HOSTILE_NUMBERS) for _ in range(r.choice([0, 1, 2, 3, 4, 5, 1000]))]
    if k == 6: return r.uniform(-1000, 1000)
    if k == 7: return r.randrange(-3, 12)
    return [[r.uniform(-100, 900) for _ in range(r.choice([2, 2, 2, 1, 3, 4]))] for _ in range(r.randrange(5))]


def number(r):
    return r.uniform(-50, 900) if r.random() < 0.7 else r.choice(HOSTILE_NUMBERS)


def region(r):
    if r.random() < 0.3: return [r.random(), r.random(), r.random(), r.random()]
    if r.random() < 0.7: return [number(r) for _ in range(4)]
    return [number(r) for _ in range(r.choice([0, 1, 3, 5, 100]))]


def args_for(tool, r, pdfs, scratch):
    """Mostly plausible arguments, each now and then hostile."""
    a = {}
    good, bad = pdfs
    if r.random() < 0.95: a['path'] = r.choice(good) if r.random() < 0.85 else r.choice(bad)
    typed = r.random() < 0.6   # (else an argument now and then of any type: most such calls end at its check)
    def maybe(key, plausible, p=0.4):
        if r.random() < p:
            a[key] = plausible() if typed or r.random() < 0.8 else any_value(r)
    if tool == 'find_plots':
        maybe('pages', lambda: r.choice(['all', '1', '1-2', [1, 2], 1, 2, '2-1', [0], [-1], 99, '1-999999']), 0.6)
        return a
    if tool == 'get_digitized':
        maybe('plot', lambda: r.choice([1, 2, 0, -1, 99]), 0.6)
        maybe('max_points', lambda: r.choice([0, 1, 2, 200, 10**9, -1]))
        maybe('picture', lambda: r.random() < 0.5)
        maybe('max_width', lambda: r.choice([1, 2, 10, 1000, 100000, 0, -5]))
        maybe('scale', lambda: r.choice([0.001, 0.5, 1, 4, 100, 0, -1, 1e9]))
        maybe('gray', lambda: True, 0.2)
        maybe('format', lambda: r.choice(['png', 'jpeg', 'bmp']), 0.2)
        return a
    if r.random() < 0.4:
        # Clean: a good PDF's first plot with two or three valid options, so
        # that the call goes all the way (each of some 25 options now and
        # then invalid, most calls end at one of their checks).
        a = {'path': r.choice(good), 'page': 1}
        if r.random() < 0.5: a['plot'] = 1
        options = [('route', lambda: r.choice(['auto', 'vector', 'raster'])), ('max_points', lambda: r.choice([0, 1, 2, 200])),
                   ('picture', lambda: r.random() < 0.5), ('max_width', lambda: r.choice([1, 64, 1000, 4000])), ('scale', lambda: r.choice([0.01, 1, 3])),
                   ('gray', lambda: True), ('format', lambda: r.choice(['png', 'jpeg'])), ('trace_scale', lambda: r.choice([0.5, 2, 16])),
                   ('x_scale', lambda: r.choice(['linear', 'log'])), ('y_scale', lambda: r.choice(['linear', 'log'])),
                   ('colours', lambda: [r.choice(['#ff0000', '#0000ff', '#008000', '#000000'])]), ('names', lambda: ['a', '=SUM(A1)', '日本']),
                   ('x_range', lambda: [r.uniform(-10, 0), r.uniform(1, 1e6)]), ('y_range', lambda: [r.uniform(-1e-9, 0), r.uniform(1e-9, 10)]),
                   ('exclude', lambda: [[r.random() * 0.5, r.random() * 0.5, 0.5 + r.random() * 0.5, 0.5 + r.random() * 0.5]]), ('exclude', lambda: []),
                   ('seeds', lambda: [[r.uniform(100, 500), r.uniform(120, 400)] for _ in range(r.randrange(1, 4))]),
                   ('csv', lambda: os.path.join(scratch, 'out', f'c{r.randrange(4)}.csv')), ('dataset', lambda: os.path.join(scratch, 'out', f'd{r.randrange(4)}.dat')),
                   ('keep', lambda: True), ('replace', lambda: 1)]
        for key, value in r.sample(options, r.randrange(2, 4)): a[key] = value()
        return a
    # (Most test PDFs have one page of one plot: else most calls stop at "has pages 1 to 1".)
    maybe('page', lambda: r.choice([1] * 10 + [2, 3, 0, -1, 99, 2**31 - 1]), 0.8)
    maybe('plot', lambda: r.choice([1] * 10 + [2, 3, 0, -1, 99]), 0.6)
    maybe('region', lambda: region(r), 0.2)
    maybe('x_scale', lambda: r.choice(['linear', 'log', 'auto', 'logx']), 0.2)
    maybe('y_scale', lambda: r.choice(['linear', 'log', 'auto']), 0.2)
    maybe('x_values', lambda: [number(r) for _ in range(r.choice([0, 1, 2, 5, 6, 40]))], 0.15)
    maybe('y_values', lambda: [number(r) for _ in range(r.choice([0, 1, 2, 5, 6, 40]))], 0.15)
    maybe('references', lambda: [{'at': [number(r), number(r)][:r.choice([2, 2, 2, 1, 0])], **({'x': number(r)} if r.random() < 0.8 else {}),
                                  **({'y': number(r)} if r.random() < 0.8 else {}), **({'r': number(r), 'theta': number(r)} if r.random() < 0.2 else {})}
                                 for _ in range(r.choice([0, 1, 2, 3, 4, 6, 50]))], 0.2)
    maybe('polar', lambda: True, 0.1)
    maybe('r_at_centre', lambda: number(r), 0.1)
    maybe('model', lambda: r.choice(['affine', 'projective', 'cubic']), 0.15)
    maybe('route', lambda: r.choice(['auto', 'vector', 'raster', 'both']), 0.4)
    maybe('colours', lambda: [r.choice(['#ff0000', '#0000ff', '#000000', '#ffffff', 'blue', '#12345', '']) for _ in range(r.choice([0, 1, 2, 3, 30]))], 0.2)
    maybe('seeds', lambda: [[number(r), number(r)] for _ in range(r.choice([0, 1, 2, 5, 100]))], 0.15)
    maybe('names', lambda: [r.choice(['a', '', 'x' * 3000, '=SUM(A1)', '日本', 'a/b', '../..']) for _ in range(r.choice([0, 1, 3, 40]))], 0.2)
    maybe('x_range', lambda: [number(r) for _ in range(r.choice([2, 2, 2, 0, 1, 3]))], 0.15)
    maybe('y_range', lambda: [number(r) for _ in range(r.choice([2, 2, 2, 0, 1, 3]))], 0.15)
    maybe('exclude', lambda: [region(r) for _ in range(r.choice([0, 1, 2, 50]))], 0.15)
    maybe('trace_scale', lambda: r.choice([0.01, 0.5, 1, 4, 16, 17, 1000, 0, -2]), 0.2)
    maybe('max_points', lambda: r.choice([0, 1, 2, 3, 200, 10**9, -1]), 0.4)
    maybe('picture', lambda: r.random() < 0.3, 0.5)
    maybe('max_width', lambda: r.choice([1, 2, 10, 1000, 100000, 0, -5]), 0.2)
    maybe('scale', lambda: r.choice([0.001, 0.5, 1, 4, 100, 0, -1, 1e9]), 0.15)
    maybe('gray', lambda: True, 0.1)
    maybe('format', lambda: r.choice(['png', 'jpeg', 'tiff']), 0.1)
    # Outputs: inside the scratch folder (made or not), a folder, or nowhere writable.
    out = lambda ext: r.choice([os.path.join(scratch, 'out', f'o{r.randrange(4)}.{ext}'), os.path.join(scratch, 'out'), os.path.join(scratch, 'no', 'such', 'dir', f'x.{ext}'),
                                f'out{r.randrange(3)}.{ext}', '', '/dev/null', os.path.join(scratch, 'out', 'x' * 300 + f'.{ext}')])
    maybe('csv', lambda: out('csv'), 0.15)
    maybe('dataset', lambda: out('dat'), 0.15)
    maybe('keep', lambda: True, 0.15)
    maybe('replace', lambda: r.choice([1, 2, 0, -1, 99]), 0.08)
    maybe('show', lambda: True, 0.05)
    return a


def prepare(src, ws, scratch, r):
    """The PDFs to call on, in the server's workspace: the good ones, damaged copies, and things that are not PDFs."""
    good = sorted(f for f in os.listdir(src) if f.endswith('.pdf'))
    names, bad = [], []
    for f in good:
        shutil.copy(os.path.join(src, f), os.path.join(ws, f))
        names.append(f)
    for i in range(min(12, len(good))):
        data = bytearray(open(os.path.join(src, r.choice(good)), 'rb').read())
        for _ in range(r.randrange(1, 20)):
            if len(data) < 32: break
            k = r.randrange(3)
            at = r.randrange(len(data))
            if k == 0: data[at] = r.randrange(256)
            elif k == 1: del data[at:at + r.randrange(400)]
            else: data = data[:max(16, at)]
        n = f'damaged_{i}.pdf'
        open(os.path.join(ws, n), 'wb').write(bytes(data))
        bad.append(n)
    open(os.path.join(ws, 'empty.pdf'), 'wb').close()
    open(os.path.join(ws, 'text.pdf'), 'w').write('not a pdf at all\n' * 100)
    os.makedirs(os.path.join(ws, 'folder.pdf'), exist_ok=True)
    bad += ['empty.pdf', 'text.pdf', 'folder.pdf', 'missing.pdf', '../outside.pdf', '/dev/null', '', 'a' * 2000 + '.pdf']
    # Kept records, damaged, beside some good PDFs.
    for f in good[:6]:
        side = os.path.join(ws, f + '.digitized.json')
        k = r.randrange(5)
        if k == 0: open(side, 'w').write('{ not json')
        elif k == 1: open(side, 'w').write(json.dumps({'format': 'qucs-s plot digitizer', 'version': 1, 'plots': [{'page': -5, 'series': [{'points': [[1e308, 'x']]}]}]}))
        elif k == 2: open(side, 'w').write(json.dumps({'format': 'qucs-s plot digitizer', 'version': 99, 'plots': 'none'}))
        elif k == 3: open(side, 'w').write('[' * 100000)
    return names, bad


def main():
    app, src, scratch = sys.argv[1], sys.argv[2], sys.argv[3]
    calls = int(sys.argv[4]) if len(sys.argv) > 4 else 500
    seed = int(sys.argv[5]) if len(sys.argv) > 5 else 1
    r = random.Random(seed)
    shutil.rmtree(scratch, ignore_errors=True)
    os.makedirs(os.path.join(scratch, 'out'), exist_ok=True)
    errpath = os.path.join(scratch, 'stderr.txt')
    findings = open(os.path.join(scratch, 'findings.txt'), 'a')
    server = Server(app, os.path.join(scratch, 'server'), errpath)
    pdfs = prepare(src, server.ws, scratch, r)
    pos, deaths, hangs, errors, results, slowest = 0, 0, 0, 0, 0, (0, None)
    why = collections.Counter()
    log = open(os.path.join(scratch, 'calls.jsonl'), 'w')
    for i in range(calls):
        tool = r.choice(['find_plots', 'digitize_plot', 'digitize_plot', 'digitize_plot', 'get_digitized'])
        args = args_for(tool, r, pdfs, scratch)
        t0 = time.time()
        ans = server.call(tool, args, timeout=180)
        took = time.time() - t0
        if took > slowest[0]: slowest = (took, (tool, args))
        hits, pos, _ = sanitizer_lines(errpath, pos)
        bad = None
        if ans in ('EOF', 'HANG'): bad = ans
        elif hits: bad = 'sanitizer: ' + ' | '.join(hits[:4])
        elif not isinstance(ans, dict) or ('result' not in ans and 'error' not in ans): bad = f'no answer: {str(ans)[:300]}'
        if isinstance(ans, dict):
            refused = 'error' in ans or ans.get('result', {}).get('isError')
            if refused:
                errors += 1
                why[re.sub(r'\d+(\.\d+)?', '#', server.text(ans) if 'result' in ans else json.dumps(ans['error']))[:90]] += 1
            else: results += 1
            log.write(json.dumps({'call': i, 'tool': tool, 'args': args, 's': round(took, 2), 'refused': bool(refused),
                                  **({'why': (server.text(ans) if 'result' in ans else json.dumps(ans['error']))[:200]} if refused else {})}) + '\n')
        if bad:
            line = f'call {i} (seed {seed}) {tool} {json.dumps(args)[:4000]} -> {bad} after {took:.1f} s'
            print('FINDING', line, flush=True)
            findings.write(line + '\n'); findings.flush()
            if ans == 'HANG': hangs += 1
            else: deaths += 1
            if ans in ('EOF', 'HANG') or 'AddressSanitizer' in bad:
                server.kill()
                server = Server(app, os.path.join(scratch, 'server'), errpath)
                pos = os.path.getsize(errpath)
        if i % 50 == 0: print(f'{i} calls: {results} answered, {errors} refused, {deaths} deaths, {hangs} hangs', flush=True)
    server.kill()
    for w, n in why.most_common(25): print(f'{n:5d}  {w}')
    print(f'{calls} calls (seed {seed}): {results} answered, {errors} refused, {deaths} deaths, {hangs} hangs; slowest {slowest[0]:.1f} s: '
          f'{slowest[1][0]} {json.dumps(slowest[1][1])[:500]}')
    return 1 if deaths or hangs else 0


if __name__ == '__main__':
    sys.exit(main())
