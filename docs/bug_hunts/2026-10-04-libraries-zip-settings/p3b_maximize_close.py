"""p3b: the last documents closed while maximized; a simulation's panel while maximized."""
import mcp, json
def main_dock(s):
    return next(a['shown'] for a in s.call('get_ui', {})['areas'] if a['area'] == 'dock:Main Dock')
s = mcp.Server('p3b')
st = s.call('get_state', {})
print('state keys:', list(st.keys())[:20])
s.call('new_document', {}); s.call('new_document', {})
s.call('trigger_action', {'action': 'View > Panes > Maximize Document'})
for i in range(3):
    r = s.call('close_document', {'unsaved': 'discard'})
    print('close', i, s.last_error, str(r)[:90], '| main dock shown:', main_dock(s))
st = s.call('get_state', {})
print('open docs now:', json.dumps(st.get('documents', st.get('open', '?')))[:200])
s.close()
