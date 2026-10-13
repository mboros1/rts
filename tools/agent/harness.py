#!/usr/bin/env python3
"""A tiny MCP client for tests and demos: starts tools/agent/mcp_server.py and
calls its tools over JSON-RPC on stdio, as Claude Code would.

  harness.py PORT STEP...    each STEP: wait:SECONDS | join | strategies | pause:TEXT | resume
"""

import json
import os
import subprocess
import sys
import time

here = os.path.dirname(os.path.abspath(__file__))
server = subprocess.Popen([sys.executable, os.path.join(here, "mcp_server.py")], stdin=subprocess.PIPE,
                          stdout=subprocess.PIPE, text=True, bufsize=1)
ids = 0


def rpc(method, params=None):
    global ids
    ids += 1
    server.stdin.write(json.dumps({"jsonrpc": "2.0", "id": ids, "method": method, "params": params or {}}) + "\n")
    server.stdin.flush()
    return json.loads(server.stdout.readline())


def tool(name, **args):
    out = rpc("tools/call", {"name": name, "arguments": args})["result"]
    print(f"harness: {name} -> {out['content'][0]['text'][:120]}", flush=True)
    return out


port = int(sys.argv[1])
rpc("initialize", {"protocolVersion": "2024-11-05"})
for step in sys.argv[2:]:
    verb, _, arg = step.partition(":")
    if verb == "wait":
        time.sleep(float(arg))
    elif verb == "join":
        tool("join", port=port)
    elif verb == "strategies":
        tool("strategies", enable={"economy": True, "production": True, "army": True})
    elif verb == "pause":
        tool("note", action="pause", text=arg)
    elif verb == "resume":
        tool("note", action="resume")
server.kill()
