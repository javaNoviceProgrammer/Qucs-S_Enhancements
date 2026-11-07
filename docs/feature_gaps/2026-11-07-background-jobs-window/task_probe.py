#!/usr/bin/env python3
"""What claude says of its tasks over stream-json, and whether stop_task stops one.

Starts the real program as the dock does, asks Haiku to start a background
command and an agent, stops what it is told to, and logs every system message
about tasks, with the wall clock.
"""
import json
import os
import subprocess
import sys
import threading
import time

prompt = sys.argv[1]
stop_kinds = sys.argv[2].split(",") if len(sys.argv) > 2 and sys.argv[2] else []
log_path = sys.argv[3] if len(sys.argv) > 3 else "probe.log"
limit = float(sys.argv[4]) if len(sys.argv) > 4 else 120

args = ["claude", "-p", "--input-format", "stream-json", "--output-format", "stream-json", "--verbose",
        "--model", "haiku", "--permission-prompt-tool", "stdio", "--allowedTools", "Bash,Task,Agent,Read,Monitor"]
p = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1)
log = open(log_path, "w")
start = time.time()


def out(tag, obj):
    log.write(json.dumps({"t": round(time.time() - start, 2), tag: obj}) + "\n")
    log.flush()


def send(obj):
    out("sent", obj)
    p.stdin.write(json.dumps(obj) + "\n")
    p.stdin.flush()


send({"type": "control_request", "request_id": "init", "request": {"subtype": "initialize"}})
send({"type": "user", "message": {"role": "user", "content": prompt}, "parent_tool_use_id": None, "session_id": ""})

asked = 0
stopped = set()
live = {}
ended = 0
turns = 0


def stopper():
    time.sleep(limit)
    p.terminate()


threading.Thread(target=stopper, daemon=True).start()

for line in p.stdout:
    try:
        m = json.loads(line)
    except ValueError:
        out("raw", line)
        continue
    kind = m.get("type")
    sub = m.get("subtype", "")
    if kind == "system" and (sub.startswith("task_") or sub == "background_tasks_changed"):
        out("task", {k: v for k, v in m.items() if k not in ("uuid", "session_id")})
        if sub == "task_started":
            live[m["task_id"]] = m.get("task_type")
            if m.get("task_type") in stop_kinds or "all" in stop_kinds:
                def later(task=m["task_id"]):
                    time.sleep(6)
                    send({"type": "control_request", "request_id": "stop-" + task, "request": {"subtype": "stop_task", "task_id": task}})
                threading.Thread(target=later, daemon=True).start()
        if sub == "task_notification":
            ended += 1
    elif kind == "system":
        out("system", {"subtype": sub})
    elif kind == "control_response":
        r = m.get("response", {})
        out("control_response", {k: (v if k != "response" or r.get("request_id") != "init" else "(init)") for k, v in r.items()})
    elif kind == "control_request":
        out("control_request", m.get("request", {}).get("subtype"))
        send({"type": "control_response", "response": {"subtype": "success", "request_id": m["request_id"],
                                                       "response": {"behavior": "allow", "updatedInput": m["request"].get("input", {})}}})
    elif kind == "assistant":
        for b in m.get("message", {}).get("content", []):
            if b.get("type") == "tool_use":
                out("tool_use", {"id": b["id"], "name": b["name"], "input": b["input"], "parent": m.get("parent_tool_use_id")})
            elif b.get("type") == "text":
                out("text", b["text"][:300])
    elif kind == "user":
        for b in (m.get("message", {}).get("content") or []):
            if isinstance(b, dict) and b.get("type") == "tool_result":
                out("tool_result", {"id": b.get("tool_use_id"), "is_error": b.get("is_error"), "content": str(b.get("content"))[:300]})
    elif kind == "result":
        turns += 1
        out("result", {"subtype": m.get("subtype"), "is_error": m.get("is_error"), "cost": m.get("total_cost_usd")})
        if ended >= len(live) and live and turns >= int(os.environ.get("TURNS", "1")):
            break
p.terminate()
out("end", time.time() - start)
