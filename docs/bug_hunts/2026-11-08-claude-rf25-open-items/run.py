"""A headless qucs-s --mcp-server of this worktree's build, called tool by tool.
usage (as a module): s = Server(folder); s.call(tool, args) -> (isError, answer, text)"""
import json, os, subprocess, tempfile, time
APP = os.environ.get("QUCS_APP", os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../../build-rel/qucs/qucs-s.app/Contents/MacOS/qucs-s"))
class Server:
    def __init__(self, folder):
        self.home = tempfile.mkdtemp(prefix="rf25home", dir=os.environ.get("SCRATCH", "/tmp"))
        os.makedirs(os.path.join(self.home, "settings"), exist_ok=True)
        env = dict(os.environ, QT_QPA_PLATFORM="offscreen", QUCS_SETTINGS_DIR=os.path.join(self.home, "settings"),
                   QUCS_NO_SHELL_ENV="1", HOME=self.home)
        self.p = subprocess.Popen([APP, "--mcp-server", "--workspace", os.path.abspath(folder)], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=open(os.path.join(self.home, "server.log"), "a"), env=env, text=True)
        self.n = 0
        self._send({"jsonrpc": "2.0", "id": 0, "method": "initialize", "params": {"protocolVersion": "2025-06-18", "capabilities": {}}}); self._recv(0)
        self._send({"jsonrpc": "2.0", "method": "notifications/initialized"})
    def _send(self, o): self.p.stdin.write(json.dumps(o) + "\n"); self.p.stdin.flush()
    def _recv(self, i):
        while True:
            line = self.p.stdout.readline()
            if not line: raise SystemExit("server ended")
            m = json.loads(line)
            if m.get("id") == i: return m
    def call(self, tool, args):
        self.n += 1
        self._send({"jsonrpc": "2.0", "id": self.n, "method": "tools/call", "params": {"name": tool, "arguments": args}})
        r = self._recv(self.n).get("result", {})
        text = "\n".join(c.get("text", "") for c in r.get("content", []))
        try: ans = json.loads(r.get("content", [{}])[0].get("text", ""))
        except Exception: ans = {"text": text}
        return bool(r.get("isError")), ans, text
    def close(self):
        try: self.p.stdin.close(); self.p.wait(timeout=60)
        except Exception: self.p.kill()
