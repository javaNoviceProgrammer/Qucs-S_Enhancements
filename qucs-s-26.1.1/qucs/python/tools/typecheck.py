"""The type check of the Python editor of Qucs-S: mypy's or pyright's, when
the script's Python has one.

    python -c <this> auto|mypy|pyright <the script's path, or ''> <a cache folder>

the script on standard input, as it is in the editor (its path for the
tools' settings and the modules beside it). One JSON object on standard
output: {"tool": "mypy 1.13.0", "problems": [{"line", "column", "endLine",
"endColumn", "message", "code"}, ...]} - lines and columns from 1, the end
after the last character -, {"tool": ""} when there is no type checker, or
{"failure": why}.

mypy runs in this process (mypy.api; --shadow-file: the text given checked as
the file it is); pyright on a copy of the text in a folder of its own, its
settings those of the script's folder (pyrightconfig.json, pyproject.toml's
[tool.pyright]) and that folder on its path, for the modules beside it.
"""

import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

which = sys.argv[1] if len(sys.argv) > 1 else 'auto'
path = sys.argv[2] if len(sys.argv) > 2 else ''
cache = sys.argv[3] if len(sys.argv) > 3 else ''
source = sys.stdin.buffer.read()
out = {'tool': '', 'problems': []}


def has_module(name):
    try:
        return importlib.util.find_spec(name) is not None
    except Exception:
        return False


def mypy():
    from mypy import api
    from mypy.version import __version__ as version
    folder = tempfile.mkdtemp(prefix='qucs-mypy-')
    try:
        if path and os.path.isfile(path):
            shadow = os.path.join(folder, os.path.basename(path))
            with open(shadow, 'wb') as f:
                f.write(source)
            os.chdir(os.path.dirname(os.path.abspath(path)))   # (its project's settings)
            target, extra = os.path.abspath(path), ['--shadow-file', os.path.abspath(path), shadow]
        else:
            target = os.path.join(folder, os.path.basename(path) or 'script.py')
            with open(target, 'wb') as f:
                f.write(source)
            os.chdir(folder)
            extra = []
        arguments = ['--show-column-numbers', '--show-error-end', '--show-error-codes', '--no-error-summary', '--no-pretty',
                     '--hide-error-context', '--no-color-output', '--follow-imports=silent']
        if cache:
            arguments += ['--cache-dir', cache]
        report, errors, status = api.run(arguments + extra + [target])
        if status not in (0, 1):
            raise RuntimeError((errors or report).strip().split('\n')[-1] if (errors or report).strip() else 'mypy ended with %d' % status)
        line_of = re.compile(r'^(.*?):(\d+):(\d+):(?:(\d+):(\d+):)?\s*(error|warning|note):\s*(.*?)(?:\s+\[([\w-]+)\])?$')
        mine = {os.path.normcase(os.path.abspath(target))}
        for line in report.splitlines():
            m = line_of.match(line)
            if not m or os.path.normcase(os.path.abspath(m.group(1))) not in mine:
                continue
            row, column = int(m.group(2)), int(m.group(3))
            end_row = int(m.group(4)) if m.group(4) else 0
            end_column = int(m.group(5)) + 1 if m.group(5) else 0
            if m.group(6) == 'note':
                # (A note says more of the error before it, at its line.)
                if out['problems'] and out['problems'][-1]['line'] == row:
                    out['problems'][-1]['message'] += '\n' + m.group(7)
                continue
            out['problems'].append({'line': row, 'column': column, 'endLine': end_row, 'endColumn': end_column,
                                    'message': m.group(7), 'code': m.group(8) or ''})
        out['tool'] = 'mypy ' + version
    finally:
        shutil.rmtree(folder, ignore_errors=True)


def pyright_command():
    found = shutil.which('pyright')
    if found:
        return [found]
    if has_module('pyright'):
        return [sys.executable, '-m', 'pyright']
    return None


def pyright(command):
    folder = tempfile.mkdtemp(prefix='qucs-pyright-')
    try:
        home = os.path.dirname(os.path.abspath(path)) if path else ''
        settings = {}
        if home:
            try:
                with open(os.path.join(home, 'pyrightconfig.json'), encoding='utf-8') as f:
                    settings = json.load(f)
            except (OSError, ValueError):
                try:
                    import tomllib
                    with open(os.path.join(home, 'pyproject.toml'), 'rb') as f:
                        settings = tomllib.load(f).get('tool', {}).get('pyright', {})
                except Exception:
                    settings = {}
        for key in ('include', 'exclude', 'ignore', 'executionEnvironments', 'venvPath', 'venv'):
            settings.pop(key, None)
        name = os.path.basename(path) if path else 'script.py'
        settings['extraPaths'] = ([home] if home else []) + [os.path.join(home, p) if home else p
                                                             for p in settings.get('extraPaths', [])]
        settings['include'] = [name]
        with open(os.path.join(folder, 'pyrightconfig.json'), 'w', encoding='utf-8') as f:
            json.dump(settings, f)
        target = os.path.join(folder, name)
        with open(target, 'wb') as f:
            f.write(source)
        run = subprocess.run(command + ['--outputjson', '--pythonpath', sys.executable, '-p', folder, target],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=folder, timeout=120)
        try:
            report = json.loads(run.stdout.decode('utf-8', 'replace') or '{}')
        except ValueError:
            said = (run.stderr or run.stdout).decode('utf-8', 'replace').strip()
            raise RuntimeError(said.split('\n')[-1] if said else 'pyright ended with %d' % run.returncode)
        for d in report.get('generalDiagnostics', []):
            if d.get('severity') not in ('error', 'warning') or os.path.normcase(os.path.abspath(d.get('file', ''))) != \
                    os.path.normcase(os.path.abspath(target)):
                continue
            start, end = d.get('range', {}).get('start', {}), d.get('range', {}).get('end', {})
            out['problems'].append({'line': start.get('line', 0) + 1, 'column': start.get('character', 0) + 1,
                                    'endLine': end.get('line', 0) + 1, 'endColumn': end.get('character', 0) + 1,
                                    'message': d.get('message', ''), 'code': d.get('rule', '')})
        out['tool'] = ('pyright ' + str(report.get('version', ''))).strip()
    finally:
        shutil.rmtree(folder, ignore_errors=True)


try:
    command = pyright_command() if which in ('auto', 'pyright') else None
    if which in ('auto', 'mypy') and has_module('mypy'):
        mypy()
    elif command:
        pyright(command)
except Exception as e:
    out = {'failure': str(e) or type(e).__name__}
sys.stdout.write(json.dumps(out))
