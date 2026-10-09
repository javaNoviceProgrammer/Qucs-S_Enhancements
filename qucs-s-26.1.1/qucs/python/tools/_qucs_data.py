"""Values as the Python editor of Qucs-S shows them: a line about each variable
(the Python Shell's Variables, the debugger's) and a value's rows as a table
(the Data Viewer) - an array, a list, a dictionary of columns, a DataFrame, a
qucs.Dataset.

    summary(name, value)  {"name", "type", "size", "value", "table"}
    table(value, start, count)
        {"shape": [rows, columns], "columns": [...], "start": start, "index":
         [...] (the rows' labels, from start), "rows": [[cell, ...], ...] (from
         start, count of them at most), "note": what was done to show it}

A cell is a number (a float, an int), a string, or null (nothing there);
complex numbers, NaN and the infinities are strings.
"""

import math
import reprlib
import types

_short = reprlib.Repr()
_short.maxstring = 120
_short.maxother = 120
_short.maxlist = _short.maxtuple = _short.maxdict = _short.maxset = 12
_short.maxlevel = 2

MOST_COLUMNS = 1000


def _numpy():
    import sys
    return sys.modules.get('numpy')


def _pandas():
    import sys
    return sys.modules.get('pandas')


def kind_of(value):
    """Its type, and an array's shape and element type."""
    name = type(value).__name__
    shape = getattr(value, 'shape', None)
    if isinstance(shape, tuple) and hasattr(value, 'dtype'):
        return '%s %s' % (name, value.dtype)
    return name


def size_of(value):
    shape = getattr(value, 'shape', None)
    if isinstance(shape, tuple):
        return ' x '.join(str(n) for n in shape) if shape else '1'
    try:
        return str(len(value))
    except Exception:
        return ''


def short(value):
    try:
        text = _short.repr(value)
    except Exception as e:
        text = '<repr failed: %s>' % e
    return ' '.join(text.split())


def is_table(value):
    """Whether it can be shown as a table."""
    if isinstance(value, (str, bytes, bytearray, int, float, complex, bool, type(None), types.ModuleType,
                          types.FunctionType, types.BuiltinFunctionType, type)):
        return False
    shape = getattr(value, 'shape', None)
    if isinstance(shape, tuple) and hasattr(value, '__getitem__'):
        return True
    if isinstance(value, (list, tuple, set, frozenset)):
        return len(value) > 0
    try:
        from collections.abc import Mapping
        if isinstance(value, Mapping):
            return len(value) > 0
    except Exception:
        pass
    return False


def summary(name, value):
    return {'name': name, 'type': kind_of(value), 'size': size_of(value), 'value': short(value), 'table': is_table(value)}


def cell(v):
    """A value as a cell: a number, a string or null."""
    np = _numpy()
    if v is None:
        return None
    if isinstance(v, bool) or (np is not None and isinstance(v, np.bool_)):
        return str(bool(v))
    if isinstance(v, int) or (np is not None and isinstance(v, np.integer)):
        v = int(v)
        return v if -2 ** 53 <= v <= 2 ** 53 else str(v)
    if isinstance(v, float) or (np is not None and isinstance(v, np.floating)):
        v = float(v)
        return v if math.isfinite(v) else repr(v)
    if isinstance(v, complex) or (np is not None and isinstance(v, np.complexfloating)):
        v = complex(v)
        return '%r%s%rj' % (v.real, '-' if math.copysign(1, v.imag) < 0 else '+', abs(v.imag))
    if isinstance(v, str):
        return v if len(v) <= 2000 else v[:2000] + '...'
    return short(v)


def _rows_of(columns, length, start, count):
    """The rows from start of columns (each a sequence; a short one blank)."""
    end = min(length, start + count)
    rows = []
    for k in range(start, end):
        rows.append([cell(c[k]) if k < len(c) else None for c in columns])
    return rows


def table(value, start=0, count=1000):
    start = max(0, int(start))
    count = max(0, int(count))
    found = _table(value, start, count)
    found['start'] = start
    return found


def _table(value, start, count):
    np = _numpy()
    pd = _pandas()
    note = ''

    # A DataFrame, a Series: their columns and index.
    if pd is not None and isinstance(value, pd.DataFrame):
        columns = [str(c) for c in value.columns[:MOST_COLUMNS]]
        part = value.iloc[start:start + count, :MOST_COLUMNS]
        rows = [[cell(v) for v in row] for row in part.itertuples(index=False, name=None)]
        index = [cell(i) for i in part.index]
        if value.shape[1] > MOST_COLUMNS:
            note = 'The first %d columns of %d.' % (MOST_COLUMNS, value.shape[1])
        return {'shape': [int(value.shape[0]), len(columns)], 'columns': columns, 'index': index, 'rows': rows, 'note': note}
    if pd is not None and isinstance(value, pd.Series):
        part = value.iloc[start:start + count]
        return {'shape': [int(value.shape[0]), 1], 'columns': [str(value.name) if value.name is not None else 'value'],
                'index': [cell(i) for i in part.index], 'rows': [[cell(v)] for v in part], 'note': note}

    # An array: a column, a matrix, more axes made rows of the last.
    shape = getattr(value, 'shape', None)
    if isinstance(shape, tuple) and hasattr(value, '__getitem__'):
        a = value
        if np is not None and not isinstance(a, np.ndarray):
            try:
                a = np.asarray(a)
            except Exception:
                pass
        shape = tuple(int(n) for n in getattr(a, 'shape', shape))
        if len(shape) == 0:
            return {'shape': [1, 1], 'columns': ['value'], 'index': [0], 'rows': [[cell(a[()])]] if start == 0 else [], 'note': ''}
        if len(shape) == 1:
            end = min(shape[0], start + count)
            return {'shape': [shape[0], 1], 'columns': ['value'], 'index': list(range(start, end)),
                    'rows': [[cell(a[k])] for k in range(start, end)], 'note': ''}
        if len(shape) > 2:
            if np is not None and isinstance(a, np.ndarray):
                a = a.reshape(-1, shape[-1])
                note = 'Its shape %s shown as %d rows of its last axis (the axes before it in order).' % (
                    ' x '.join(str(n) for n in shape), a.shape[0])
                shape = a.shape
            else:
                return {'shape': [0, 0], 'columns': [], 'index': [], 'rows': [], 'note': 'An array of %d axes.' % len(shape)}
        columns = min(shape[1], MOST_COLUMNS)
        if shape[1] > MOST_COLUMNS:
            note = (note + ' ' if note else '') + 'The first %d columns of %d.' % (MOST_COLUMNS, shape[1])
        end = min(shape[0], start + count)
        rows = [[cell(a[r, c]) for c in range(columns)] for r in range(start, end)]
        return {'shape': [shape[0], columns], 'columns': [str(c) for c in range(columns)], 'index': list(range(start, end)),
                'rows': rows, 'note': note}

    # A mapping (a dict of columns, a qucs.Dataset): a column each.
    try:
        from collections.abc import Mapping
        mapping = isinstance(value, Mapping)
    except Exception:
        mapping = False
    if mapping:
        names = list(value.keys())[:MOST_COLUMNS]
        columns = []
        for n in names:
            v = value[n]
            if isinstance(v, (str, bytes)) or not hasattr(v, '__len__') or not hasattr(v, '__getitem__'):
                v = [v]
            elif np is not None and isinstance(v, np.ndarray) and v.ndim > 1:
                v = list(v.reshape(-1))
                note = 'Columns of more axes than one made one.'
            columns.append(v)
        length = max((len(c) for c in columns), default=0)
        return {'shape': [length, len(names)], 'columns': [str(n) for n in names], 'index': list(range(start, min(length, start + count))),
                'rows': _rows_of(columns, length, start, count), 'note': note}

    # A list, a tuple, a set: a column - or rows, of lists or of dicts.
    if isinstance(value, (set, frozenset)):
        try:
            value = sorted(value)
        except TypeError:
            value = list(value)
    if isinstance(value, (list, tuple)):
        items = value
        if items and all(isinstance(i, dict) for i in items[:200]):
            keys = []
            for i in items:
                for k in i:
                    if k not in keys:
                        keys.append(k)
                if len(keys) >= MOST_COLUMNS:
                    break
            end = min(len(items), start + count)
            rows = [[cell(items[r].get(k)) if isinstance(items[r], dict) else None for k in keys] for r in range(start, end)]
            return {'shape': [len(items), len(keys)], 'columns': [str(k) for k in keys], 'index': list(range(start, end)),
                    'rows': rows, 'note': note}
        nested = items and all(isinstance(i, (list, tuple)) or (np is not None and isinstance(i, np.ndarray) and i.ndim == 1)
                               for i in items[:200])
        if nested:
            width = min(max(len(i) for i in items), MOST_COLUMNS)
            end = min(len(items), start + count)
            rows = [[cell(items[r][c]) if c < len(items[r]) else None for c in range(width)] for r in range(start, end)]
            return {'shape': [len(items), width], 'columns': [str(c) for c in range(width)], 'index': list(range(start, end)),
                    'rows': rows, 'note': note}
        end = min(len(items), start + count)
        return {'shape': [len(items), 1], 'columns': ['value'], 'index': list(range(start, end)),
                'rows': [[cell(items[r])] for r in range(start, end)], 'note': note}
    return {'shape': [0, 0], 'columns': [], 'index': [], 'rows': [], 'note': 'Not a table: %s.' % kind_of(value)}
