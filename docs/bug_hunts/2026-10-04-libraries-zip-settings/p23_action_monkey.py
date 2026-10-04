"""p23: every menu action (but quitting, printing, consoles, simulation and anything run outside) in four states: no
document, a schematic, a text document, a ZIP archive open. A dialog is cancelled. Never a crash or a stuck server."""
import mcp, json, re, time, zipfile, os
mcp.APP = '<repo>/build-asan/qucs/qucs-s.app/Contents/MacOS/qucs-s'
skip = re.compile(r'Quit|Exit|Print|Simulat|Octave|Terminal|Python|Console|Shell|Run|Script|Calculat|Help|About|Report|Update|'
                  r'Web|Online|Tutorial|Technical|Book|Open Recent|Clear Recent|Restart|Settings|Preferences|Netlist|Build|Compile|'
                  r'Optim|Tune|Export|Import|Save|Open|Attenuator|Filter|Line|Matching|Power|Transcalc|Spar|Library Manager|Create Library|'
                  r'Claude|Git|Commit|Push|Pull|Unlink|Delete Project|Close Project|Archive|Zip|Diff', re.I)
s = mcp.Server('p23')
acts = [a['action'] for a in s.call('list_actions', {}) if not skip.search(a['action'])]
print('actions tried per state:', len(acts))
z = s.ws + '/a.zip'
with zipfile.ZipFile(z, 'w') as f: f.writestr('x.txt', 'hello'); f.writestr('d/y.sch', '<Qucs Schematic 26.1.5>\n')
open(s.ws + '/t.txt', 'w').write('text\n')
s.call('import_netlist', {'text': 't\nR1 a b 1k\nV1 a 0 1\n.op\n.end', 'save_as': s.ws + '/a.sch'})
states = {'no document': lambda: [s.call('close_document', {'unsaved': 'discard'}) for _ in range(4)],
          'schematic': lambda: s.call('open_document', {'path': s.ws + '/a.sch'}),
          'text': lambda: s.call('open_document', {'path': s.ws + '/t.txt'}),
          'zip': lambda: s.call('open_document', {'path': z})}
slow, dead = [], None
for state, enter in states.items():
    for a in acts:
        try:
            enter()
            t = time.time()
            r = s.call('trigger_action', {'action': a})
            if time.time() - t > 5: slow.append((state, a, round(time.time() - t, 1)))
            if 'dialog' in str(r).lower(): s.call('set_dialog', {'press': 'Cancel'})
        except Exception as e:
            dead = (state, a, repr(e)[:300]); break
    if dead: break
err = open(s.root + '/server.err', errors='replace').read()
print('dead:', dead); print('slow (>5 s):', slow)
print('sanitizer reports:', len(re.findall(r'ERROR: AddressSanitizer|runtime error:', err)))
for l in re.findall(r'.*(?:ERROR: AddressSanitizer|runtime error:).*', err)[:6]: print('  ', l[:300])
try: s.close()
except Exception: pass
