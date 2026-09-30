import json, os, sys, glob, random, re, time, shutil
import mcp
from mcp import Server, HERE
mcp.APP = sys.argv[1]; random.seed(int(sys.argv[2])); budget = float(sys.argv[3])
WS = HERE + '/wsfz2'; shutil.rmtree(WS, ignore_errors=True); os.makedirs(WS)
files = [f for f in glob.glob(HERE + '/exs/**/*.sch', recursive=True) if '<Mkr' in open(f, errors='replace').read()]
nums = ['-1', '0', '5', '6', '99', '-99', '2147483648', 'x', '', '1e3', '3.5', '-', '#zzzzzz', '#ff0000', '999999999']
def mut(text):
    out = []
    for l in text.split('\n'):
        s = l.strip()
        if s.startswith('<Mkr '):
            f = s[1:-1].split(' ')
            for _ in range(random.randint(1, 3)):
                r = random.random()
                if r < 0.5 and len(f) > 2: f[random.randrange(1, len(f))] = random.choice(nums)
                elif r < 0.7: f.append(random.choice(nums))
                elif r < 0.85 and len(f) > 3: f = f[:random.randrange(3, len(f))]
                else: f.insert(random.randrange(1, len(f)), random.choice(nums))
            l = l[:len(l) - len(l.lstrip())] + '<' + ' '.join(f) + '>'
        out.append(l)
    return '\n'.join(out)
s = Server(WS); t0 = time.time(); n = opened = 0; errs = []
while time.time() - t0 < budget:
    f = random.choice(files)
    name = 'm%d.sch' % n; n += 1
    open(WS + '/' + name, 'w').write(mut(open(f, errors='replace').read()))
    try:
        r = s.call('open_document', {'path': WS + '/' + name}, ok=False)
        if isinstance(r, str) and ' is open' in r: opened += 1
        s.call('get_schematic', {'path': name}, ok=False)
        s.call('export_image', {'path': name, 'save_as': WS + '/x.png'}, ok=False)
        s.call('save_document', {'path': name}, ok=False)
        s.call('close_document', {'path': name, 'unsaved': 'discard'}, ok=False)
        s.call('open_document', {'path': WS + '/' + name}, ok=False)
        s.call('close_document', {'path': name, 'unsaved': 'discard'}, ok=False)
    except Exception as e:
        errs.append((name, os.path.relpath(f, HERE + '/exs'), repr(e)[:120]))
        try: s.close()
        except Exception: pass
        s = Server(WS)
try: s.close()
except Exception: pass
print(len(files), 'seed files;', n, 'mutants,', opened, 'opened,', len(errs), 'server deaths')
for e in errs[:8]: print(e)
