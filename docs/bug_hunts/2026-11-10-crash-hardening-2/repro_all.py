# Each finding of the second round reproduced: the models in the real program
# (qucs-s --mcp-server, offscreen), each an example with one value changed;
# the converter's and the simulator's on the files beside this script.
#   QUCS=<qucs-s> QUCSATOR=<qucsator_rf> QUCSCONV=<qucsconv_rf> \
#   EXAMPLES=<qucs-s-26.1.1/examples> python3 repro_all.py SCRATCH
import os, shutil, subprocess, sys, threading, time
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(here, '..', '2026-11-09-crash-hardening'))
from mcp import Server

app, ex, scratch = os.environ['QUCS'], os.environ['EXAMPLES'], sys.argv[1]
MEM_GB = 8


def edited(src, old, new, name, ws):
    text = open(os.path.join(ex, src)).read()
    assert old in text, (src, old)
    with open(os.path.join(ws, name), 'w') as f: f.write(text.replace(old, new, 1))
    return os.path.join(ws, name)


def case(tag, src, old, new, steps):
    home = os.path.join(scratch, tag)
    shutil.rmtree(home, ignore_errors=True)
    s = Server(app, home, home + '.stderr')
    over = []
    def watch():   # the server killed past MEM_GB, and that said
        while s.p.poll() is None:
            out = subprocess.run(['ps', '-o', 'rss=', '-p', str(s.p.pid)], capture_output=True, text=True).stdout.strip()
            if out and int(out) > MEM_GB * 1024 * 1024:
                over.append(int(out) // 1024); s.p.kill(); return
            time.sleep(1)
    threading.Thread(target=watch, daemon=True).start()
    try:
        name = edited(src, old, new, os.path.basename(src), s.ws)
        t0 = time.time(); said = 'answered'
        for tool, args, timeout in steps(name):
            r = s.call(tool, args, timeout)
            if over: said = f'MEM past {over[0]} MB at {tool}'; break
            if r in ('HANG', 'EOF'): said = f'{r} at {tool}'; break
            said = 'answered: ...' + s.text(r)[-150:].replace('\n', ' ')
        print(f'{tag}: {said} ({time.time() - t0:.1f} s)', flush=True)
    finally:
        s.kill()


opened = lambda n: [('open_document', {'path': n}, 60)]
# S1: a ray trace that does not end, closed under its tab
case('S1-rays-closed', 'multiphysics/grin_lens.qfem', '"-4"]', '"(-4)*1e-300"]',
     lambda n: opened(n) + [('fem_solve', {'path': n, 'timeout': 5}, 60), ('close_document', {'path': n, 'unsaved': 'discard'}, 120)])
# S2: a 2D mesh 1e-7 fine
case('S2-mesh-1e-7', 'multiphysics/annulus_laplace.qfem', '"hmax": "0.15"', '"hmax": "0.0000001"',
     lambda n: opened(n) + [('fem_build', {'path': n, 'what': 'mesh', 'timeout': 300}, 360)])
# S3: a grating at 1e30 Hz
case('S3-grating-1e30', 'multiphysics/grating_orders.qfem', '"frequencies": "c0/lambda0"', '"frequencies": "1e30"',
     lambda n: opened(n) + [('fem_solve', {'path': n, 'timeout': 120}, 180)])


def run(tag, cmd, cwd=None):
    p = subprocess.run(cmd, capture_output=True, cwd=cwd, stdin=subprocess.DEVNULL,
                       env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0'))
    err = p.stderr.decode('utf-8', 'replace')
    rep = next((l.strip() for l in err.splitlines() if 'ERROR: AddressSanitizer' in l or 'runtime error' in l), '')
    print(f'{tag}: ' + (f'signal {-p.returncode}' if p.returncode < 0 else f'exit {p.returncode}') + (f', {rep[:120]}' if rep else ''), flush=True)


conv = os.environ['QUCSCONV']
for tag, fmt, out, f in [('R1-vcd-upscope', 'vcd', 'qucsdata', 'r1_vcd_upscope.vcd'), ('R1-vcd-dumpvars', 'vcd', 'qucsdata', 'r1_vcd_dumpvars.vcd'),
                         ('R2-spice-long-name', 'spice', 'qucs', 'r2_spice_long_name.cir'),
                         ('R3-touchstone-hash', 'touchstone', 'qucsdata', 'r3_touchstone_hash.s1p'),
                         ('R4-csv-nul-name', 'csv', 'qucsdata', 'r4_csv_nul_name.csv')]:
    run(tag, [conv, '-if', fmt, '-of', out, '-i', os.path.join(here, f), '-o', os.devnull])
run('R3-qucsator-spfile', [os.environ['QUCSATOR'], '-i', 'r3_spfile.net', '-o', os.devnull], cwd=here)
run('R4-qucsator-vfile', [os.environ['QUCSATOR'], '-i', 'r4_vfile.net', '-o', os.devnull], cwd=here)
