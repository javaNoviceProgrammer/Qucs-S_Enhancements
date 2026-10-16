# F5: each shipped multiphysics example opened in `qucs-s --mcp-server`,
# solved with fem_solve's timeout of SECONDS (3 unless given) - the solve goes
# on after it -, then closed: how long close_document takes, which is how
# long the solve took to stop. Copies of the examples are used; the server
# has its own HOME, settings, cache and trash in HUNT_RUNS.
#   QUCS=<qucs-s> HUNT_RUNS=<scratch> python3 close_times.py [SECONDS [example.qfem ...]]
import json, os, shutil, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..'))
APP = os.environ['QUCS']
H = os.path.join(os.environ['HUNT_RUNS'], 'close_times')
EX = os.path.join(ROOT, 'qucs-s-26.1.1', 'examples', 'multiphysics')
seconds = int(sys.argv[1]) if len(sys.argv) > 1 else 3
only = set(sys.argv[2:])
shutil.rmtree(H, ignore_errors=True)
for d in ('home', 'settings', 'cache', 'trash', 'models'):
    os.makedirs(os.path.join(H, d))
env = dict(os.environ, HOME=H + '/home', QUCS_SETTINGS_DIR=H + '/settings', QUCS_CACHE_DIR=H + '/cache',
           QUCS_TRASH_DIR=H + '/trash', QT_QPA_PLATFORM='offscreen', ASAN_OPTIONS='detect_leaks=0')
p = subprocess.Popen([APP, '--mcp-server', '--workspace', H + '/models'], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=subprocess.DEVNULL, env=env, text=True)
n = 0


def call(name, args):
    global n
    n += 1
    p.stdin.write(json.dumps({'jsonrpc': '2.0', 'id': n, 'method': 'tools/call', 'params': {'name': name, 'arguments': args}}) + '\n')
    p.stdin.flush()
    while True:
        line = p.stdout.readline()
        if not line:
            sys.exit('the server ended')
        try:
            m = json.loads(line)
        except ValueError:
            continue
        if m.get('id') == n:
            return m


p.stdin.write(json.dumps({'jsonrpc': '2.0', 'id': 0, 'method': 'initialize', 'params': {'protocolVersion': '2025-06-18', 'capabilities': {}}}) + '\n')
p.stdin.flush()
p.stdout.readline()
p.stdin.write(json.dumps({'jsonrpc': '2.0', 'method': 'notifications/initialized'}) + '\n')
p.stdin.flush()
for f in sorted(os.listdir(EX)):
    if not f.endswith('.qfem') or (only and f not in only):
        continue
    path = os.path.join(H, 'models', f)
    shutil.copy(os.path.join(EX, f), path)
    call('open_document', {'path': path})
    call('fem_solve', {'path': path, 'timeout': seconds})
    t0 = time.time()
    call('close_document', {'path': path, 'unsaved': 'discard'})
    print(f'{time.time() - t0:7.2f} s  {f}', flush=True)
p.stdin.close()
p.wait(60)
