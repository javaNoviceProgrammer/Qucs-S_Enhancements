"""Formats a script, or fixes what ruff can fix in it - the Python editor of
Qucs-S, Format Document and Fix Problems:

    python -c <this> format|fix <the script's path>

the script on standard input (its path for the tools' settings); one JSON
object on standard output: {"tool": "ruff 0.6.9", "text": the script after}
or {"failure": why}. Formatting is ruff's (ruff format), else black's; fixing
is ruff's (ruff check --fix) alone.
"""

import importlib.util
import json
import os
import shutil
import subprocess
import sys

mode = sys.argv[1] if len(sys.argv) > 1 else 'format'
name = sys.argv[2] if len(sys.argv) > 2 else 'script.py'
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
else:
    black = None if ruff else tool('black')
    used = ruff or black
    if ruff:
        command = ruff + ['format', '--no-cache', '--stdin-filename', name, '-']
    elif black:
        command = black + ['-q', '--stdin-filename', name, '-']
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
        if run.returncode != 0:   # (a syntax error, most often)
            said = run.stderr.decode('utf-8', 'replace').strip()
            out['failure'] = said.split('\n')[-1] if said else 'It ended with exit code %d.' % run.returncode
        else:
            out['tool'] = version(used) or used[-1]
            out['text'] = run.stdout.decode('utf-8', 'replace')
    except Exception as e:
        out['failure'] = str(e)
sys.stdout.write(json.dumps(out))
