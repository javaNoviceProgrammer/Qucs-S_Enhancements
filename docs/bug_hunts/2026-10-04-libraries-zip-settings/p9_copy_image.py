"""p9: Copy Circuit as Image on an empty schematic, one of a diagram alone, one of an analysis block alone."""
import mcp, json
s = mcp.Server('p9')
acts = s.call('list_actions', {'search': 'image'})
print('actions:', json.dumps(acts)[:600])
name = None
for a in (acts.get('actions', []) if isinstance(acts, dict) else []):
    t = a.get('action') if isinstance(a, dict) else a
    if t and 'Copy' in t and 'Image' in t: name = t
print('using', name)
s.call('new_document', {})
r = s.call('trigger_action', {'action': name}); print('empty:', s.last_error, str(r)[:200])
s.call('add_diagram', {'traces': [{'variable': 'v(x)'}]})
r = s.call('trigger_action', {'action': name}); print('diagram only:', s.last_error, str(r)[:200])
s.call('new_document', {})
s.call('add_analysis', {'kind': 'tran', 'stop': '1 ms'})
r = s.call('trigger_action', {'action': name}); print('analysis only:', s.last_error, str(r)[:200])
print('alive:', 'workspace' in s.call('get_state', {}))
s.close()
