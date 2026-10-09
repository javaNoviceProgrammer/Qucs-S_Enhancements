# The Python Shell's start-up file (PYTHONSTARTUP), in Qucs-S: its variables
# shown in the Python Variables pane (_qucs_shell.install()), then the user's
# own start-up file, when there is one, as Python would have run it.
def _qucs_start():
    import os
    import _qucs_shell
    _qucs_shell.install()
    own = os.environ.get('QUCS_S_PYTHONSTARTUP', '')
    return own if own and os.path.isfile(own) else ''


_qucs_own = _qucs_start()
del _qucs_start
if _qucs_own:
    with open(_qucs_own, 'rb') as _qucs_file:
        exec(compile(_qucs_file.read(), _qucs_own, 'exec'))
    del _qucs_file
del _qucs_own
