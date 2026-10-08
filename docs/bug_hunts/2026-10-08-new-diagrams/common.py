"""Helpers of the probes: an RC example copied into a project and run with ngspice."""
import os, json, shutil, mcp
EX = mcp.REPO + '/qucs-s-26.1.1/examples'


def rc(s, name='rc', ac=False):
    """RC_filter_FFT (a 1 kHz square into 100k/10n) in project NAME, opened and simulated; returns its path."""
    s.call('new_project', {'name': name})
    d = s.ws + f'/{name}_prj'
    p = d + '/rc.sch'
    shutil.copy(EX + '/ngspice/General Electronics/RC_filter_FFT.sch', p)
    s.call('open_project', {'name': name})
    s.call('open_document', {'path': p})
    if ac:
        s.call('add_analysis', {'kind': 'ac', 'properties': {'Start': '10 Hz', 'Stop': '1 MHz', 'Points': '101'}})
    s.call('save_document', {})
    sim = s.call('simulate', {'simulator': 'ngspice'})
    return p, sim


def show(label, o, n=1500):
    print(label, (json.dumps(o) if not isinstance(o, str) else o)[:n], flush=True)
