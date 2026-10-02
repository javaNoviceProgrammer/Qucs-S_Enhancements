import sys, json, os
sys.path.insert(0, '/private/tmp/claude-501/-Users-meisam-git-Qucs-S-Enhancements/ff1f864c-ed6a-4c31-a788-5998e059b547/scratchpad/repro')
from client import Server
APP = '/Users/meisam/git/Qucs-S_Enhancements/build/qucs/qucs-s.app/Contents/MacOS/qucs-s'
REPO = '/Users/meisam/git/Qucs-S_Enhancements'
def sch(components, wires=''):
    return ('<Qucs Schematic 26.1.5>\n<Properties>\n</Properties>\n<Symbol>\n</Symbol>\n<Components>\n' + components +
            '</Components>\n<Wires>\n' + wires + '</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n')
class Probe:
    def __init__(self, root):
        os.makedirs(root + '/ws/p', exist_ok=True)
        self.root = root
        self.s = Server(root, REPO, APP)
    def open(self, name, text):
        path = self.root + '/ws/p/' + name
        open(path, 'w').write(text)
        e, t = self.s.call('open_document', {'path': path})
        return e, t[0]
    def call(self, tool, args={}):
        e, t = self.s.call(tool, args)
        return e, t[0]
    def close(self):
        self.s.close()
