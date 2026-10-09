"""Values as the Python editor of Qucs-S shows them: a line about each variable
(the Python Shell's Variables, the debugger's) and a value's rows as a table
(the Data Viewer) - an array, a list, a dictionary of columns, a DataFrame, a
qucs.Dataset.

    summary(name, value)  {"name", "type", "size", "value", "table", "inside"}
    table(value, start, count, sort, descending, filters, token)
        {"shape": [rows, columns], "columns": [...], "start": start, "index":
         [...] (the rows' labels, from start), "rows": [[cell, ...], ...] (from
         start, count of them at most), "note": what was done to show it,
         "total": the rows before a filter}
      sorted by the column sort (descending), the rows the filters keep -
      {column: "> 5" | ">= 5" | "< 5" | "<= 5" | "== 5" | "!= 5" | text it
      has}; the order kept for the token (one Data Viewer's view) as long as
      it asks with it
    children(value, expression)   a value's insides, each a summary() with
        the expression that is it ("expression"; None: none) - a dict's
        items, a list's, an array's rows, a DataFrame's columns, an object's
        attributes
    as_variables(value, name)     (variables, x) for qucs.display()

A cell is a number (a float, an int), a string, or null (nothing there);
complex numbers, NaN and the infinities are strings.
"""

import json
import math
import re
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


def has_inside(value):
    """Whether a value has insides to show (cheaply: not made to know)."""
    if isinstance(value, (str, bytes, bytearray, int, float, complex, bool, type(None), types.ModuleType, types.FunctionType,
                          types.BuiltinFunctionType, types.MethodType, type)):
        return False
    try:
        from collections.abc import Mapping
        if isinstance(value, (Mapping, list, tuple, set, frozenset)):
            return len(value) > 0
    except Exception:
        return False
    shape = getattr(value, 'shape', None)
    if isinstance(shape, tuple):
        return len(shape) >= 1 and shape[0] > 0
    try:
        return any(not k.startswith('__') for k in vars(value))
    except TypeError:
        return False


def summary(name, value):
    return {'name': name, 'type': kind_of(value), 'size': size_of(value), 'value': short(value), 'table': is_table(value),
            'inside': has_inside(value)}


def children(value, expression, most=300):
    """A value's insides: summaries, each with the expression that is it."""
    np = _numpy()
    pd = _pandas()
    pairs = []
    if pd is not None and isinstance(value, pd.DataFrame):
        pairs = [(str(c), value[c], '%s[%r]' % (expression, c)) for c in list(value.columns)[:most]]
    else:
        try:
            from collections.abc import Mapping
            mapping = isinstance(value, Mapping)
        except Exception:
            mapping = False
        if mapping:
            pairs = [(repr(k), value[k], '%s[%r]' % (expression, k)) for k in list(value.keys())[:most]]
        elif isinstance(value, (list, tuple)):
            pairs = [('[%d]' % k, v, '%s[%d]' % (expression, k)) for k, v in enumerate(value[:most])]
        elif isinstance(value, (set, frozenset)):
            pairs = [('', v, None) for v in list(value)[:most]]
        elif isinstance(getattr(value, 'shape', None), tuple) and hasattr(value, '__getitem__'):
            try:
                pairs = [('[%d]' % k, value[k], '%s[%d]' % (expression, k)) for k in range(min(value.shape[0], most))]
            except Exception:
                pairs = []
        else:
            try:
                attributes = vars(value)
            except TypeError:
                attributes = {}
            pairs = [(k, v, '%s.%s' % (expression, k)) for k, v in sorted(attributes.items(), key=lambda kv: kv[0].lower())
                     if not k.startswith('__')][:most]
    out = []
    for name, child, child_expression in pairs:
        item = summary(name, child)
        item['expression'] = child_expression
        out.append(item)
    return out


def _safe_name(name):
    """A name a dataset takes for a variable."""
    text = re.sub(r'[^A-Za-z0-9_.]+', '_', str(name)).strip('_')
    return text or 'value'


def as_variables(value, name='value'):
    """What qucs.display() is given for a value: (a Dataset, None), or (its
    variables - a name's values each -, the sweep's name)."""
    np = _numpy()
    pd = _pandas()
    module = __import__('sys').modules.get('qucs')
    if module is not None and isinstance(value, getattr(module, 'Dataset', ())):
        return value, None
    name = _safe_name(name)
    if pd is not None and isinstance(value, pd.Series):
        value = value.to_frame(name if value.name is None else value.name)
    if pd is not None and isinstance(value, pd.DataFrame):
        variables = {}
        if not isinstance(value.index, pd.RangeIndex):
            variables[_safe_name(value.index.name or 'index')] = list(value.index)
        for c in value.columns:
            variables[_safe_name(c)] = list(value[c])
        return variables, next(iter(variables))
    try:
        from collections.abc import Mapping
        if isinstance(value, Mapping):
            variables = {_safe_name(k): v for k, v in value.items()}
            if not variables:
                raise ValueError('nothing in it to show')
            return variables, next(iter(variables))
    except ImportError:
        pass
    rows = value
    if np is not None and isinstance(value, np.ndarray):
        rows = value.tolist()
    if isinstance(rows, (list, tuple)) and rows:
        if all(isinstance(r, (list, tuple)) for r in rows):
            width = len(rows[0])
            if width >= 2 and all(len(r) == width for r in rows):
                variables = {name + '_x': [r[0] for r in rows]}
                for k in range(1, width):
                    variables['%s_%d' % (name, k)] = [r[k] for r in rows]
                return variables, name + '_x'
            raise ValueError('its rows are not of one length, two values or more each')
        if all(isinstance(v, (int, float, complex)) and not isinstance(v, bool) for v in rows):
            return {'index': list(range(len(rows))), name: list(rows)}, 'index'
    raise ValueError('%s is no array, list of numbers, table of columns or dataset' % kind_of(value))


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


_views = {}   # a Data Viewer's view (its token): the whole table and its rows' order


def _number(v):
    """A cell's number to sort by and compare (a complex one: its magnitude);
    None for text."""
    if isinstance(v, (int, float)):
        return float(v)
    if isinstance(v, str):
        try:
            return abs(complex(v.replace(' ', '')))
        except ValueError:
            try:
                return float(v)
            except ValueError:
                return None
    return None


_COMPARE = re.compile(r'^\s*(>=|<=|==|!=|=|>|<)\s*([-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?)\s*$')


def _passes(v, rule):
    m = _COMPARE.match(rule)
    if m:
        n = _number(v)
        if n is None or n != n:
            return False
        limit = float(m.group(2))
        return {'>': n > limit, '>=': n >= limit, '<': n < limit, '<=': n <= limit, '==': n == limit, '=': n == limit,
                '!=': n != limit}[m.group(1)]
    return rule.strip().lower() in ('' if v is None else str(v)).lower()


def table(value, start=0, count=1000, sort=None, descending=False, filters=None, token=None):
    start = max(0, int(start))
    count = max(0, int(count))
    filters = {int(k): str(v) for k, v in (filters or {}).items() if str(v).strip()}
    if sort is None and not filters:
        found = _table(value, start, count)
        found['start'] = start
        found['total'] = found['shape'][0]
        found['token'] = token
        return found
    key = (token, sort, bool(descending), json.dumps(filters, sort_keys=True))
    view = _views.get(key) if token is not None else None
    if view is None:
        whole = _table(value, 0, 1 << 62)
        rows = whole['rows']
        order = [i for i in range(len(rows)) if all(c < len(rows[i]) and _passes(rows[i][c], rule) for c, rule in filters.items())]
        if sort is not None and 0 <= int(sort) < whole['shape'][1]:
            column = int(sort)
            def rank(i):   # numbers before text, nothing (NaN, empty) last either way
                v = rows[i][column]
                n = _number(v)
                if n is not None and n == n:
                    return (0, n, '')
                if v is None or n is not None:
                    return (2, 0.0, '')
                return (1, 0.0, str(v).lower())
            present = [i for i in order if rank(i)[0] < 2]
            absent = [i for i in order if rank(i)[0] == 2]
            present.sort(key=rank, reverse=bool(descending))
            order = present + absent
        view = {'whole': whole, 'order': order}
        _views.clear()   # (one view kept: the last one asked of)
        if token is not None:
            _views[key] = view
    whole, order = view['whole'], view['order']
    picked = order[start:start + count]
    note = whole['note']
    if filters:
        note = (note + ' ' if note else '') + '%d of its %d rows kept by the filters.' % (len(order), whole['shape'][0])
    return {'shape': [len(order), whole['shape'][1]], 'columns': whole['columns'], 'start': start,
            'index': [whole['index'][i] for i in picked], 'rows': [whole['rows'][i] for i in picked], 'note': note,
            'total': whole['shape'][0], 'token': token}


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
