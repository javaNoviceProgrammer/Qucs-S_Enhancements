# Each finding's reproducer in the real program (qucs-s --mcp-server,
# offscreen), each in a server of its own: answered, or 'HANG' (no answer in
# its time) or 'EOF' (it died). On the code before the fixes every one but
# C2's (repro_c2.py; ASan) ends in EOF or HANG.
#   QUCS=<qucs-s> EXAMPLES=<qucs-s-26.1.1/examples> python3 repro_all.py SCRATCH
import os, shutil, sys, time
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
from mcp import Server

app, ex, scratch = os.environ['QUCS'], os.environ['EXAMPLES'], sys.argv[1]


def edited(src, old, new, name, ws):
    text = open(os.path.join(ex, src)).read()
    assert old in text, (src, old)
    with open(os.path.join(ws, name), 'w') as f: f.write(text.replace(old, new, 1))
    return name


def case(tag, prepare, steps):
    home = os.path.join(scratch, tag)
    shutil.rmtree(home, ignore_errors=True)
    s = Server(app, home, home + '.stderr')
    try:
        name = prepare(s.ws)
        t0 = time.time()
        said = 'answered'
        for tool, args, timeout in steps(name):
            r = s.call(tool, args, timeout)
            if r in ('HANG', 'EOF'): said = r; break
            said = 'answered: ...' + s.text(r)[-150:].replace('\n', ' ')
        print(f'{tag}: {said} ({time.time() - t0:.1f} s)', flush=True)
    finally:
        s.kill()


def copy(*names):
    def prepare(ws):
        for src, dst in names: shutil.copy(src, os.path.join(ws, dst))
        return names[0][1]
    return prepare


opened = lambda name: [('open_document', {'path': name}, 60)]
case('C1-die-in-nm', copy((os.path.join(here, 'c1_die_in_nm.qfem'), 'die.qfem')), opened)
case('C1-gds-units', copy((os.path.join(ex, 'multiphysics/heater_crosssection.qfem'), 'cut.qfem'),
                          (os.path.join(here, 'c1_gds_units.gds'), 'thermo_optic_heater.gds')), opened)
case('C3-mesh-1e-9', lambda ws: edited('multiphysics3d/msm_photodiode.qfem3d', '"hmax": "0.1"', '"hmax": "1e-9"', 'pd.qfem3d', ws),
     lambda n: opened(n) + [('fem_build', {'what': 'mesh', 'timeout': 600}, 700)])
case('H1-defs-bomb', copy((os.path.join(here, 'h1_defs_bomb.svg'), 'bomb.svg')), opened)
case('H2-array-closed', lambda ws: edited('multiphysics3d/msm_photodiode.qfem3d', '"count": ["N", "1"]', '"count": ["N", "2147483647"]', 'pd.qfem3d', ws),
     lambda n: opened(n) + [('close_document', {'unsaved': 'discard'}, 60)])
case('H2-chip-heater-closed', lambda ws: edited('multiphysics3d/chip_heater.qfem3d', '"position": ["50", "30"]', '"position": ["-1e308", "30"]', 'ch.qfem3d', ws),
     lambda n: opened(n) + [('close_document', {'unsaved': 'discard'}, 60)])
case('H3-rect-1e308', lambda ws: edited('multiphysics/soi_waveguide_modes.qfem', '"H + 2*d"', '"H + 1e308*d"', 'soi.qfem', ws), opened)
case('H4-markdown', copy((os.path.join(here, 'h4_markdown.md'), 'h4.md')), opened)
