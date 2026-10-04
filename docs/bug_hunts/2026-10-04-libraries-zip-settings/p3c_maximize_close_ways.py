"""p3c: one of three documents closed while maximized: File > Close (the GUI's) against close_document."""
import mcp
def main_dock(s):
    return next(a['shown'] for a in s.call('get_ui', {})['areas'] if a['area'] == 'dock:Main Dock')
for way in ('trigger_action File > Close', 'close_document', 'get_state only', 'get_schematic only'):
    s = mcp.Server('p3c')
    s.call('new_document', {}); s.call('new_document', {})
    s.call('trigger_action', {'action': 'View > Panes > Maximize Document'})
    shown0 = main_dock(s)
    if way.startswith('trigger'): r = s.call('trigger_action', {'action': 'File > Close'})
    elif way == 'close_document': r = s.call('close_document', {'unsaved': 'discard'})
    elif way == 'get_state only': r = s.call('get_state', {})
    else: r = s.call('get_schematic', {})
    print(f'{way}: maximized before={not shown0} error={s.last_error} -> main dock shown after={main_dock(s)} docs={len(s.call("get_state", {})["documents"])}')
    s.close()
