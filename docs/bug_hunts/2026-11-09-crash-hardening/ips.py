# macOS's crash reports (.ips) summarised: the program's path, the
# exception or signal, the abort message, the crashed thread's frames.
#   python3 ips.py [--path SUBSTRING] [--since HHMMSS] FILE...
import json, sys, os

args = sys.argv[1:]
want = None; since = None
if args and args[0] == '--path': want = args[1]; args = args[2:]
if args and args[0] == '--since': since = args[1]; args = args[2:]
for f in args:
    try:
        raw = open(f).read()
        head, body = raw.split('\n', 1)
        h = json.loads(head); b = json.loads(body)
    except Exception as e:
        continue
    path = b.get('procPath', '')
    if want and want not in path: continue
    stamp = os.path.basename(f).rsplit('-', 1)[-1].split('.')[0]
    if since and stamp < since: continue
    ex = b.get('exception', {})
    print(f'== {os.path.basename(f)} {path}')
    print('  ', ex.get('type'), ex.get('signal'), ex.get('subtype', ''), (b.get('asi') or {}))
    imgs = b.get('usedImages', [])
    th = [t for t in b.get('threads', []) if t.get('triggered')]
    for t in th[:1]:
        for fr in t.get('frames', [])[:18]:
            img = imgs[fr['imageIndex']].get('name', '?') if fr.get('imageIndex') is not None and fr['imageIndex'] < len(imgs) else '?'
            print(f"     {img:28.28} {fr.get('symbol', hex(fr.get('imageOffset', 0)))}{'' if 'sourceLine' not in fr else ' ' + str(fr.get('sourceFile')) + ':' + str(fr['sourceLine'])}")
