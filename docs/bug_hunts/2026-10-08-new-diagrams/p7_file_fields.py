"""The new diagrams' fields in a saved schematic changed one at a time; each file loaded and rendered (qucs-s -p) on the ASan build."""
import os, re, shutil, subprocess, sys, time
seed_dir, out = sys.argv[1], sys.argv[2]
app = os.environ['QUCS']
src = open(seed_dir + '/rc.sch').read()
os.makedirs(out, exist_ok=True)
env = dict(os.environ, HOME=out + '/home', QUCS_SETTINGS_DIR=out + '/settings', QUCS_TRASH_DIR=out + '/trash', QUCS_CACHE_DIR=out + '/cache',
           QT_QPA_PLATFORM='offscreen', ASAN_OPTIONS='detect_leaks=0', UBSAN_OPTIONS='print_stacktrace=1:halt_on_error=1')
for d in ('home', 'settings', 'trash', 'cache'): os.makedirs(out + '/' + d, exist_ok=True)
shutil.copy(seed_dir + '/RC_filter_FFT.dat.ngspice', out)
def line(tag):
    return next(l for l in src.splitlines() if l.lstrip().startswith('<' + tag + ' '))
def field(tag, i, value, src=src):
    l = line(tag); t = l.split(' ')
    lead = len(l) - len(l.lstrip(' '))
    toks = l.strip().split(' ')
    toks[i] = value
    return src.replace(l, ' ' * lead + ' '.join(toks))
st = line('Stacked').strip().split(' ')
pi = st.index('-1') + 1          # the panes' count follows the -1 (as saved)
cases = {
  'stacked_panes_0': field('Stacked', pi, '0'), 'stacked_panes_100': field('Stacked', pi, '100'), 'stacked_panes_huge': field('Stacked', pi, '2147483647'),
  'stacked_panes_neg': field('Stacked', pi, '-5'),
  'trace_pane_99': src.replace('<"ngspice/tran.v(in)" #ff0000 1 3 0 0 0 0 0 0 2>', '<"ngspice/tran.v(in)" #ff0000 1 3 0 0 0 0 0 0 99>'),
  'trace_pane_neg': src.replace('<"ngspice/tran.v(in)" #ff0000 1 3 0 0 0 0 0 0 2>', '<"ngspice/tran.v(in)" #ff0000 1 3 0 0 0 0 0 0 -7>'),
  'limit_pane_50': src.replace('<Limit 1 0 2 "" 0,0.1>', '<Limit 1 0 50 "" 0,0.1>'),
  'limit_pane_neg': src.replace('<Limit 1 0 2 "" 0,0.1>', '<Limit 1 0 -3 "" 0,0.1>'),
  'limit_points_bad': src.replace('0,0.7;0.005,0.7;0.005,0.5;0.01,0.5', '0.01,0.7;0,abc;;,;nan,inf'),
  'limit_points_many': src.replace('0,0.7;0.005,0.7;0.005,0.5;0.01,0.5', ';'.join(f'{i*1e-8},0.5' for i in range(200000))),
  'marker_rel_self': src.replace('<Mkr 0.00400001 148 -317 3 0 0 2 - - -1 1>', '<Mkr 0.00400001 148 -317 3 0 0 2 - - -1 2>'),
  'marker_rel_99': src.replace('<Mkr 0.00400001 148 -317 3 0 0 2 - - -1 1>', '<Mkr 0.00400001 148 -317 3 0 0 2 - - -1 99>'),
  'marker_rel_neg': src.replace('<Mkr 0.00400001 148 -317 3 0 0 2 - - -1 1>', '<Mkr 0.00400001 148 -317 3 0 0 2 - - -1 -2147483648>'),
  'marker_loop': src.replace('<Mkr 0.002 84 -311 3 0 0>', '<Mkr 0.002 84 -311 3 0 0 2 - - -1 2>'),
  'spectrogram_seg_1e-12': src.replace('1 0.001 0.5 80 -', '1 1e-12 0.5 80 -'),
  'spectrogram_overlap_1': src.replace('1 0.001 0.5 80 -', '1 0.001 1 80 -'),
  'spectrogram_range_0': src.replace('1 0.001 0.5 80 -', '1 0.001 0.5 0 -'),
  'bathtub_ui_tiny': src.replace('-1 0.0005 - 0 - 1e-12', '-1 1e-300 - 0 - 1e-12'),
  'bathtub_ber_0': src.replace('-1 0.0005 - 0 - 1e-12', '-1 0.0005 - 0 - 0'),
  'bathtub_ber_1': src.replace('-1 0.0005 - 0 - 1e-12', '-1 0.0005 - 0 - 1'),
  'spectrum_harmonics_huge': src.replace('-1 5 9 1 1 - -', '-1 5 2000000000 1 1 - -'),
  'spectrum_window_99': src.replace('-1 5 9 1 1 - -', '-1 99 9 1 1 - -'),
  'contour_levels_huge': src.replace('-1 8 0 1 1 - -', '-1 2000000000 0 1 1 - -'),
  'bars_huge': src.replace('-1 1 - 15 ""', '-1 1 - 2000000000 ""'),
  'bars_0': src.replace('-1 1 - 15 ""', '-1 1 - 0 ""'),
  'constellation_tiny': src.replace('-1 0.0005 - - 2 ""', '-1 1e-300 - - 2 ""'),
  'constellation_mod_99': src.replace('-1 0.0005 - - 2 ""', '-1 0.0005 - - 99 ""'),
}
for name, text in cases.items():
    if text == src: print(f'{name:26s} (unchanged: pattern not found)'); continue
    f = f'{out}/{name}.sch'; open(f, 'w').write(text)
    t0 = time.time()
    try:
        p = subprocess.run([app, '-p', '-i', f, '-o', f'{out}/{name}.png'], env=env, capture_output=True, timeout=60, text=True, errors='replace')
        err = p.stderr
        bad = [l for l in err.splitlines() if 'ERROR: AddressSanitizer' in l or 'runtime error' in l or 'SUMMARY' in l]
        print(f'{name:26s} exit {p.returncode:4d} {time.time() - t0:5.1f}s {bad[:3] if bad else ""}', flush=True)
        if bad: open(f'{out}/{name}.err', 'w').write(err)
    except subprocess.TimeoutExpired:
        print(f'{name:26s} TIMEOUT after 60 s', flush=True)
