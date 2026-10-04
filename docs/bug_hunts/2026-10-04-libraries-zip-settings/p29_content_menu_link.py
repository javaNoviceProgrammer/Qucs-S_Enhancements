"""p29: the Content panel's menu on a linked library source: its entries; Duplicate, Delete - the original untouched?"""
import os, json, libsetup
s, team, proj, made, saved = libsetup.setup('p29')
orig = team + '/VaRes/vres.va'; before = open(orig).read()
m = s.call('context_menu', {'on': {'project_item': 'Libraries/VaRes/vres.va'}})
print('menu:', json.dumps(m)[:400])
for entry in ('Duplicate', 'Delete'):
    r = s.call('context_menu', {'on': {'project_item': 'Libraries/VaRes/vres.va'}, 'choose': entry})
    d = s.call('get_dialog', {})
    if isinstance(d, dict) and d.get('controls'):
        print(entry, 'dialog:', [c.get('label') for c in d['controls']][:8], str(d.get('text', d.get('title', '')))[:200])
        s.call('set_dialog', {'press': 'Yes'}) if entry == 'Delete' else s.call('set_dialog', {'press': 'OK'})
    print(entry, '->', s.last_error, str(r)[:160])
    print('   original intact:', os.path.isfile(orig) and open(orig).read() == before, '| link:', os.path.islink(proj + '/Libraries/VaRes/vres.va'),
          '| folder:', sorted(os.listdir(proj + '/Libraries/VaRes')) if os.path.isdir(proj + '/Libraries/VaRes') else None,
          '| trash:', os.listdir(s.root + '/trash'))
s.close()
