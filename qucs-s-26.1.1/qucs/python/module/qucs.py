"""A Qucs-S simulation's results in Python.

A dataset is what a simulation of a schematic leaves beside it: amp.dat.ngspice,
amp.dat.xyce, amp.dat.spopus or amp.dat (Qucsator), in text or binary. This
module reads one into arrays:

    import qucs
    data = qucs.load('amp.sch')        # the schematic's dataset (the newest)
    print(data)                        # its variables, their sizes and sweeps
    f = data['frequency']              # numpy arrays (lists without numpy)
    s21 = data['S[2,1]']               # complex where the values are
    data.dependencies('S[2,1]')        # ['frequency']: its axes

A variable over two sweeps has two axes, the outer sweep first: v[k] is the
inner sweep at the k-th value of the outer one.

    qucs.save('gain.dat', {'frequency': f, 'gain': gain}, independent=['frequency'])
    data = qucs.simulate('amp.sch')    # simulated by Qucs-S (ngspice), then read

Inside Qucs-S - Run, Debug, Run in Shell, the Python Shell - the module is on
the path. It needs Python 3 alone; numpy, when there, for the arrays.
"""

import os
import re
import shutil
import struct
import subprocess
from collections.abc import Mapping

try:
    import numpy as _np
except ImportError:   # (lists of floats and complex numbers instead)
    _np = None

__all__ = ['Dataset', 'load', 'save', 'simulate', 'dataset_of']

# A simulator's dataset: the schematic's DataSet (amp.dat) and this.
SUFFIXES = {'ngspice': '.ngspice', 'xyce': '.xyce', 'spiceopus': '.spopus', 'qucsator': ''}


def _number(text):
    """A value as a dataset's text has it: '1.5e-3', '+1.5e-3+j2e-4', '-j2e-4'."""
    j = text.find('j')
    if j < 0:
        return float(text)
    if j == 0 or text[j - 1] not in '+-':
        raise ValueError('not a number: %r' % text)
    real = text[:j - 1]
    imag = float(text[j + 1:])
    return complex(float(real) if real else 0.0, -imag if text[j - 1] == '-' else imag)


def _text(value):
    """A value as a dataset's text has it, every digit."""
    if isinstance(value, complex) or (_np is not None and isinstance(value, _np.complexfloating)):
        re_, im = float(value.real), float(value.imag)
        negative = im < 0 or (im == 0 and str(im).startswith('-'))
        return '%r%sj%r' % (re_, '-' if negative else '+', abs(im))
    return repr(float(value))


def _flat(values):
    """A sequence's values in a row: an array's in its order (the last axis
    fastest), nested lists' likewise."""
    if _np is not None and isinstance(values, _np.ndarray):
        return list(values.ravel())
    out = []

    def walk(v):
        if isinstance(v, (list, tuple)) or (_np is not None and isinstance(v, _np.ndarray)):
            for item in v:
                walk(item)
        else:
            out.append(v)
    walk(values)
    return out


def _shaped(values, shape):
    """\\a values (a row) as nested lists of \\a shape."""
    if len(shape) <= 1:
        return list(values)
    step = len(values) // shape[0]
    return [_shaped(values[k * step:(k + 1) * step], shape[1:]) for k in range(shape[0])]


class Dataset(Mapping):
    """A dataset read: a mapping of each variable's name to its values.

    data[name]                the values; over two sweeps or more, an array of
                              as many axes, in the order dependencies() gives
    data.dependencies(name)   the independent variables it is over, the outer
                              sweep first ([] for an independent one)
    data.independent          the independent variables (the sweeps)
    data.dependent            the others
    data.path                 the file
    """

    def __init__(self, path):
        self.path = os.path.abspath(os.fspath(path))
        self._values = {}    # a name's values, in a row
        self._over = {}      # its independent variables as the file has them: the inner first
        self._order = []
        with open(self.path, 'rb') as f:
            head = f.read(64)
        if not head.startswith(b'<Qucs Dataset'):
            raise ValueError('%s is not a Qucs dataset' % self.path)
        if head.split(b'\n', 1)[0].rstrip().endswith(b' binary>'):
            self._read_binary()
        else:
            self._read_text()

    def _add(self, name, over, values):
        if name not in self._values:
            self._order.append(name)
        self._values[name] = values
        self._over[name] = over

    def _array(self, values, complex_):
        if _np is not None:
            return _np.array(values, dtype=_np.complex128 if complex_ else _np.float64)
        return [complex(v) for v in values] if complex_ else [float(v) for v in values]

    def _read_text(self):
        with open(self.path, 'r', encoding='utf-8', errors='replace') as f:
            lines = f.read().split('\n')[1:]
        name, over, values = None, [], []
        for raw in lines:
            line = raw.strip()
            if not line:
                continue
            if line.startswith('<'):
                if line.startswith('</'):
                    if name is not None:
                        self._finish_text(name, over, values)
                    name = None
                    continue
                words = line[1:].rstrip('>').split()
                if len(words) >= 2 and words[0] in ('indep', 'dep'):
                    name, values = words[1], []
                    over = [] if words[0] == 'indep' else words[2:]
                continue
            if name is not None:
                values.append(line)
        if name is not None:   # (a block not closed: a file cut short)
            self._finish_text(name, over, values)

    def _finish_text(self, name, over, lines):
        numbers = [_number(v) for v in lines]
        complex_ = any(isinstance(v, complex) for v in numbers)
        self._add(name, over, self._array(numbers, complex_))

    def _read_binary(self):
        with open(self.path, 'rb') as f:
            data = f.read()
        damaged = '%s is a binary dataset that is damaged' % self.path
        footer = data[-43:]
        if len(data) < 64 + 43 or not footer.startswith(b'QDSINDEX ') or footer[25:26] != b' ':
            raise ValueError(damaged)
        at, length = int(footer[9:25], 16), int(footer[26:42], 16)
        index = data[at:at + length].decode('utf-8', 'replace').split('\n')
        if not index or index[0] != '<index>':
            raise ValueError(damaged)
        for line in index[1:]:
            if not line or line == '</index>':
                continue
            close = line.rfind('>')
            header = line[1:close].split()
            offset, count, kind = line[close + 1:].split()
            offset, count = int(offset), int(count)
            complex_ = kind == 'c'
            if _np is not None:
                values = _np.frombuffer(data, '<c16' if complex_ else '<f8', count, offset).astype(
                    _np.complex128 if complex_ else _np.float64)
            else:
                numbers = struct.unpack_from('<%dd' % (count * (2 if complex_ else 1)), data, offset)
                values = [complex(numbers[2 * k], numbers[2 * k + 1]) for k in range(count)] if complex_ else list(numbers)
            self._add(header[1], [] if header[0] == 'indep' else header[2:], values)

    # The mapping: its names in the file's order.
    def __getitem__(self, name):
        values = self._values[name]
        axes = self.dependencies(name)
        if len(axes) <= 1 or any(a not in self._values for a in axes):
            return values
        shape = [len(self._values[a]) for a in axes]
        total = 1
        for n in shape:
            total *= n
        if total != len(values):   # (sizes that do not agree: as they are)
            return values
        return values.reshape(shape) if _np is not None else _shaped(values, shape)

    def __iter__(self):
        return iter(self._order)

    def __len__(self):
        return len(self._order)

    def dependencies(self, name):
        """The independent variables \\a name is over, its axes' order: the outer sweep first."""
        return list(reversed(self._over[name]))

    @property
    def independent(self):
        return [n for n in self._order if not self._over[n]]

    @property
    def dependent(self):
        return [n for n in self._order if self._over[n]]

    def __repr__(self):
        return '<qucs.Dataset %s: %d variables>' % (os.path.basename(self.path), len(self))

    def __str__(self):
        rows = []
        for name in self._order:
            values = self._values[name]
            axes = self.dependencies(name)
            sizes = [len(self._values[a]) for a in axes if a in self._values] or [len(values)]
            complex_ = (values.dtype.kind == 'c') if _np is not None else any(isinstance(v, complex) for v in values)
            size = ' x '.join(str(n) for n in sizes) + (' complex' if complex_ else '')
            rows.append((name, size, 'over ' + ', '.join(axes) if axes else 'independent'))
        wide = [max([len(r[k]) for r in rows] + [0]) for k in range(2)]
        lines = ['%s: %d variables' % (os.path.basename(self.path), len(rows))]
        lines += ['  %-*s  %-*s  %s' % (wide[0], r[0], wide[1], r[1], r[2]) for r in rows]
        return '\n'.join(lines)


def _dataset_base(schematic):
    """The schematic's dataset as it names it (amp.dat), beside it."""
    schematic = os.path.abspath(os.fspath(schematic))
    folder = os.path.dirname(schematic)
    try:
        with open(schematic, 'r', encoding='utf-8', errors='replace') as f:
            for k, line in enumerate(f):
                m = re.match(r'\s*<DataSet=(.*)>\s*$', line)
                if m and m.group(1).strip():
                    return os.path.join(folder, os.path.basename(m.group(1).strip()))
                if k > 200 or line.strip() == '</Properties>':
                    break
    except OSError:
        pass
    return os.path.splitext(schematic)[0] + '.dat'


def dataset_of(schematic, simulator=None):
    """The dataset a simulation of \\a schematic left: \\a simulator's ('ngspice',
    'xyce', 'spiceopus', 'qucsator'), else the newest there is."""
    base = _dataset_base(schematic)
    if simulator is not None:
        if simulator not in SUFFIXES:
            raise ValueError('no simulator %r: one of %s' % (simulator, ', '.join(SUFFIXES)))
        path = base + SUFFIXES[simulator]
        if not os.path.isfile(path):
            raise FileNotFoundError('%s has no %s dataset (%s): simulate it first' % (schematic, simulator, path))
        return path
    found = [base + s for s in SUFFIXES.values() if os.path.isfile(base + s)]
    if not found:
        raise FileNotFoundError('%s has no dataset (%s or a simulator\'s): simulate it first' % (schematic, base))
    return max(found, key=os.path.getmtime)


def load(path, simulator=None):
    """The dataset at \\a path - or a schematic's (amp.sch): \\a simulator's, else the newest."""
    if os.fspath(path).lower().endswith('.sch'):
        path = dataset_of(path, simulator)
    return Dataset(path)


def save(path, variables, independent=(), dependencies=None):
    """Writes \\a variables (a name's values each) as a dataset in text, which a
    diagram of Qucs-S shows. \\a independent: the sweeps among them, the outer
    first; each other variable is over all of them, its values with as many
    axes (or in a row, the inner sweep fastest) - or over those
    \\a dependencies names for it ({'gain': ['frequency']})."""
    independent = list(independent)
    dependencies = dict(dependencies or {})
    for name in independent:
        if name not in variables:
            raise KeyError('the independent variable %r is not among the variables' % name)
    rows = {name: _flat(values) for name, values in variables.items()}
    lines = ['<Qucs Dataset 1.0.0>']
    for name in independent:
        lines.append('<indep %s %d>' % (name, len(rows[name])))
        lines += [_text(v) for v in rows[name]]
        lines.append('</indep>')
    for name, values in rows.items():
        if name in independent:
            continue
        over = list(dependencies.get(name, independent))
        expected = 1
        for axis in over:
            if axis not in rows:
                raise KeyError('%r is over %r, which is not among the variables' % (name, axis))
            expected *= len(rows[axis])
        if over and len(values) != expected:
            raise ValueError('%r has %d values, not the %d its sweeps (%s) have' % (name, len(values), expected, ', '.join(over)))
        lines.append('<dep %s %s>' % (name, ' '.join(reversed(over))) if over else '<indep %s %d>' % (name, len(values)))
        lines += [_text(v) for v in values]
        lines.append('</dep>' if over else '</indep>')
    with open(os.fspath(path), 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')


def simulate(schematic, simulator='ngspice', timeout=None):
    """Simulates \\a schematic with \\a simulator ('ngspice' or 'xyce') as Qucs-S
    does - Qucs-S's own program, without a window - and returns its dataset."""
    if simulator not in ('ngspice', 'xyce'):
        raise ValueError("simulate() runs 'ngspice' or 'xyce', not %r" % simulator)
    program = os.environ.get('QUCS_S_EXECUTABLE') or shutil.which('qucs-s')
    if not program:
        raise RuntimeError('No Qucs-S to simulate with: run the script from Qucs-S, or put qucs-s on the PATH.')
    schematic = os.path.abspath(os.fspath(schematic))
    out = _dataset_base(schematic) + SUFFIXES[simulator]
    before = os.path.getmtime(out) if os.path.isfile(out) else None
    environment = dict(os.environ, QT_QPA_PLATFORM='offscreen')
    run = subprocess.run([program, '-n', '-i', schematic, '-o', out, '--' + simulator, '--run'],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout, env=environment)
    if run.returncode != 0 or not os.path.isfile(out) or os.path.getmtime(out) == before:
        said = (run.stderr or run.stdout).decode('utf-8', 'replace').strip()
        raise RuntimeError('%s was not simulated with %s%s' % (schematic, simulator, ': ' + said[-2000:] if said else '.'))
    return Dataset(out)
