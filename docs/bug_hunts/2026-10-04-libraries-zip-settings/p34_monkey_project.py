"""p34: the menu actions of p23 inside a project with a linked library source, its tab open (read-only), ASan."""
import mcp, re, time, os
mcp.APP = '<repo>/build-asan/qucs/qucs-s.app/Contents/MacOS/qucs-s'
import libsetup
skip = re.compile(open('p23_action_monkey.py').read().split("skip = re.compile(r'")[1].split("', re.I)")[0].replace("'\n                  r'", ''), re.I)
s, team, proj, made, saved = libsetup.setup('p34')
acts = [a['action'] for a in s.call('list_actions', {}) if not skip.search(a['action'])]
link = proj + '/Libraries/VaRes/vres.va'
before = open(team + '/VaRes/vres.va').read()
dead = None
for state in ('linked source', 'schematic'):
    for a in acts:
        try:
            s.call('open_document', {'path': link if state == 'linked source' else proj + '/top.sch'})
            r = s.call('trigger_action', {'action': a})
            if 'dialog' in str(r).lower(): s.call('set_dialog', {'press': 'Cancel'})
        except Exception as e:
            dead = (state, a, repr(e)[:300]); break
    if dead: break
err = open(s.root + '/server.err', errors='replace').read()
print('actions per state', len(acts), '| dead:', dead, '| sanitizer reports:', len(re.findall(r'ERROR: AddressSanitizer|runtime error:', err)))
print('library original unchanged:', open(team + '/VaRes/vres.va').read() == before, '| link still a link:', os.path.islink(link))
try: s.close()
except Exception: pass
