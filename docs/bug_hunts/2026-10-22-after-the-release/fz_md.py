"""Fuzz the Markdown viewer (preview, math, image watch) on the ASan app: hostile .md files opened, read, rewritten."""
import json, os, random, sys, threading, time, struct, zlib
S = '/tmp/qucs-hunt'
sys.path.insert(0, S)
from mcp_client import Client
H = S + '/hunt2'; BOX = H + '/fzmd'
APP = '/Users/meisam/git/Qucs-S_Enhancements/build-asan/qucs/qucs-s.app/Contents/MacOS/qucs-s'
seed, count = int(sys.argv[1]), int(sys.argv[2])
rnd = random.Random(seed)
def png(w, h):
    raw = b''.join(b'\x00' + bytes([rnd.randrange(256)]) * (w * 3) for _ in range(h))
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b'')
open(BOX + '/a.png', 'wb').write(png(40, 30))
open(BOX + '/bad.png', 'wb').write(b'\x89PNG\r\n\x1a\n' + os.urandom(200))
open(BOX + '/huge.png', 'wb').write(png(1, 1)[:16] + struct.pack('>II', 60000, 60000) + png(1, 1)[24:])
PIECES = ['$x^2$', '$$\\frac{a}{b}$$', '$\\', '$$', '\\(', '\\[x\\]', '$\\begin{matrix}a&b\\\\c&d\\end{matrix}$', '$' + '{' * 300 + '$', '$\\sqrt' * 50 + '$',
          '![a](a.png)', '![b](bad.png)', '![h](huge.png)', '![x]()', '![y](#frag)', '![z](../../../../etc/hosts)', '![w](a.png?v=2)', '![n](nope.png)', '![d](data:image/png;base64,iVBORw0KGgo=)',
          '<!-- open', '<pre>', '</pre>', '<b title="x', '`code', '```', '~~~', '> quote', '- item', '1. item', '| a | b |\n|---|---|\n| 1 | 2 |', '<div>', '</div>', '[l](<a ', '<', '>' * 50,
          '# H', '***', '\\', '&amp;', '&#0;', '\u0000', '퟿', 'é😀', '\t', '    indented', '[x]: http://e', '[x]', '<script>alert(1)</script>', '<img src="a.png">']
def doc():
    parts = []
    for _ in range(rnd.randint(1, 120)):
        p = rnd.choice(PIECES)
        if rnd.random() < 0.05: p = p * rnd.randint(2, 400)
        parts.append(p)
        parts.append(rnd.choice(['', ' ', '\n', '\n\n', '\n> ', '\n- ', '\n    ']))
    if rnd.random() < 0.1: parts = ['> ' * 300 + 'deep'] + parts
    if rnd.random() < 0.1: parts = ['- ' * 200 + 'deep list'] + parts
    return ''.join(parts)
log = open(H + '/fz_md_%d.log' % seed, 'a')
for i in range(count):
    f = BOX + '/d%d.md' % i
    text = doc()
    open(f, 'w', encoding='utf-8', errors='surrogatepass').write(text)
    errf = H + '/fzmd/err_%d_%d.txt' % (seed, i)
    os.environ['MCP_STDERR'] = errf
    c = Client(H + '/fzmd.w', app=APP, env_extra={'ASAN_OPTIONS': 'detect_leaks=0:detect_stack_use_after_return=1'})
    res = {}
    def go():
        try:
            res['open'] = c.call('open_document', {'path': f})
            res['text'] = c.call('get_text', {'path': f})
            open(BOX + '/a.png', 'wb').write(png(rnd.randint(1, 80), rnd.randint(1, 80)))
            time.sleep(0.6)
            res['shot'] = c.call('screenshot', {'path': f, 'area': 'window'})
            res['edit'] = c.call('edit_text', {'path': f, 'edits': [{'lines': [1, 1], 'text': doc()}, {'find': rnd.choice(PIECES), 'replace': doc()[:200], 'all': True}]}) if rnd.random() < 0.5 else (False, '')
            if rnd.random() < 0.5: res['save'] = c.call('save_document', {'path': f})
            time.sleep(0.5)
            res['shot2'] = c.call('screenshot', {'area': 'window'})
            res['close'] = c.call('close_document', {'path': f, 'unsaved': 'discard'})
        except Exception as e:
            res['exc'] = repr(e)
    th = threading.Thread(target=go); th.start(); th.join(120)
    verdict = 'ok'
    if th.is_alive(): verdict = 'HANG'; c.p.kill()
    else:
        dead = c.p.poll() is not None
        c.close()
        if 'exc' in res or dead: verdict = 'DIED'
    err = open(errf, errors='replace').read() if os.path.exists(errf) else ''
    if 'ERROR: AddressSanitizer' in err or 'runtime error' in err: verdict = 'SANITIZER'
    elif 'ASSERT' in err: verdict = 'ASSERT'
    detail = [l for l in err.splitlines() if 'ERROR:' in l or 'runtime error' in l or 'ASSERT' in l or 'SUMMARY' in l][:3]
    log.write('%s d%d %s %s\n' % (verdict, i, [ (k, v[0]) for k, v in res.items() if isinstance(v, tuple)], detail)); log.flush()
    if verdict == 'ok':
        try: os.remove(errf)
        except OSError: pass
print('done')
