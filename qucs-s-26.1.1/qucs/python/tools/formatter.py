"""Formats a script, fixes what ruff can fix in it, or sorts its imports - the
Python editor of Qucs-S, Format Document, Format Selection, Fix Problems and
Organize Imports:

    python -c <this> format|fix|imports <the script's path> [first-last]

the script on standard input (its path for the tools' settings); one JSON
object on standard output: {"tool": "ruff 0.6.9", "text": the script after}
or {"failure": why}. Formatting is ruff's (ruff format), else black's - with
lines first-last (from 1), those lines alone (ruff --range, black
--line-ranges); fixing is ruff's (ruff check --fix) alone; the imports are
sorted by ruff (its isort rules: ruff check --select I --fix), else isort.
"""

import importlib.util
import json
import os
import shutil
import subprocess
import sys

mode = sys.argv[1] if len(sys.argv) > 1 else 'format'
name = sys.argv[2] if len(sys.argv) > 2 else 'script.py'
lines = sys.argv[3] if len(sys.argv) > 3 else ''
source = sys.stdin.buffer.read()


def tool(module):
    try:
        if importlib.util.find_spec(module) is not None:
            return [sys.executable, '-m', module]
    except Exception:
        pass
    found = shutil.which(module)
    return [found] if found else None


def version(command):
    try:
        run = subprocess.run(command + ['--version'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=20)
        return run.stdout.decode('utf-8', 'replace').strip().split('\n')[0]
    except Exception:
        return ''


out = {}
ruff = tool('ruff')
if mode == 'fix':
    used = ruff
    command = ruff + ['check', '--fix', '--exit-zero', '--quiet', '--no-cache', '--stdin-filename', name, '-'] if ruff else None
    missing = 'Fix Problems needs ruff, installed for this Python (pip install ruff).'
elif mode == 'imports':
    isort = None if ruff else tool('isort')
    used = ruff or isort
    if ruff:
        command = ruff + ['check', '--select', 'I', '--fix', '--exit-zero', '--quiet', '--no-cache', '--stdin-filename', name, '-']
    elif isort:
        command = isort + ['--filename', name, '-']
    else:
        command = None
    missing = 'Organize Imports needs ruff or isort, installed for this Python (pip install ruff).'
else:
    black = None if ruff else tool('black')
    used = ruff or black
    if ruff:
        command = ruff + ['format', '--no-cache', '--stdin-filename', name] + (['--range', lines] if lines else []) + ['-']
    elif black:
        command = black + ['-q', '--stdin-filename', name] + (['--line-ranges', lines] if lines else []) + ['-']
    else:
        command = None
    missing = 'Format Document needs ruff or black, installed for this Python (pip install ruff).'
if command is None:
    out['failure'] = missing
else:
    try:
        folder = os.path.dirname(os.path.abspath(name))   # (its project's settings)
        run = subprocess.run(command, input=source, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60,
                             cwd=folder if os.path.isdir(folder) else None)
        if run.returncode != 0:   # (a syntax error, most often - or a tool too old for a range)
            said = run.stderr.decode('utf-8', 'replace').strip()
            out['failure'] = said.split('\n')[-1] if said else 'It ended with exit code %d.' % run.returncode
        else:
            out['tool'] = version(used) or used[-1]
            out['text'] = run.stdout.decode('utf-8', 'replace')
    except Exception as e:
        out['failure'] = str(e)
sys.stdout.write(json.dumps(out))
