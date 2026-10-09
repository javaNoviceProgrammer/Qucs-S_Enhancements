"""The debugger of the Python editor of Qucs-S: a script run under bdb.

    python -u -c <this> script.py [its arguments]

The script's output, its standard output and error together, is this
program's standard output, and nothing is on its standard input (input()
reads the end of the file, as Run has it). This program's own standard input
and error are the editor's: what to do, a JSON object a line, in; where the
script stopped and what is there, a JSON object a line, out.

    in   {"command": "start", "breakpoints": [{"file": f, "line": n}, ...]}
         {"command": "continue" | "next" | "step" | "return"}
         {"command": "frame", "index": k}             the variables of frame k
         {"command": "children", "handle": h}         a value's insides
         {"command": "evaluate", "expression": e, "frame": k}
         {"command": "breakpoints", "file": f, "lines": [n, ...]}   at any time
    out  {"event": "stopped", "reason": "breakpoint" | "step" | "exception",
          "exception": "ZeroDivisionError: ...", "stack": [{"file", "line",
          "function"}, ...] (the innermost first), "frame": 0, "variables": [...]}
         {"event": "running"}
         {"event": "variables", "frame": k, "variables": [...]}
         {"event": "children", "handle": h, "items": [...]}
         {"event": "evaluated", "expression": e, "value": {...}} or "error": text
    a variable: {"name", "type", "value" (its repr, cut short), "handle" (-1:
    nothing inside)}

It stops at a breakpoint, and as it is stepped, in the code of the user - not
in Python's library or a package installed for it, stepped over - and, where
an exception no one catches is raised, after its traceback, to be looked at.
"""

import sys
# (Not the folder it runs in - the script's, where a json.py or a queue.py of
# its own would stand in for the modules this program imports.)
sys.path[:] = [p for p in sys.path if p not in ('', '.')]

import bdb
import io
import json
import linecache
import os
import queue
import reprlib
import site
import sysconfig
import threading
import traceback
import types

# The editor's channels, kept; the script's output goes to its own.
_events = os.fdopen(os.dup(2), 'w', encoding='utf-8', buffering=1)
_commands = os.fdopen(os.dup(0), 'r', encoding='utf-8')
os.dup2(1, 2)
_null = os.open(os.devnull, os.O_RDONLY)
os.dup2(_null, 0)
os.close(_null)
_lock = threading.Lock()


def send(event):
    with _lock:
        _events.write(json.dumps(event) + '\n')
        _events.flush()


_LIBRARY = set()
for key in ('stdlib', 'platstdlib', 'purelib', 'platlib'):
    try:
        _LIBRARY.add(os.path.realpath(sysconfig.get_paths()[key]))
    except (KeyError, OSError):
        pass
try:
    _LIBRARY.update(os.path.realpath(p) for p in site.getsitepackages())
except (AttributeError, OSError):
    pass
_OWN = os.path.realpath(bdb.__file__)


def is_library(path):
    """Python's library, a package installed for it, or no file at all."""
    if not path or path.startswith('<'):
        return True
    real = os.path.realpath(path)
    return any(real == p or real.startswith(p + os.sep) for p in _LIBRARY)


_short = reprlib.Repr()
_short.maxstring = 200
_short.maxother = 200
_short.maxlist = _short.maxtuple = _short.maxdict = _short.maxset = 20
_short.maxlevel = 3


def kind_of(value):
    name = type(value).__name__
    shape = getattr(value, 'shape', None)
    if isinstance(shape, tuple) and shape and hasattr(value, 'dtype'):
        return '%s %s %s' % (name, shape, value.dtype)
    if isinstance(value, (list, tuple, dict, set, frozenset, str, bytes)):
        return '%s (%d)' % (name, len(value))
    return name


def has_inside(value):
    """Whether a value has insides to show (cheaply: not made to know)."""
    if isinstance(value, (str, bytes, int, float, complex, bool, type(None), types.ModuleType, types.FunctionType,
                          types.BuiltinFunctionType, type)):
        return False
    if isinstance(value, (dict, list, tuple, set, frozenset)):
        return len(value) > 0
    shape = getattr(value, 'shape', None)
    if isinstance(shape, tuple):
        return len(shape) >= 1 and shape[0] > 0
    try:
        return any(not k.startswith('__') for k in vars(value))
    except TypeError:
        return False


def inside(value):
    """A value's insides: (name, value) pairs, or None when it has none."""
    if not has_inside(value):
        return None
    if isinstance(value, dict):
        return [(repr(k), v) for k, v in value.items()]
    if isinstance(value, (list, tuple)):
        return [('[%d]' % i, v) for i, v in enumerate(value)]
    if isinstance(value, (set, frozenset)):
        return [('', v) for v in value]
    shape = getattr(value, 'shape', None)
    if isinstance(shape, tuple) and len(shape) >= 1 and hasattr(value, '__getitem__'):
        try:
            return [('[%d]' % i, value[i]) for i in range(min(shape[0], 1000))]
        except Exception:
            return None
    if isinstance(value, (types.ModuleType, types.FunctionType, types.BuiltinFunctionType, type)):
        return None
    try:
        attributes = vars(value)
    except TypeError:
        return None
    return sorted(((k, v) for k, v in attributes.items() if not k.startswith('__')), key=lambda kv: kv[0].lower()) or None


class Debugger(bdb.Bdb):
    MOST = 300   # a value's insides shown

    def __init__(self, script):
        bdb.Bdb.__init__(self)
        self.script = self.canonic(script)
        self.started = False
        self.wanted = {}           # breakpoints as asked for: a file's lines
        self.frames = []           # the stack at the stop, the innermost first
        self.handles = []          # the values whose insides were asked for at this stop
        self.queue = queue.Queue()
        self.post_mortem = False

    # -- breakpoints: on the line asked for, or the next line with code
    def code_lines(self, path):
        try:
            source = linecache.getlines(path)
            code = compile(''.join(source), path, 'exec')
        except (SyntaxError, ValueError, TypeError):
            return None
        lines = set()
        todo = [code]
        while todo:
            c = todo.pop()
            try:
                lines.update(line for _, _, line in c.co_lines() if line)
            except AttributeError:   # (before Python 3.10)
                import dis
                lines.update(line for _, line in dis.findlinestarts(c))
            todo.extend(k for k in c.co_consts if isinstance(k, types.CodeType))
        return sorted(lines)

    def set_breakpoints(self, path, lines):
        path = self.canonic(path)
        self.clear_all_file_breaks(path)
        self.wanted[path] = list(lines)
        linecache.checkcache(path)
        code = self.code_lines(path)
        for line in lines:
            at = line
            if code:
                later = [n for n in code if n >= line]
                if later:
                    at = later[0]
            self.set_break(path, at)

    # -- tracing
    def set_continue(self):
        # (Traced still: a breakpoint set while it runs stops it.)
        self._set_stopinfo(self.botframe, None, -1)

    def user_call(self, frame, argument_list):
        pass

    def user_return(self, frame, return_value):
        pass

    def user_exception(self, frame, exc_info):
        pass

    def user_line(self, frame):
        path = self.canonic(frame.f_code.co_filename)
        here = frame.f_lineno in self.breaks.get(path, ())
        if not self.started:   # (stepping until the script's first line: on from there)
            if path != self.script:
                return
            self.started = True
            if not here:
                self.set_continue()
                return
        if not here and is_library(frame.f_code.co_filename):
            self.set_return(frame)   # (stepped over: on in the code that called it)
            return
        self.interaction(frame, 'breakpoint' if here else 'step')

    # -- a stop
    def stack_of(self, frame, tb=None):
        stack, _ = self.get_stack(frame, tb)
        frames = []
        for f, line in reversed(stack):
            name = f.f_code.co_filename
            if name == '<string>' or os.path.realpath(name) == _OWN or f.f_globals is globals():
                continue
            frames.append((f, line))
        return frames

    def describe(self, name, value):
        handle = -1
        if has_inside(value):
            handle = len(self.handles)
            self.handles.append(value)
        try:
            said = _short.repr(value)
        except Exception as e:
            said = '<repr failed: %s>' % e
        return {'name': name, 'type': kind_of(value), 'value': said, 'handle': handle}

    def variables(self, index):
        if not 0 <= index < len(self.frames):
            return []
        frame = self.frames[index][0]
        top = frame.f_code.co_name == '<module>'
        out = []
        for name, value in sorted(frame.f_locals.items(), key=lambda kv: kv[0].lower()):
            if name.startswith('__') and name.endswith('__'):
                continue
            if top and isinstance(value, (types.ModuleType, types.FunctionType, types.BuiltinFunctionType, type)):
                continue
            out.append(self.describe(name, value))
        return out

    def interaction(self, frame, reason, tb=None, exception=None):
        # (After an exception: the traceback's frames, at the lines it has.)
        self.frames = self.stack_of(None, tb) if tb is not None else self.stack_of(frame)
        if not self.frames:
            return
        self.handles = []
        stack = [{'file': f.f_code.co_filename, 'line': line, 'function': f.f_code.co_name} for f, line in self.frames]
        send({'event': 'stopped', 'reason': reason, 'exception': exception, 'stack': stack, 'frame': 0,
              'variables': self.variables(0)})
        while True:
            c = self.queue.get()
            command = c.get('command')
            if command == 'quit':
                self.set_quit()
                break
            if command in ('continue', 'next', 'step', 'return'):
                if self.post_mortem:
                    pass
                elif command == 'continue':
                    self.set_continue()
                elif command == 'next':
                    self.set_next(frame)
                elif command == 'step':
                    self.set_step()
                else:
                    self.set_return(frame)
                break
            if command == 'frame':
                index = int(c.get('index', 0))
                self.handles = []
                send({'event': 'variables', 'frame': index, 'variables': self.variables(index)})
            elif command == 'children':
                handle = int(c.get('handle', -1))
                items = []
                if 0 <= handle < len(self.handles):
                    pairs = inside(self.handles[handle]) or []
                    items = [self.describe(n, v) for n, v in pairs[:self.MOST]]
                    if len(pairs) > self.MOST:
                        items.append({'name': '...', 'type': '', 'value': '%d more' % (len(pairs) - self.MOST), 'handle': -1})
                send({'event': 'children', 'handle': handle, 'items': items})
            elif command == 'evaluate':
                self.evaluate(c.get('expression') or '', int(c.get('frame', 0)))
        send({'event': 'running'})

    def evaluate(self, expression, index):
        out = {'event': 'evaluated', 'expression': expression}
        if 0 <= index < len(self.frames):
            frame = self.frames[index][0]
            try:
                try:
                    code = compile(expression, '<evaluate>', 'eval')
                except SyntaxError:
                    exec(compile(expression, '<evaluate>', 'exec'), frame.f_globals, frame.f_locals)
                    out['value'] = None
                else:
                    out['value'] = self.describe(expression, eval(code, frame.f_globals, frame.f_locals))
            except Exception as e:
                out['error'] = ''.join(traceback.format_exception_only(type(e), e)).strip()
        else:
            out['error'] = 'No frame to evaluate it in.'
        send(out)
        send({'event': 'variables', 'frame': index, 'variables': self.variables(index)})


def read_commands(debugger):
    for line in _commands:
        try:
            c = json.loads(line)
        except ValueError:
            continue
        if c.get('command') == 'breakpoints':
            debugger.set_breakpoints(c.get('file') or '', [int(n) for n in c.get('lines') or []])
        else:
            debugger.queue.put(c)
    os._exit(1)   # (the editor is gone)


def main():
    script = os.path.abspath(sys.argv[1])
    sys.argv = [script] + sys.argv[2:]
    sys.path.insert(0, os.path.dirname(script))   # (as python script.py has it)
    debugger = Debugger(script)
    first = json.loads(_commands.readline() or '{}')
    for b in first.get('breakpoints') or []:
        wanted = debugger.wanted.setdefault(debugger.canonic(b.get('file') or ''), [])
        wanted.append(int(b.get('line') or 0))
    for path, lines in list(debugger.wanted.items()):
        debugger.set_breakpoints(path, lines)
    threading.Thread(target=read_commands, args=(debugger,), daemon=True).start()

    # The script as __main__, in a module of its own (this program's names
    # are not its).
    main_module = types.ModuleType('__main__')
    main_module.__file__ = script
    main_module.__builtins__ = __builtins__
    main_module.__spec__ = None
    sys.modules['__main__'] = main_module
    status = 0
    try:
        with io.open_code(script) as f:
            code = compile(f.read(), script, 'exec')
        debugger.run(code, main_module.__dict__)
    except bdb.BdbQuit:
        status = 1
    except SystemExit as e:
        if e.code is None:
            status = 0
        elif isinstance(e.code, int):
            status = e.code
        else:
            print(e.code, file=sys.stderr)
            status = 1
    except BaseException as e:
        sys.settrace(None)
        tb = e.__traceback__
        while tb is not None and (tb.tb_frame.f_code.co_filename == '<string>' or tb.tb_frame.f_globals is globals()
                                  or os.path.realpath(tb.tb_frame.f_code.co_filename) == _OWN):
            tb = tb.tb_next
        traceback.print_exception(type(e), e, tb)
        sys.stderr.flush()
        # Where it was raised, to be looked at.
        last = tb
        while last is not None and last.tb_next is not None:
            last = last.tb_next
        if last is not None:
            debugger.post_mortem = True
            said = ''.join(traceback.format_exception_only(type(e), e)).strip()
            debugger.interaction(last.tb_frame, 'exception', tb, said)
        status = 1
    sys.stdout.flush()
    os._exit(status)


main()
