"""The debugger of the Python editor of Qucs-S: a script run under bdb.

    python -u -c <this> script.py [its arguments]

The script's output, its standard output and error together, is this
program's standard output; what is typed for it comes as "input" commands
(its standard input a pipe of this program's). This program's own standard
input and error are the editor's: what to do, a JSON object a line, in;
where the script stopped and what is there, a JSON object a line, out.

    in   {"command": "start", "breakpoints": [b, ...], "raised": bool,
          "run_to": {"file": f, "line": n}, "library": bool}
         {"command": "continue" | "next" | "step" | "return"}
         {"command": "run_to", "file": f, "line": n}    on to there (once)
         {"command": "pause"}                          stopped where it is
         {"command": "frame", "index": k}              the variables of frame k
         {"command": "children", "handle": h}          a value's insides
         {"command": "evaluate", "expression": e, "frame": k}
         {"command": "inspect", "expression": e, "frame": k, "id": i}
         {"command": "data", "handle": h | "expression": e, "frame": k,
          "start": r, "count": n, "id": i}             a value's rows
         {"command": "breakpoints", "file": f, "lines": [b, ...]}   at any time
         {"command": "raised", "on": bool}             stopped where one is raised
         {"command": "library", "on": bool}            Python's library stepped into too
         {"command": "input", "text": t} | {"command": "eof"}   the script's input
    a breakpoint b: a line, or {"line": n, "condition": e, "hit": "5" | "== 5"
         | "> 5" | "% 5" ..., "log": "x is {x}", "enabled": bool}
    out  {"event": "stopped", "reason": "breakpoint" | "step" | "pause" |
          "raised" | "exception", "exception": "ZeroDivisionError: ...",
          "stack": [{"file", "line", "function", "library"}, ...] (the innermost
          first; library: Python's own, or a package's),
          "frame": 0, "variables": [...]}
         {"event": "running"}
         {"event": "variables", "frame": k, "variables": [...]}
         {"event": "children", "handle": h, "items": [...]}
         {"event": "evaluated", "expression": e, "value": {...}} or "error": text
         {"event": "inspected", "id": i, "expression": e, "value": {...}} or "error"
         {"event": "data", "id": i, ... _qucs_data.table()'s} or "error"
    a variable: {"name", "type", "value" (its repr, cut short), "handle" (-1:
    nothing inside), "table" (shown as one)}

It stops at a breakpoint - when its condition holds and its hits are as asked;
a logpoint writes its message (the {expressions} in it evaluated) and goes on
-, as it is stepped, in the code of the user - not in Python's library or a
package installed for it, stepped over, unless asked to ("library") -, where it is paused, where an
exception is raised when asked to, and where an exception no one catches is
raised, after its traceback, to be looked at.
"""

import signal
import sys
# (An Interrupt before the script runs is none of its: the debugger's start
# goes on - main() takes it again for the script.)
signal.signal(signal.SIGINT, signal.SIG_IGN)
# (Not the folder it runs in - the script's, where a json.py or a queue.py of
# its own would stand in for the modules this program imports.)
sys.path[:] = [p for p in sys.path if p not in ('', '.')]

import bdb
import io
import json
import linecache
import os
import queue
import re
import reprlib
import site
import sysconfig
import threading
import traceback
import types

try:
    import _qucs_data   # (beside the qucs module: the tables of the Data Viewer)
except ImportError:
    _qucs_data = None

# The editor's channels, kept; the script's output goes to its own, its input
# comes from a pipe of this program's ("input" commands write it).
_events = os.fdopen(os.dup(2), 'w', encoding='utf-8', buffering=1)
_commands = os.fdopen(os.dup(0), 'r', encoding='utf-8')
os.dup2(1, 2)
_input_read, _input = os.pipe()
os.dup2(_input_read, 0)
os.close(_input_read)
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


def no_source(path):
    """Code of no file to show (<frozen importlib._bootstrap>, <string>)."""
    return not path or path.startswith('<')


def is_library(path):
    """Python's library, a package installed for it, or no file at all."""
    if no_source(path):
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


HIT = re.compile(r'^\s*(==|>=|<=|>|<|%)?\s*(\d+)\s*$')


def hit_matches(condition, hits):
    """Whether the hits so far are as \a condition asks: 5 (from the fifth on),
    == 5, > 5, >= 5, < 5, <= 5, % 5 (every fifth). One it cannot read: yes."""
    m = HIT.match(condition or '')
    if not m:
        return True
    how, n = m.group(1) or '>=', int(m.group(2))
    return {'==': hits == n, '>=': hits >= n, '>': hits > n, '<': hits < n, '<=': hits <= n,
            '%': n > 0 and hits % n == 0}[how]


def interpolate(message, frame):
    """A logpoint's message, each {expression} in it evaluated in the frame
    ({{ and }} themselves)."""
    out, k, n = [], 0, len(message)
    while k < n:
        c = message[k]
        if c == '{' and message.startswith('{{', k):
            out.append('{')
            k += 2
        elif c == '}' and message.startswith('}}', k):
            out.append('}')
            k += 2
        elif c == '{':
            depth, e = 1, k + 1
            while e < n and depth:
                depth += {'{': 1, '}': -1}.get(message[e], 0)
                e += 1
            expression = message[k + 1:e - 1]
            try:
                out.append(str(eval(expression, frame.f_globals, frame.f_locals)))
            except Exception as error:
                out.append('<%s: %s>' % (type(error).__name__, error))
            k = e
        else:
            out.append(c)
            k += 1
    return ''.join(out)


class Debugger(bdb.Bdb):
    MOST = 300   # a value's insides shown

    def __init__(self, script):
        bdb.Bdb.__init__(self)
        self.script = self.canonic(script)
        self.started = False
        self.wanted = {}           # breakpoints as asked for: a file's, each {"line", "condition", ...}
        self.points = {}           # where they are: a file's lines with code, each its breakpoint
        self.temporary = None      # Run to Cursor's place: (file, line with code)
        self.raised = False        # stopped where an exception is raised
        self.library = False       # Python's library stepped into too
        self.pausing = False       # Pause asked for
        self.main_thread = threading.get_ident()
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

    def placed(self, path, line, code):
        """The line with code a breakpoint at \a line stops at."""
        if code:
            later = [n for n in code if n >= line]
            if later:
                return later[0]
        return line

    @staticmethod
    def spec(b):
        if isinstance(b, dict):
            return {'line': int(b.get('line') or 0), 'condition': (b.get('condition') or '').strip(),
                    'hit': (b.get('hit') or '').strip(), 'log': b.get('log') or '', 'enabled': b.get('enabled', True) is not False,
                    'hits': 0}
        return {'line': int(b), 'condition': '', 'hit': '', 'log': '', 'enabled': True, 'hits': 0}

    def set_breakpoints(self, path, breakpoints):
        path = self.canonic(path)
        self.clear_all_file_breaks(path)
        specs = [self.spec(b) for b in breakpoints]
        self.wanted[path] = specs
        linecache.checkcache(path)
        code = self.code_lines(path)
        points = {}
        for b in specs:
            at = self.placed(path, b['line'], code)
            points.setdefault(at, b)
            self.set_break(path, at)
        self.points[path] = points
        if self.temporary and self.temporary[0] == path:
            self.set_break(path, self.temporary[1])

    def run_to(self, path, line):
        path = self.canonic(path)
        linecache.checkcache(path)
        self.temporary = (path, self.placed(path, line, self.code_lines(path)))
        self.set_break(*self.temporary)

    def end_run_to(self):
        if self.temporary is None:
            return
        path, line = self.temporary
        self.temporary = None
        if line not in self.points.get(path, {}):
            self.clear_break(path, line)

    def fires(self, frame, path):
        """Whether a breakpoint here stops it: enabled, its condition holding,
        its hits as asked - a logpoint's message written, and on."""
        b = self.points.get(path, {}).get(frame.f_lineno)
        if b is None or not b['enabled']:
            return False
        if b['condition']:
            try:
                if not eval(b['condition'], frame.f_globals, frame.f_locals):
                    return False
            except Exception as e:   # (stopped there, to look at why)
                print('The condition of the breakpoint at %s, line %d, failed: %s'
                      % (os.path.basename(path), frame.f_lineno, ''.join(traceback.format_exception_only(type(e), e)).strip()),
                      flush=True)
                return True
        b['hits'] += 1
        if b['hit'] and not hit_matches(b['hit'], b['hits']):
            return False
        if b['log']:
            print(interpolate(b['log'], frame), flush=True)
            return False
        return True

    # -- tracing
    def set_continue(self):
        # (Traced still: a breakpoint set while it runs stops it.)
        self._set_stopinfo(self.botframe, None, -1)

    def skipped(self, path):
        """Code stepped over: Python's library (unless it is stepped into
        too), and code of no file."""
        return no_source(path) or (not self.library and is_library(path))

    def own(self, frame):
        name = frame.f_code.co_filename
        return name == '<string>' or os.path.realpath(name) == _OWN or frame.f_globals is globals()

    def dispatch_call(self, frame, arg):
        traced = bdb.Bdb.dispatch_call(self, frame, arg)
        # Asked to stop where an exception is raised, or to pause: the
        # user's every function traced (bdb leaves those of no breakpoint).
        if traced is None and (self.raised or self.pausing) and self.started and not self.quitting \
                and not self.own(frame) and not self.skipped(frame.f_code.co_filename):
            return self.trace_dispatch
        return traced

    def dispatch_exception(self, frame, arg):
        kind, value, tb = arg
        if (self.raised and self.started and not self.post_mortem and tb is not None and tb.tb_next is None
                and not self.skipped(frame.f_code.co_filename) and not self.own(frame)
                and not issubclass(kind, (StopIteration, StopAsyncIteration, GeneratorExit, bdb.BdbQuit))):
            said = ''.join(traceback.format_exception_only(kind, value)).strip()
            self.interaction(frame, 'raised', exception=said)
            if self.quitting:
                raise bdb.BdbQuit
        return bdb.Bdb.dispatch_exception(self, frame, arg)

    def user_call(self, frame, argument_list):
        pass

    def user_return(self, frame, return_value):
        pass

    def user_exception(self, frame, exc_info):
        pass

    def user_line(self, frame):
        path = self.canonic(frame.f_code.co_filename)
        if not self.started:   # (stepping until the script's first line: on from there)
            if path != self.script:
                return
            self.started = True
            if not (self.fires(frame, path) or self.temporary == (path, frame.f_lineno)):
                self.set_continue()
                return
            if self.temporary == (path, frame.f_lineno):
                self.end_run_to()
            self.interaction(frame, 'breakpoint')
            return
        stepping = self.stop_here(frame) or self.pausing
        there = self.temporary == (path, frame.f_lineno)
        hit = self.fires(frame, path) or there
        if there:
            self.end_run_to()
        if not hit:
            if not stepping:
                return   # (a breakpoint whose condition did not hold: on)
            if self.skipped(frame.f_code.co_filename):
                self.set_return(frame)   # (stepped over: on in the code that called it)
                return
        reason = 'breakpoint' if hit else ('pause' if self.pausing else 'step')
        self.interaction(frame, reason)

    def pause(self):
        """(From the thread of commands.) Stopped at the next line of the
        user's: every frame of the script's traced, its stack's too."""
        self.pausing = True
        self._set_stopinfo(None, None)   # (not set_step(): not "not on this line again")
        frame = sys._current_frames().get(self.main_thread)
        while frame is not None:
            if frame.f_trace is None and not self.own(frame) and not self.skipped(frame.f_code.co_filename):
                frame.f_trace = self.trace_dispatch
            frame = frame.f_back

    # -- a stop
    def stack_of(self, frame, tb=None):
        stack, _ = self.get_stack(frame, tb)
        frames = []
        for f, line in reversed(stack):
            if self.own(f):
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
        table = False
        if _qucs_data is not None:
            try:
                table = _qucs_data.is_table(value)
            except Exception:
                pass
        return {'name': name, 'type': kind_of(value), 'value': said, 'handle': handle, 'table': table}

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
        self.pausing = False
        # (After an exception: the traceback's frames, at the lines it has.)
        self.frames = self.stack_of(None, tb) if tb is not None else self.stack_of(frame)
        if not self.frames:
            return
        self.handles = []
        stack = [{'file': f.f_code.co_filename, 'line': line, 'function': f.f_code.co_name,
                  'library': is_library(f.f_code.co_filename)} for f, line in self.frames]
        send({'event': 'stopped', 'reason': reason, 'exception': exception, 'stack': stack, 'frame': 0,
              'variables': self.variables(0)})
        while True:
            c = self.queue.get()
            command = c.get('command')
            if command == 'quit':
                self.set_quit()
                break
            if command in ('continue', 'next', 'step', 'return', 'run_to'):
                if self.post_mortem:
                    pass
                elif command == 'continue':
                    self.set_continue()
                elif command == 'run_to':
                    self.run_to(c.get('file') or '', int(c.get('line') or 0))
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
                        items.append({'name': '...', 'type': '', 'value': '%d more' % (len(pairs) - self.MOST), 'handle': -1,
                                      'table': False})
                send({'event': 'children', 'handle': handle, 'items': items})
            elif command == 'evaluate':
                self.evaluate(c.get('expression') or '', int(c.get('frame', 0)))
            elif command == 'inspect':
                self.inspect(c)
            elif command == 'data':
                self.data(c)
        send({'event': 'running'})

    def value_of(self, c):
        """The value a command names: a handle's, or an expression's in a frame."""
        if 'handle' in c:
            handle = int(c.get('handle', -1))
            if not 0 <= handle < len(self.handles):
                raise LookupError('That value is no longer there.')
            return self.handles[handle]
        index = int(c.get('frame', 0))
        if not 0 <= index < len(self.frames):
            raise LookupError('No frame to evaluate it in.')
        frame = self.frames[index][0]
        return eval(compile(c.get('expression') or '', '<inspect>', 'eval'), frame.f_globals, frame.f_locals)

    def inspect(self, c):
        out = {'event': 'inspected', 'id': c.get('id'), 'expression': c.get('expression') or ''}
        try:
            value = self.value_of(c)
            try:
                said = repr(value)
            except Exception as e:
                said = '<repr failed: %s>' % e
            out['value'] = {'type': kind_of(value), 'value': said if len(said) <= 2000 else said[:2000] + '...'}
        except Exception as e:
            out['error'] = ''.join(traceback.format_exception_only(type(e), e)).strip()
        send(out)

    def data(self, c):
        out = {'event': 'data', 'id': c.get('id')}
        try:
            if _qucs_data is None:
                raise RuntimeError('The tables of the Data Viewer are not on the path (_qucs_data).')
            value = self.value_of(c)
            out.update(_qucs_data.table(value, c.get('start', 0), c.get('count', 1000)))
            out['type'] = _qucs_data.kind_of(value)
        except Exception as e:
            out['error'] = ''.join(traceback.format_exception_only(type(e), e)).strip()
        send(out)

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
    global _input
    for line in _commands:
        try:
            c = json.loads(line)
        except ValueError:
            continue
        command = c.get('command')
        if command == 'breakpoints':
            debugger.set_breakpoints(c.get('file') or '', c.get('lines') or [])
        elif command == 'raised':
            debugger.raised = bool(c.get('on'))
        elif command == 'library':
            debugger.library = bool(c.get('on'))
        elif command == 'pause':
            debugger.pause()
        elif command == 'input':
            if _input is not None:
                try:
                    os.write(_input, (c.get('text') or '').encode('utf-8'))
                except OSError:
                    pass
        elif command == 'eof':
            if _input is not None:
                os.close(_input)
                _input = None
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
        wanted.append(b)
    for path, specs in list(debugger.wanted.items()):
        debugger.set_breakpoints(path, specs)
    debugger.raised = bool(first.get('raised'))
    debugger.library = bool(first.get('library'))
    if isinstance(first.get('run_to'), dict):
        debugger.run_to(first['run_to'].get('file') or '', int(first['run_to'].get('line') or 0))
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
        signal.signal(signal.SIGINT, signal.default_int_handler)   # (Interrupt: a KeyboardInterrupt in the script)
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
