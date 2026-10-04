"""p3: Maximize Document against the ways out: a new document, a split, the last document closed, a project opened,
a second Maximize, a simulation's panel coming up; Main Dock shown afterwards each time?"""
import mcp
def main_dock(s):
    return next(a['shown'] for a in s.call('get_ui', {})['areas'] if a['area'] == 'dock:Main Dock')
def maximize(s):
    r = s.call('trigger_action', {'action': 'View > Panes > Maximize Document'})
    return s.last_error, str(r)[:120]
s = mcp.Server('p3')
s.call('new_document', {})
print('maximize:', maximize(s), 'main dock shown:', main_dock(s))
s.call('new_document', {})
print('new_document while maximized: main dock shown:', main_dock(s))
print('maximize again (toggle?):', maximize(s), 'shown:', main_dock(s))
maximize(s)
docs = s.call('list_documents', {})
print('docs:', str(docs)[:200])
r = s.call('move_to_pane', {'path': 'untitled', 'pane': 'new'}) if False else None
# close every document while maximized
for d in s.call('list_documents', {}).get('documents', []):
    s.call('close_document', {'path': d.get('path') or d.get('name'), 'unsaved': 'discard'})
print('all closed while maximized: main dock shown:', main_dock(s), '| docs:', str(s.call('list_documents', {}))[:120])
print('maximize with no document:', maximize(s), 'shown:', main_dock(s))
s.call('new_document', {})
maximize(s)
s.call('new_project', {'name': 'mx'})
s.call('open_project', {'name': 'mx'})
print('open_project while maximized: main dock shown:', main_dock(s))
s.close()
