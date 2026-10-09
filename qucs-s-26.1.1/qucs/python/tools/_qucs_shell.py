"""Lines of a script run in the Python Shell of Qucs-S: Run Selection or Line,
Run Cell. The editor writes them to a file of its own - the script's path, the
line they begin on, its folder, the lines - and the shell is sent

    __import__('_qucs_shell').run(globals(), '<that file>')

They run in the shell's variables, in the script's folder, their tracebacks at
the script's lines; the value of the last of them, an expression, is shown as
the shell shows one.
"""

import ast
import json
import os
import sys
import textwrap
import traceback


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
