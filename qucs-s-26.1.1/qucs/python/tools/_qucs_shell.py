"""The Python Shell of Qucs-S: lines of a script run in it, its variables shown.

Run Selection or Line, Run Cell: the editor writes the lines to a file of its
own - the script's path, the line they begin on, its folder, the lines - and
the shell is sent

    __import__('_qucs_shell').run(globals(), '<that file>')

They run in the shell's variables, in the script's folder, their tracebacks at
the script's lines; the value of the last of them, an expression, is shown as
the shell shows one; the figures they leave open are shown (Python Plots).

install() - the shell's start-up file runs it - writes the shell's variables
into the folder QUCS_S_SHELL names (variables.json, a line about each:
_qucs_data.summary()) before each prompt, and answers what is asked of them
there: requests/<n>.json, {"kind": "table", "expression", "start", "count"}
or {"kind": "variables"}, answered in answers/<n>.json.
"""

import ast
import json
import os
import sys
import textwrap
import threading
import time
import traceback
import types

FOLDER = os.environ.get('QUCS_S_SHELL', '')
MOST = 1000   # variables listed


def run(namespace, job):
    with open(job, encoding='utf-8') as f:
        work = json.load(f)
    try:
        os.remove(job)
    except OSError:
        pass
    path = work.get('file') or '<selection>'
    first = int(work.get('line') or 1)
    folder = work.get('folder') or ''
    if folder and os.path.isdir(folder):
        os.chdir(folder)
        if folder not in sys.path:
            sys.path.insert(0, folder)
    if work.get('file'):
        namespace['__file__'] = path
    source = textwrap.dedent(work.get('code') or '')
    try:
        try:
            tree = ast.parse(source, path, 'exec')
        except SyntaxError as e:   # (at the script's line, not the selection's)
            if e.lineno:
                e.lineno += first - 1
            if getattr(e, 'end_lineno', None):
                e.end_lineno += first - 1
            raise
        ast.increment_lineno(tree, first - 1)
        last = None
        if tree.body and isinstance(tree.body[-1], ast.Expr):
            last = ast.Expression(tree.body.pop().value)
        exec(compile(tree, path, 'exec'), namespace)
        if last is not None:
            value = eval(compile(last, path, 'eval'), namespace)
            if value is not None:
                sys.displayhook(value)
    except SystemExit:
        raise
    except BaseException as e:   # (as the shell says one: without this file's frames)
        tb = e.__traceback__
        while tb is not None and tb.tb_frame.f_code.co_filename == __file__:
            tb = tb.tb_next
        traceback.print_exception(type(e), e, tb)
    plots = sys.modules.get('_qucs_plots')
    if plots is not None:
        plots.flush()


# ----------------------------------------------------------------------
# The shell's variables

def _shown(name, value):
    """A variable of the shell's to list: not a module, a function, a class -
    nor a name of Python's (_, __builtins__)."""
    if name.startswith('_') or name in ('In', 'Out', 'exit', 'quit'):
        return False
    return not isinstance(value, (types.ModuleType, types.FunctionType, types.BuiltinFunctionType, types.MethodType, type))


def variables():
    import _qucs_data
    main = sys.modules.get('__main__')
    names = dict(vars(main)) if main is not None else {}
    out = []
    for name in sorted(names, key=lambda n: (n.lower(), n)):
        if len(out) >= MOST:
            break
        value = names[name]
        if _shown(name, value):
            out.append(_qucs_data.summary(name, value))
    return out


def _write(path, data):
    part = path + '.part'
    with open(part, 'w', encoding='utf-8') as f:
        json.dump(data, f)
    os.replace(part, path)


class _Prompt:
    """sys.ps1: the variables written each time it is shown."""

    def __init__(self, text):
        self.text = str(text)
        self.last = None

    def __str__(self):
        try:
            listed = variables()
            said = json.dumps(listed)
            if said != self.last:
                _write(os.path.join(FOLDER, 'variables.json'), {'time': time.time(), 'variables': listed})
                self.last = said
        except Exception:
            pass
        return self.text


def _answer(request):
    import _qucs_data
    kind = request.get('kind')
    if kind == 'variables':
        return {'kind': kind, 'variables': variables()}
    if kind == 'table':
        expression = request.get('expression') or ''
        main = sys.modules.get('__main__')
        try:
            value = eval(compile(expression, '<variables>', 'eval'), vars(main) if main is not None else {})
        except Exception as e:
            return {'kind': kind, 'expression': expression,
                    'error': ''.join(traceback.format_exception_only(type(e), e)).strip()}
        found = _qucs_data.table(value, request.get('start', 0), request.get('count', 1000))
        found.update({'kind': kind, 'expression': expression, 'type': _qucs_data.kind_of(value)})
        return found
    return {'kind': kind, 'error': 'Not a request: %r' % kind}


def _serve():
    requests = os.path.join(FOLDER, 'requests')
    answers = os.path.join(FOLDER, 'answers')
    while True:
        time.sleep(0.1)
        try:
            names = sorted(n for n in os.listdir(requests) if n.endswith('.json'))
        except OSError:
            continue
        for name in names:
            path = os.path.join(requests, name)
            try:
                with open(path, encoding='utf-8') as f:
                    request = json.load(f)
                os.remove(path)
            except (OSError, ValueError):
                continue
            try:
                answer = _answer(request)
            except Exception as e:
                answer = {'kind': request.get('kind'), 'error': str(e)}
            answer['id'] = request.get('id')
            try:
                _write(os.path.join(answers, name), answer)
            except OSError:
                pass


def install():
    """The shell's variables shown and served (its start-up file's)."""
    if not FOLDER or not os.path.isdir(FOLDER):
        return
    for sub in ('requests', 'answers'):
        os.makedirs(os.path.join(FOLDER, sub), exist_ok=True)
    sys.ps1 = _Prompt(getattr(sys, 'ps1', '>>> '))
    threading.Thread(target=_serve, name='qucs-variables', daemon=True).start()
