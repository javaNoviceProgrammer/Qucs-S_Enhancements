# The completer of the Python editor of Qucs-S (pythondoc.h): run as
# python -u -c <this> while a script is open, a question a line on its
# standard input - {"id", "kind", "source", "line" (from 1), "column" (from
# 0), "path"} - and its answer a line on its standard output. The kinds:
#   complete     the words that complete the name at the place ("items")
#   signature    the call the place is in: its parameters, the one being
#                written, where its bracket is ("signature")
#   help         what the name at the place is, and its documentation ("help")
#   definition   where the name at the place is defined ("definition")
#   references   where the name at the place is used, and defined
#                ("references": [{"file" ("": the script), "line", "column",
#                "end", "text", "definition"}, ...], "name", "scope")
#   rename       the name at the place renamed "new_name" wherever it is the
#                same name ("changes": [{"file", "text" (the file after)}],
#                "name", "count") - or "refusal": why not
#   imports      the imports that would define "name", not defined in the
#                script ("imports": [{"title", "line" (from 1: the line it
#                goes before), "text"}])
# jedi answers when the interpreter has it; otherwise this program, from the
# script, Python's builtins and keywords, the standard modules it imports
# (imported) and the modules beside it (read, not run).
import sys, json, re, os, io, ast, keyword, builtins, inspect, importlib, pkgutil, tokenize, sysconfig, site
try:
    import jedi
    ENGINE = ('jedi ' + str(getattr(jedi, '__version__', ''))).strip()
except Exception:
    jedi = None
    ENGINE = ''
STDLIB = set(getattr(sys, 'stdlib_module_names', ())) | set(sys.builtin_module_names)
NEVER_IMPORTED = {'antigravity', 'this', '__hello__', '__phello__', 'idlelib', 'turtledemo'}
MOST = 500

def kind_of(value):
    if inspect.ismodule(value):
        return 'module'
    if inspect.isclass(value):
        return 'class'
    if callable(value):
        return 'function'
    return 'instance'

KEYWORDS = {k: 'keyword' for k in keyword.kwlist + list(getattr(keyword, 'softkwlist', []))}
BUILTINS = {}
for n in dir(builtins):
    if not n.startswith('_'):
        BUILTINS[n] = kind_of(getattr(builtins, n))
for n in ('__name__', '__file__', '__doc__'):
    BUILTINS[n] = 'instance'

_modules = None
def module_names(folders):
    global _modules
    if _modules is None:
        found = set(sys.builtin_module_names)
        try:
            for m in pkgutil.iter_modules():
                found.add(m.name)
        except Exception:
            pass
        _modules = found
    found = set(_modules)
    for folder in folders:
        try:
            for m in pkgutil.iter_modules([folder]):
                found.add(m.name)
        except Exception:
            pass
    return {n: 'module' for n in found}

def source_of(name, folders, path=True):
    parts = name.split('.')
    for base in list(folders) + ([p for p in sys.path if p] if path else []):
        if not os.path.isdir(base):
            continue
        p = os.path.join(base, *parts)
        if os.path.isfile(p + '.py'):
            return p + '.py'
        if os.path.isfile(os.path.join(p, '__init__.py')):
            return os.path.join(p, '__init__.py')
    return None

def read_members(path):
    with open(path, 'rb') as f:
        tree = ast.parse(f.read())
    out = {}
    def visit(body):
        for node in body:
            if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
                out[node.name] = 'function'
            elif isinstance(node, ast.ClassDef):
                out[node.name] = 'class'
            elif isinstance(node, ast.Import):
                for a in node.names:
                    out[a.asname or a.name.split('.')[0]] = 'module'
            elif isinstance(node, ast.ImportFrom):
                for a in node.names:
                    if a.name != '*':
                        out[a.asname or a.name] = 'instance'
            elif isinstance(node, (ast.Assign, ast.AnnAssign, ast.AugAssign)):
                targets = node.targets if isinstance(node, ast.Assign) else [node.target]
                for t in targets:
                    for n in ast.walk(t):
                        if isinstance(n, ast.Name):
                            out.setdefault(n.id, 'statement')
            elif isinstance(node, (ast.If, ast.Try, ast.With)):
                visit(node.body)
                visit(getattr(node, 'orelse', []))
                visit(getattr(node, 'finalbody', []))
                for h in getattr(node, 'handlers', []):
                    visit(h.body)
    visit(tree.body)
    if os.path.basename(path) == '__init__.py':
        for m in pkgutil.iter_modules([os.path.dirname(path)]):
            out.setdefault(m.name, 'module')
    return out

_members = {}
def module_members(name, folders):
    key = (name, tuple(folders))
    if key in _members:
        return _members[key]
    out = {}
    root = name.split('.')[0]
    try:
        if root in STDLIB and root not in NEVER_IMPORTED and source_of(root, folders, path=False) is None:
            module = importlib.import_module(name)
            for n in dir(module):
                try:
                    out[n] = kind_of(getattr(module, n))
                except Exception:
                    out[n] = 'instance'
            for m in pkgutil.iter_modules(getattr(module, '__path__', None) or []):
                out.setdefault(m.name, 'module')
        else:
            path = source_of(name, folders)
            if path:
                out = read_members(path)
    except Exception:
        pass
    _members[key] = out
    return out

def imports_of(source):
    aliases = {}
    try:
        for node in ast.walk(ast.parse(source)):
            if isinstance(node, ast.Import):
                for a in node.names:
                    aliases[a.asname or a.name.split('.')[0]] = a.name if a.asname else a.name.split('.')[0]
            elif isinstance(node, ast.ImportFrom) and node.module and not node.level:
                for a in node.names:
                    aliases.setdefault(a.asname or a.name, node.module + '.' + a.name)
    except (SyntaxError, ValueError):
        # (Each of a list: import a, b.c as d; from m import (x, y as z).)
        for m in re.finditer(r'^[ \t]*import[ \t]+([\w., \t]+)', source, re.M):
            for part in m.group(1).split(','):
                n = re.match(r'\s*([\w.]+)(?:\s+as\s+(\w+))?\s*$', part)
                if n:
                    aliases[n.group(2) or n.group(1).split('.')[0]] = n.group(1) if n.group(2) else n.group(1).split('.')[0]
        for m in re.finditer(r'^[ \t]*from[ \t]+([\w.]+)[ \t]+import[ \t]+\(?([\w, \t]+)', source, re.M):
            for part in m.group(2).split(','):
                n = re.match(r'\s*(\w+)(?:\s+as\s+(\w+))?\s*$', part)
                if n:
                    aliases.setdefault(n.group(2) or n.group(1), m.group(1) + '.' + n.group(1))
    return aliases

def script_names(text):
    out = {}
    for m in re.finditer(r'^[ \t]*(?:async[ \t]+)?def[ \t]+(\w+)', text, re.M):
        out[m.group(1)] = 'function'
    for m in re.finditer(r'^[ \t]*class[ \t]+(\w+)', text, re.M):
        out[m.group(1)] = 'class'
    for m in re.finditer(r'^[ \t]*import[ \t]+([\w.]+)(?:[ \t]+as[ \t]+(\w+))?', text, re.M):
        out[m.group(2) or m.group(1).split('.')[0]] = 'module'
    whole = True
    try:
        for tok in tokenize.generate_tokens(io.StringIO(text).readline):
            if tok.type == tokenize.NAME:
                out.setdefault(tok.string, 'statement')
    except (tokenize.TokenError, IndentationError, SyntaxError):
        whole = False
    if not whole:
        for m in re.finditer(r'\b[A-Za-z_]\w*', text):
            out.setdefault(m.group(0), 'statement')
    return out

def own(source, line, column, path):
    lines = source.split('\n')
    before = lines[line - 1][:column] if 0 < line <= len(lines) else ''
    word = re.search(r'\w*$', before).group(0)
    start = sum(len(l) + 1 for l in lines[:line - 1]) + column - len(word)
    rest = source[:start] + source[start + len(word):]   # the script without the word being typed
    folders = [os.path.dirname(os.path.abspath(path))] if path else []
    m = re.match(r'^\s*from\s+([\w.]+)\s+import\s+(?:.*,\s*)?\(?\s*(\w*)$', before)
    if m:
        return module_members(m.group(1), folders), m.group(2)
    m = re.match(r'^\s*(?:import|from)\s+([\w.]*)$', before)
    if m:
        package, dot, prefix = m.group(1).rpartition('.')
        if dot:
            return {k: v for k, v in module_members(package, folders).items() if v == 'module'}, prefix
        return module_names(folders), prefix
    m = re.search(r'([A-Za-z_]\w*(?:\.[A-Za-z_]\w*)*)\.(\w*)$', before)
    if m:
        base, prefix = m.group(1), m.group(2)
        head, _, tail = base.partition('.')
        aliases = imports_of(rest)
        if head in aliases:
            found = module_members(aliases[head] + ('.' + tail if tail else ''), folders)
            if found:
                return found, prefix
        out = {}
        for n in re.finditer(r'\b' + re.escape(base) + r'\.([A-Za-z_]\w*)', rest):
            out[n.group(1)] = 'statement'
        return out, prefix
    if (word and word[0].isdigit()) or before[:len(before) - len(word)].endswith('.'):
        return {}, word   # (a number's, or an attribute of what is no name)
    names = dict(script_names(rest))
    names.update(BUILTINS)
    names.update(KEYWORDS)
    return names, word

# ----------------------------------------------------------------------
# Signatures, help and definitions.

LIBRARY = set()
for _key in ('stdlib', 'platstdlib', 'purelib', 'platlib'):
    try:
        LIBRARY.add(os.path.realpath(sysconfig.get_paths()[_key]))
    except Exception:
        pass
try:
    LIBRARY.update(os.path.realpath(p) for p in site.getsitepackages())
except Exception:
    pass

def is_library(path):
    """Python's library or a package installed for it: shown, not edited."""
    if not path:
        return False
    real = os.path.realpath(path)
    return any(real == p or real.startswith(p + os.sep) for p in LIBRARY)

def summary(doc, most=400):
    """A docstring's first paragraph."""
    text = (doc or '').strip().split('\n\n')[0].strip()
    return text if len(text) <= most else text[:most].rstrip() + '...'

def cut(doc, lines=30, most=3000):
    text = (doc or '').strip()
    parts = text.split('\n')
    if len(parts) > lines:
        text = '\n'.join(parts[:lines]) + '\n...'
    return text if len(text) <= most else text[:most].rstrip() + '...'

def in_string_or_comment(text):
    quote = None
    k = 0
    while k < len(text):
        c = text[k]
        if quote:
            if c == '\\':
                k += 1
            elif c == quote:
                quote = None
        elif c in '\'"':
            quote = c
        elif c == '#':
            return True
        k += 1
    return quote is not None

def offset_of(source, line, column):
    lines = source.split('\n')
    return sum(len(l) + 1 for l in lines[:line - 1]) + column

def place_of(source, offset):
    before = source[:offset]
    return before.count('\n') + 1, offset - (before.rfind('\n') + 1)

def scan_call(text):
    """The innermost call open at the end of text: where its bracket is, the
    argument's index and the argument's text so far - None outside a call."""
    stack = []   # each bracket open: [where, commas, where its argument begins, which]
    k, n = 0, len(text)
    while k < n:
        c = text[k]
        if c == '#':
            e = text.find('\n', k)
            k = n if e < 0 else e
            continue
        if c in '\'"':
            q = text[k:k + 3] if text[k:k + 3] in ("'''", '"""') else c
            e = k + len(q)
            while e < n:
                if text[e] == '\\':
                    e += 2
                    continue
                if text.startswith(q, e) or (len(q) == 1 and text[e] == '\n'):
                    break
                e += 1
            k = e + len(q)
            continue
        if c in '([{':
            stack.append([k, 0, k + 1, c])
        elif c in ')]}':
            if stack:
                stack.pop()
        elif c == ',' and stack:
            stack[-1][1] += 1
            stack[-1][2] = k + 1
        k += 1
    for entry in reversed(stack):
        if entry[3] == '(':
            return entry[0], entry[1], text[entry[2]:]
    return None

def tolerant_parse(source, line):
    """The script parsed - its broken lines mended: each line Python says is
    in the way (a call being typed, a bracket or a string left open) made a
    pass at its indentation, or nothing (an indentation not expected), until
    it parses; else up to the line being written."""
    lines = source.split('\n')
    work = list(lines)
    for _ in range(40):
        try:
            return ast.parse('\n'.join(work))
        except SyntaxError as e:
            k = (e.lineno or 0) - 1
            if not 0 <= k < len(work):
                break
            # The line said - or one above it left open, which the lines
            # before it say (a bracket that takes the next lines in).
            try:
                ast.parse('\n'.join(work[:k]))
            except SyntaxError as before:
                if before.lineno and 0 < before.lineno <= k:
                    e, k = before, before.lineno - 1
            except ValueError:
                pass
            indent = re.match(r'\s*', work[k]).group(0)
            mended = '' if isinstance(e, IndentationError) and 'unexpected indent' in (e.msg or '') else indent + 'pass'
            if work[k] == mended:
                if not mended:
                    break
                mended = ''   # (a pass did not do: nothing there)
            work[k] = mended
        except ValueError:
            break
    if 0 < line <= len(lines):
        try:
            return ast.parse('\n'.join(lines[:line - 1]))
        except (SyntaxError, ValueError):
            pass
    return None

def statements(body):
    """A body's statements, those under if, try and with too."""
    for node in body:
        yield node
        if isinstance(node, (ast.If, ast.Try, ast.With)):
            for part in (node.body, getattr(node, 'orelse', []), getattr(node, 'finalbody', [])):
                yield from statements(part)
            for h in getattr(node, 'handlers', []):
                yield from statements(h.body)

def find_def(tree, dotted):
    """The def or class dotted names: 'f', 'C', 'C.m' (the last one so named)."""
    if tree is None:
        return None
    body, node = tree.body, None
    for part in dotted.split('.'):
        node = None
        for n in statements(body):
            if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)) and n.name == part:
                node = n
        if node is None:
            return None
        body = node.body if isinstance(node, ast.ClassDef) else []
    return node

def method_named(tree, name):
    """The one method of the script's classes so named (None: none, or more
    than one) - an attribute of what cannot be told (self.gain, Amp().gain)."""
    if tree is None:
        return None
    found = [n for c in ast.walk(tree) if isinstance(c, ast.ClassDef) for n in c.body
             if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef)) and n.name == name]
    return found[0] if len(found) == 1 else None

def find_assignment(tree, name):
    """Where the script first gives name a value: an assignment, a loop, an import."""
    if tree is None:
        return None
    for node in statements(tree.body):
        targets = []
        if isinstance(node, ast.Assign):
            targets = node.targets
        elif isinstance(node, (ast.AnnAssign, ast.AugAssign, ast.For, ast.AsyncFor)):
            targets = [node.target]
        elif isinstance(node, (ast.Import, ast.ImportFrom)):
            for a in node.names:
                if (a.asname or a.name.split('.')[0]) == name:
                    return node
        for t in targets:
            for n in ast.walk(t):
                if isinstance(n, ast.Name) and n.id == name:
                    return node
    return None

def unparse(node):
    try:
        return ast.unparse(node)
    except Exception:
        return '...'

def params_of(a):
    def one(arg, default=None):
        text = arg.arg + (': ' + unparse(arg.annotation) if arg.annotation is not None else '')
        return text + ('=' + unparse(default) if default is not None else '')
    out = []
    positional = list(getattr(a, 'posonlyargs', [])) + list(a.args)
    defaults = [None] * (len(positional) - len(a.defaults)) + list(a.defaults)
    for k, (arg, default) in enumerate(zip(positional, defaults)):
        out.append(one(arg, default))
        if getattr(a, 'posonlyargs', None) and k == len(a.posonlyargs) - 1:
            out.append('/')
    if a.vararg:
        out.append('*' + one(a.vararg))
    elif a.kwonlyargs:
        out.append('*')
    for arg, default in zip(a.kwonlyargs, a.kw_defaults):
        out.append(one(arg, default))
    if a.kwarg:
        out.append('**' + one(a.kwarg))
    return out

def split_params(text):
    out, depth, start = [], 0, 0
    for k, c in enumerate(text):
        if c in '([{':
            depth += 1
        elif c in ')]}':
            depth -= 1
        elif c == ',' and depth == 0:
            out.append(text[start:k].strip())
            start = k + 1
    if text[start:].strip():
        out.append(text[start:].strip())
    return out

def stdlib_root(name, folders):
    root = name.split('.')[0]
    return root in STDLIB and root not in NEVER_IMPORTED and source_of(root, folders, path=False) is None

def resolve_module_path(full, folders):
    """A dotted name through a module: ('object', value) for a standard
    module's (imported), ('def', node, file) or ('module', file) for another's
    (read)."""
    parts = full.split('.')
    for k in range(len(parts), 0, -1):
        module, attributes = '.'.join(parts[:k]), parts[k:]
        if stdlib_root(module, folders):
            try:
                value = importlib.import_module(module)
            except Exception:
                continue
            try:
                for a in attributes:
                    value = getattr(value, a)
            except AttributeError:
                return None
            return ('object', value)
        file = source_of(module, folders)
        if file:
            if not attributes:
                return ('module', file)
            try:
                with open(file, 'rb') as f:
                    tree = ast.parse(f.read())
            except Exception:
                return None
            node = find_def(tree, '.'.join(attributes))
            return ('def', node, file) if node is not None else None
    return None

def resolve(dotted, source, line, path, folders, attribute=False):
    """What dotted is in the script: ('def', node, file), ('method', node,
    file), ('module', file), ('object', value) - or ('assigned', node, file)
    - else None. attribute: dotted follows a dot (of a call, a subscript)."""
    tree = tolerant_parse(source, line)
    head, _, tail = dotted.partition('.')
    node = find_def(tree, dotted)
    if node is not None and not attribute:
        return ('method' if '.' in dotted and not isinstance(node, ast.ClassDef) else 'def', node, path)
    aliases = imports_of(source)
    if head in aliases and not attribute:
        found = resolve_module_path(aliases[head] + ('.' + tail if tail else ''), folders)
        if found is not None:
            return found
    if hasattr(builtins, head) and find_assignment(tree, head) is None:
        value = getattr(builtins, head)
        try:
            for part in tail.split('.') if tail else []:
                value = getattr(value, part)
        except AttributeError:
            return None
        return ('object', value)
    if not tail and not attribute:
        node = find_assignment(tree, head)
        if node is not None:
            return ('assigned', node, path)
    if tail or attribute:
        node = method_named(tree, dotted.split('.')[-1])
        if node is not None:
            return ('method', node, path)
    return None

def dotted_at(source, line, column):
    """The name at a place, with what is before it and a dot (math.sqrt) -
    and whether that begins with a dot (Amp().gain: an attribute)."""
    lines = source.split('\n')
    text = lines[line - 1] if 0 < line <= len(lines) else ''
    column = max(0, min(column, len(text)))
    if in_string_or_comment(text[:column]):
        return None
    end = column
    while end < len(text) and (text[end].isalnum() or text[end] == '_'):
        end += 1
    start = column
    while start > 0 and (text[start - 1].isalnum() or text[start - 1] in '_.'):
        start -= 1
    attribute = text[start:start + 1] == '.'
    dotted = text[start:end].strip('.')
    return (dotted, attribute) if re.match(r'[A-Za-z_][\w.]*$', dotted) else (None, False)

def active_param(params, index, keyword_name):
    """Which of params the argument being written is."""
    names = [re.match(r'\**(\w*)', p).group(1) for p in params]
    if keyword_name:
        for k, n in enumerate(names):
            if n == keyword_name and not params[k].startswith('*'):
                return k
        return next((k for k, p in enumerate(params) if p.startswith('**')), -1)
    positional, star = [], -1
    for k, p in enumerate(params):
        if p == '/':
            continue
        if p.startswith('*'):
            if p != '*' and not p.startswith('**'):
                star = k
            break
        positional.append(k)
    return positional[index] if index < len(positional) else star

def own_signature(source, line, column, path, folders):
    offset = offset_of(source, line, column)
    call = scan_call(source[:offset])
    if call is None:
        return None
    where, index, argument = call
    before = source[:where]
    m = re.search(r'([A-Za-z_][\w.]*)\s*$', before)
    if m is None or re.search(r'\b(def|class)\s+[\w.]*\s*$', before) or m.group(1) in KEYWORDS:
        return None
    dotted = m.group(1)
    attribute = m.start(1) > 0 and before[m.start(1) - 1] == '.'
    found = resolve(dotted, source, line, path, folders, attribute)
    name = dotted.split('.')[-1]
    params, doc = None, ''
    if found and found[0] == 'object':
        value = found[1]
        try:
            params = [str(p) for p in inspect.signature(value).parameters.values()]
        except (TypeError, ValueError):
            first = (inspect.getdoc(value) or '').split('\n', 1)[0]
            m = re.match(r'\s*[\w.]+\((.*)\)', first)
            params = split_params(m.group(1)) if m else None
        doc = summary(inspect.getdoc(value))
    elif found and found[0] in ('def', 'method'):
        node = found[1]
        if isinstance(node, ast.ClassDef):
            init = next((n for n in node.body if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef)) and n.name == '__init__'), None)
            params = params_of(init.args)[1:] if init is not None else []
            doc = summary(ast.get_docstring(node) or (ast.get_docstring(init) if init is not None else ''))
        else:
            params = params_of(node.args)
            if found[0] == 'method' and params and params[0].split(':')[0] in ('self', 'cls'):
                params = params[1:]
            doc = summary(ast.get_docstring(node))
    if params is None:
        return None
    k = re.match(r'\s*(\w+)\s*=(?!=)', argument)
    open_line, open_column = place_of(source, where)
    return {'name': name, 'params': params, 'index': active_param(params, index, k.group(1) if k else None),
            'doc': doc, 'open': [open_line, open_column]}

def own_help(source, line, column, path, folders):
    dotted, attribute = dotted_at(source, line, column)
    if not dotted or dotted in KEYWORDS:
        return None
    found = resolve(dotted, source, line, path, folders, attribute)
    if not found:
        return None
    if found[0] == 'object':
        value = found[1]
        kind = kind_of(value)
        title = dotted
        if kind in ('function', 'class'):
            try:
                title = dotted + str(inspect.signature(value))
            except (TypeError, ValueError):
                pass
            return {'title': title, 'type': kind, 'text': cut(inspect.getdoc(value))}
        if kind == 'module':
            return {'title': 'module ' + dotted, 'type': kind, 'text': cut(inspect.getdoc(value))}
        return {'title': '%s: %s = %s' % (dotted, type(value).__name__, repr(value)[:200]), 'type': 'instance', 'text': ''}
    if found[0] in ('def', 'method'):
        node = found[1]
        if isinstance(node, ast.ClassDef):
            bases = ', '.join(unparse(b) for b in node.bases)
            title = 'class %s%s' % (node.name, '(%s)' % bases if bases else '')
            return {'title': title, 'type': 'class', 'text': cut(ast.get_docstring(node))}
        return {'title': 'def %s(%s)' % (node.name, ', '.join(params_of(node.args))), 'type': 'function',
                'text': cut(ast.get_docstring(node))}
    if found[0] == 'module':
        try:
            with open(found[1], 'rb') as f:
                text = ast.get_docstring(ast.parse(f.read()))
        except Exception:
            text = ''
        return {'title': 'module ' + dotted, 'type': 'module', 'text': cut(text)}
    return None

def own_definition(source, line, column, path, folders):
    dotted, attribute = dotted_at(source, line, column)
    if not dotted or dotted in KEYWORDS:
        return None
    found = resolve(dotted, source, line, path, folders, attribute)
    name = dotted.split('.')[-1]
    if not found:
        return None
    if found[0] in ('def', 'method', 'assigned'):
        node, file = found[1], found[2] or ''
        mine = not file or (path and os.path.abspath(file) == os.path.abspath(path))
        return {'file': '' if mine else file, 'line': node.lineno, 'column': node.col_offset, 'name': name,
                'library': False if mine else is_library(file)}
    if found[0] == 'module':
        return {'file': found[1], 'line': 1, 'column': 0, 'name': name, 'library': is_library(found[1])}
    value = found[1]
    try:
        file = inspect.getsourcefile(value)
        line_of = 1 if inspect.ismodule(value) else inspect.getsourcelines(value)[1]
    except (TypeError, OSError):
        file = None
    if not file:
        return {'builtin': True, 'name': name}
    return {'file': file, 'line': max(1, line_of), 'column': 0, 'name': name, 'library': is_library(file)}

def jedi_signature(source, line, column, path):
    found = jedi.Script(code=source, path=path).get_signatures(line, column)
    if not found:
        return None
    s = found[0]
    try:
        doc = summary(s.docstring(raw=True))
    except Exception:
        doc = ''
    start = getattr(s, 'bracket_start', None) or (line, column)
    return {'name': s.name, 'params': [p.to_string() for p in s.params],
            'index': s.index if s.index is not None else -1, 'doc': doc, 'open': [start[0], start[1]]}

def jedi_help(source, line, column, path):
    script = jedi.Script(code=source, path=path)
    names = script.help(line, column)
    if not names:
        return None
    n = names[0]
    text = n.docstring() or ''
    title = n.full_name or n.name
    if not text and n.type in ('statement', 'instance', 'param'):
        inferred = script.infer(line, column)
        if inferred:
            title = '%s: %s' % (n.name, inferred[0].name)
    return {'title': title, 'type': n.type, 'text': cut(text)}

def jedi_definition(source, line, column, path):
    script = jedi.Script(code=source, path=path)
    names = script.goto(line, column, follow_imports=True) or script.infer(line, column)
    if not names:
        return None
    n = names[0]
    file = str(n.module_path) if n.module_path else ''
    if n.line is None or (not file and n.in_builtin_module()):
        return {'builtin': True, 'name': n.name}
    mine = not file or (path and os.path.abspath(file) == os.path.abspath(path))
    return {'file': '' if mine else file, 'line': n.line, 'column': n.column or 0, 'name': n.name,
            'library': False if mine else is_library(file)}

# ----------------------------------------------------------------------
# References and renaming: jedi's, else the script's own names, by Python's
# rules of scope (a function's locals, a class body's, the module's).

MOST_REFERENCES = 2000


def char_column(text, byte_offset):
    """ast's column (UTF-8 bytes) as characters."""
    return len(text.encode('utf-8')[:byte_offset].decode('utf-8', 'replace'))


class Scope:
    def __init__(self, node, kind, parent):
        self.node, self.kind, self.parent = node, kind, parent
        self.bound, self.globals, self.nonlocals = set(), set(), set()


def _targets(node):
    """The names an assignment's target binds."""
    for n in ast.walk(node):
        if isinstance(n, ast.Name) and isinstance(n.ctx, (ast.Store, ast.Del)):
            yield n.id
        elif isinstance(n, ast.Starred) and isinstance(n.value, ast.Name):
            yield n.value.id


class Names(ast.NodeVisitor):
    """Each place of a name in a script, and the scope that binds it."""

    def __init__(self, source):
        self.lines = source.split('\n')
        self.module = Scope(None, 'module', None)
        self.scope = self.module
        self.places = []      # (name, line, column, end, scope looked up in, is a definition)

    # -- where
    def text(self, line):
        return self.lines[line - 1] if 0 < line <= len(self.lines) else ''

    def add(self, name, line, column, definition=False):
        self.places.append((name, line, column, column + len(name), self.scope, definition))

    def add_word(self, name, line, after=0):
        """\a name as a word on \a line, from character \a after on."""
        m = re.compile(r'\b%s\b' % re.escape(name)).search(self.text(line), after)
        if m:
            self.add(name, line, m.start(), True)

    # -- scopes
    def enter(self, node, kind):
        scope = Scope(node, kind, self.scope)
        self.scope = scope
        return scope

    def leave(self, scope):
        self.scope = scope.parent

    def bind(self, name):
        self.scope.bound.add(name)

    def arguments(self, a):
        for arg in list(getattr(a, 'posonlyargs', [])) + list(a.args) + list(a.kwonlyargs) + [a.vararg, a.kwarg]:
            if arg is not None:
                self.bind(arg.arg)
                self.add(arg.arg, arg.lineno, char_column(self.text(arg.lineno), arg.col_offset), True)
                if arg.annotation is not None:
                    self.visit(arg.annotation)

    def visit_FunctionDef(self, node):
        for d in node.decorator_list:
            self.visit(d)
        for default in list(node.args.defaults) + [d for d in node.args.kw_defaults if d is not None]:
            self.visit(default)
        if node.returns is not None:
            self.visit(node.returns)
        self.bind(node.name)
        line = node.lineno
        while line < len(self.lines) and not re.match(r'\s*(async\s+)?def\b', self.text(line)):
            line += 1   # (past its decorators)
        m = re.match(r'\s*(?:async\s+)?def\s+', self.text(line))
        if m:
            self.add(node.name, line, m.end(), True)
        scope = self.enter(node, 'function')
        self.arguments(node.args)
        for statement in node.body:
            self.visit(statement)
        self.leave(scope)

    visit_AsyncFunctionDef = visit_FunctionDef

    def visit_Lambda(self, node):
        for default in list(node.args.defaults) + [d for d in node.args.kw_defaults if d is not None]:
            self.visit(default)
        scope = self.enter(node, 'function')
        self.arguments(node.args)
        self.visit(node.body)
        self.leave(scope)

    def visit_ClassDef(self, node):
        for d in node.decorator_list:
            self.visit(d)
        for b in list(node.bases) + [k.value for k in node.keywords]:
            self.visit(b)
        self.bind(node.name)
        line = node.lineno
        while line < len(self.lines) and not re.match(r'\s*class\b', self.text(line)):
            line += 1
        m = re.match(r'\s*class\s+', self.text(line))
        if m:
            self.add(node.name, line, m.end(), True)
        scope = self.enter(node, 'class')
        for statement in node.body:
            self.visit(statement)
        self.leave(scope)

    def comprehension(self, node, parts):
        # (The first iterable is the enclosing scope's.)
        self.visit(node.generators[0].iter)
        scope = self.enter(node, 'comprehension')
        for k, g in enumerate(node.generators):
            for name in _targets(g.target):
                self.bind(name)
            self.visit(g.target)
            if k > 0:
                self.visit(g.iter)
            for condition in g.ifs:
                self.visit(condition)
        for part in parts:
            self.visit(part)
        self.leave(scope)

    def visit_ListComp(self, node):
        self.comprehension(node, [node.elt])

    visit_SetComp = visit_GeneratorExp = visit_ListComp

    def visit_DictComp(self, node):
        self.comprehension(node, [node.key, node.value])

    def visit_Global(self, node):
        for name in node.names:
            self.scope.globals.add(name)
            self.add_word(name, node.lineno, char_column(self.text(node.lineno), node.col_offset) + len('global'))

    def visit_Nonlocal(self, node):
        for name in node.names:
            self.scope.nonlocals.add(name)
            self.add_word(name, node.lineno, char_column(self.text(node.lineno), node.col_offset) + len('nonlocal'))

    def visit_Name(self, node):
        if isinstance(node.ctx, (ast.Store, ast.Del)):
            self.bind(node.id)
        self.add(node.id, node.lineno, char_column(self.text(node.lineno), node.col_offset), isinstance(node.ctx, ast.Store))

    def visit_NamedExpr(self, node):
        # (A walrus binds in the scope around a comprehension.)
        scope = self.scope
        while scope.kind == 'comprehension':
            scope = scope.parent
        scope.bound.add(node.target.id)
        self.places.append((node.target.id, node.target.lineno, char_column(self.text(node.target.lineno), node.target.col_offset),
                            char_column(self.text(node.target.lineno), node.target.col_offset) + len(node.target.id), scope, True))
        self.visit(node.value)

    def visit_alias(self, node):
        name = node.asname or node.name.split('.')[0]
        if name == '*':
            return
        self.bind(name)
        line = getattr(node, 'lineno', None)
        if line is not None:
            text = self.text(line)
            start = char_column(text, node.col_offset)
            if node.asname:
                m = re.compile(r'\bas\s+(%s)\b' % re.escape(node.asname)).search(text, start)
                if m:
                    self.add(name, line, m.start(1), True)
            else:
                self.add(name, line, start, True)

    def visit_ExceptHandler(self, node):
        if node.type is not None:
            self.visit(node.type)
        if node.name:
            self.bind(node.name)
            m = re.compile(r'\bas\s+(%s)\b' % re.escape(node.name)).search(self.text(node.lineno))
            if m:
                self.add(node.name, node.lineno, m.start(1), True)
        for statement in node.body:
            self.visit(statement)

    # -- which binding a place is of
    def lookup(self, name, scope):
        """The scope whose name it is (None: a builtin, or nowhere)."""
        if name in scope.globals:
            return self.module if name in self.module.bound else None
        s, first = scope, True
        while s is not None:
            if name in s.nonlocals and s is scope:
                s = s.parent
                first = False
                continue
            if name in s.bound and (first or s.kind != 'class') and name not in s.globals:
                return s
            s = s.parent
            first = False
        return None


def own_names(source):
    tree = ast.parse(source)
    names = Names(source)
    names.visit(tree)
    return names


def own_references(source, line, column, path, folders, rename_to=None):
    dotted, attribute = dotted_at(source, line, column) or (None, False)
    if not dotted:
        return None
    name = dotted.split('.')[-1]
    if name in KEYWORDS:
        return None
    try:
        names = own_names(source)
    except (SyntaxError, ValueError):
        return {'refusal': 'The script has a syntax error: fix it first.'} if rename_to is not None else None
    if attribute or '.' in dotted:
        if rename_to is not None:
            return {'refusal': 'Renaming an attribute (.%s) needs jedi, installed for this Python (pip install jedi).' % name}
        # Every .name of the script, and the methods so named: what it may be.
        found = []
        tree = ast.parse(source)
        for node in ast.walk(tree):
            if isinstance(node, ast.Attribute) and node.attr == name and getattr(node, 'end_lineno', None):
                text = names.text(node.end_lineno)
                end = char_column(text, node.end_col_offset)
                found.append((node.end_lineno, end - len(name), end, isinstance(node.ctx, ast.Store)))
            elif isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) and node.name == name:
                found += [(n[1], n[2], n[3], True) for n in names.places if n[0] == name and n[5] and n[1] >= node.lineno][:1]
        places = sorted(set(found))
        return {'name': name, 'scope': 'attribute', 'references': [
            {'file': '', 'line': l, 'column': c, 'end': e, 'text': names.text(l), 'definition': d} for l, c, e, d in places]}
    # The place asked about, and the binding it is of.
    at = [p for p in names.places if p[0] == name and p[1] == line and p[2] <= column <= p[3]]
    if not at:
        return None
    scope = names.lookup(name, at[0][4])
    if scope is None:
        if rename_to is not None:
            return {'refusal': '%s is built into Python, or not defined in the script.' % name}
        same = [p for p in names.places if p[0] == name and names.lookup(name, p[4]) is None]
    else:
        same = [p for p in names.places if p[0] == name and names.lookup(name, p[4]) is scope]
    places = sorted({(p[1], p[2], p[3], p[5]) for p in same})
    kind = {'module': 'module', 'function': 'function', 'class': 'class', 'comprehension': 'comprehension'}[scope.kind] if scope else 'builtin'
    out = {'name': name, 'scope': kind, 'references': [
        {'file': '', 'line': l, 'column': c, 'end': e, 'text': names.text(l), 'definition': d} for l, c, e, d in places[:MOST_REFERENCES]]}
    if rename_to is not None:
        lines = source.split('\n')
        for l, c, e, _ in sorted(places, reverse=True):
            lines[l - 1] = lines[l - 1][:c] + rename_to + lines[l - 1][e:]
        return {'name': name, 'count': len(places), 'changes': [{'file': '', 'text': '\n'.join(lines)}]}
    return out


def jedi_references(source, line, column, path):
    script = jedi.Script(code=source, path=path)
    found = script.get_references(line, column, include_builtins=False)
    if not found:
        return None
    lines = source.split('\n')
    out = []
    for r in found[:MOST_REFERENCES]:
        file = str(r.module_path) if r.module_path else ''
        mine = not file or (path and os.path.abspath(file) == os.path.abspath(path))
        if mine:
            text = lines[r.line - 1] if 0 < r.line <= len(lines) else ''
        else:
            import linecache
            text = linecache.getline(file, r.line).rstrip('\n')
        out.append({'file': '' if mine else file, 'line': r.line, 'column': r.column, 'end': r.column + len(r.name),
                    'text': text, 'definition': bool(r.is_definition())})
    return {'name': found[0].name, 'scope': 'jedi', 'references': out}


def jedi_rename(source, line, column, path, new_name):
    script = jedi.Script(code=source, path=path)
    names = script.get_references(line, column, include_builtins=False)
    if not names:
        return {'refusal': 'There is no name at the cursor to rename.'}
    if any(n.in_builtin_module() for n in names):
        return {'refusal': '%s is built into Python.' % names[0].name}
    refactoring = script.rename(line, column, new_name=new_name)
    changes = []
    for file, changed in refactoring.get_changed_files().items():
        file = str(file)
        mine = not path or os.path.abspath(file) == os.path.abspath(path)
        if not mine and is_library(file):
            return {'refusal': '%s is in Python\'s library (%s): not renamed.' % (names[0].name, file)}
        changes.append({'file': '' if mine else file, 'text': changed.get_new_code()})
    return {'name': names[0].name, 'count': len(names), 'changes': changes}


# ----------------------------------------------------------------------
# Imports that would define a name (Quick Fix)

ALIASES = {'np': ('numpy', 'np'), 'pd': ('pandas', 'pd'), 'plt': ('matplotlib.pyplot', 'plt'), 'mpl': ('matplotlib', 'mpl'),
           'sp': ('scipy', 'sp'), 'sns': ('seaborn', 'sns'), 'tf': ('tensorflow', 'tf'), 'nx': ('networkx', 'nx'),
           'sym': ('sympy', 'sym'), 'skrf': ('skrf', None), 'rf': ('skrf', 'rf')}
COMMON = ['math', 'cmath', 'os', 'os.path', 'sys', 're', 'json', 'time', 'datetime', 'itertools', 'functools', 'collections',
          'pathlib', 'typing', 'dataclasses', 'random', 'statistics', 'glob', 'shutil', 'subprocess', 'csv', 'fractions',
          'decimal', 'copy', 'pprint', 'textwrap', 'string', 'operator', 'enum', 'io', 'struct', 'numpy', 'scipy.constants',
          'qucs']

def has_module(name, folders):
    try:
        if source_of(name, folders, path=False):
            return True
        return importlib.util.find_spec(name) is not None
    except Exception:
        return False

def import_line(source):
    """The line (from 1) an import goes before: after the imports at the top
    (and a docstring, comments, from __future__), else at the start."""
    try:
        tree = ast.parse(source)
    except (SyntaxError, ValueError):
        return 1
    after = 0
    for k, node in enumerate(tree.body):
        if k == 0 and isinstance(node, ast.Expr) and isinstance(getattr(node, 'value', None), ast.Constant) \
                and isinstance(node.value.value, str):
            after = node.end_lineno
        elif isinstance(node, (ast.Import, ast.ImportFrom)):
            after = node.end_lineno
        else:
            break
    if after == 0:   # (past a #! line and a coding line)
        lines = source.split('\n')
        while after < len(lines) and lines[after].startswith('#') and (after == 0 or 'coding' in lines[after]):
            after += 1
    return after + 1

def imports_for(name, source, folders):
    found = []
    def add(text):
        if text not in found:
            found.append(text)
    if name in ALIASES:
        module, alias = ALIASES[name]
        if has_module(module.split('.')[0], folders):
            add('import %s as %s' % (module, alias) if alias else 'import %s' % module)
    if has_module(name, folders):
        add('import %s' % name)
    import warnings
    for module in COMMON:
        try:
            if module.split('.')[0] in STDLIB or has_module(module.split('.')[0], folders):
                with warnings.catch_warnings():
                    warnings.simplefilter('ignore')
                    value = importlib.import_module(module)
                public = getattr(value, '__all__', None)
                if not name.startswith('_') and hasattr(value, name) and (public is None or name in public) \
                        and not inspect.ismodule(getattr(value, name)):
                    add('from %s import %s' % (module, name))
        except Exception:
            pass
    # A module beside the script that defines it.
    for folder in folders:
        try:
            for m in pkgutil.iter_modules([folder]):
                path = source_of(m.name, [folder], path=False)
                if path and read_members(path).get(name) in ('function', 'class', 'statement'):
                    add('from %s import %s' % (m.name, name))
        except Exception:
            pass
    at = import_line(source)
    return [{'title': t, 'line': at, 'text': t + '\n'} for t in found[:12]]

def answer(request):
    kind = request.get('kind') or 'complete'
    source, line, column, path = request.get('source', ''), int(request.get('line', 1)), int(request.get('column', 0)), request.get('path') or None
    folders = [os.path.dirname(os.path.abspath(path))] if path else []
    if kind == 'imports':
        return '', {'imports': imports_for(request.get('name') or '', source, folders)}
    if kind in ('references', 'rename'):
        new_name = request.get('new_name') or ''
        if kind == 'rename' and (not new_name.isidentifier() or keyword.iskeyword(new_name)):
            return '', {'rename': {'refusal': '%r is no name Python takes.' % new_name}}
        if jedi is not None:
            try:
                found = jedi_references(source, line, column, path) if kind == 'references' else \
                    jedi_rename(source, line, column, path, new_name)
                return ENGINE, {kind: found}
            except Exception:
                pass
        return '', {kind: own_references(source, line, column, path, folders, new_name if kind == 'rename' else None)}
    if kind in ('signature', 'help', 'definition'):
        if jedi is not None:
            try:
                ask = {'signature': jedi_signature, 'help': jedi_help, 'definition': jedi_definition}[kind]
                return ENGINE, {kind: ask(source, line, column, path)}
            except Exception:
                pass
        ask = {'signature': own_signature, 'help': own_help, 'definition': own_definition}[kind]
        return '', {kind: ask(source, line, column, path, folders)}
    if jedi is not None:
        try:
            items = []
            for c in jedi.Script(code=source, path=path).complete(line, column):
                items.append({'name': c.name_with_symbols, 'type': c.type, 'description': c.description})
                if len(items) >= MOST:
                    break
            return ENGINE, {'items': items}
        except Exception:
            pass
    names, prefix = own(source, line, column, path)
    low = prefix.lower()
    chosen = sorted((n for n in names if n.lower().startswith(low)), key=lambda n: (n.startswith('_'), n.lower(), n))
    return '', {'items': [{'name': n, 'type': names[n], 'description': ''} for n in chosen[:MOST]]}

while True:
    raw = sys.stdin.readline()
    if not raw:
        break
    if not raw.strip():
        continue
    request = {}
    try:
        request = json.loads(raw)
        engine, found = answer(request)
        out = {'id': request.get('id', -1), 'kind': request.get('kind') or 'complete', 'engine': engine}
        out.update(found)
    except Exception as e:
        out = {'id': request.get('id', -1), 'kind': request.get('kind') or 'complete', 'engine': '', 'items': [], 'failure': str(e)}
    sys.stdout.write(json.dumps(out) + '\n')
    sys.stdout.flush()
