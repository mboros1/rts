#!/usr/bin/env python3
"""An MCP server (stdio) that plays a side of a zone: tools/agent/game.py.

The model playing calls these tools; its strategies (tools/agent/strategies)
run in this process between calls. Hand-rolled JSON-RPC, so there's
nothing to install: initialize, tools/list, tools/call, ping.
"""

import json
import sys
import time

from game import COMBAT, BUILDERS, Game

game = Game()


def summary(s, full=False):
    if not s.get("ok"):
        return s
    if full:
        return s
    mine = {}
    for u in s["units"]:
        key = u["type"] if u["progress"] >= 1 else u["type"] + " (building)"
        mine[key] = mine.get(key, 0) + 1
    army = [u for u in s["units"] if u["type"] in COMBAT]
    idle_builders = [u["id"] for u in s["units"] if u["type"] in BUILDERS and u["order"] == "idle" and u["later"] == 0]
    factories = [{"id": u["id"], "queue": u.get("queue", []), "repeat": u.get("repeat")}
                 for u in s["units"] if u["type"] == "factory" and u["progress"] >= 1]
    seen = {}
    for e in s["enemies"]:
        seen[e["type"]] = seen.get(e["type"], 0) + 1
    base = s["base"]
    near = [e for e in s["enemies"] if Game.dist(e, base) < 120]
    hq = [u for u in s["units"] if u["type"] == "headquarters"]
    return {
        "seconds": s["seconds"], "paused": s["paused"], "note": s["note"], "side": s["side"],
        "economy": s["economy"], "base": base, "enemy_base": s["enemy_base"], "map": s["map"],
        "mine": mine, "army": {"count": len(army), "centre": Game.centre(army)},
        "idle_builders": idle_builders, "factories": factories,
        "enemies_seen": seen, "enemy_centre": Game.centre(s["enemies"]), "enemies_near_base": len(near),
        "headquarters_hp": hq[0]["hp"] if hq else 0,
        "free_spots": len([p for p in s["spots"] if p["free"]]),
        "strategies": game.strategies(), "log": game.log[-8:],
    }


def select(spec):
    """Units by ids, or by {"type": ..., "idle": bool}."""
    if isinstance(spec, list):
        return spec
    game.state()
    return [u["id"] for u in game.units(type=spec.get("type"), idle=spec.get("idle"))]


def t_join(a):
    return summary(game.join(int(a.get("port", 39200)), a.get("host", "127.0.0.1"), a.get("faction", "red")))


def t_state(a):
    return summary(game.state(), bool(a.get("full")))


def t_command(a):
    ids = select(a.get("units", []))
    return game.cmd(a["kind"], ids, float(a.get("x", 0)), float(a.get("z", 0)), a.get("type", "-"),
                    int(a.get("amount", 0)), bool(a.get("queued")), bool(a.get("repeat")))


def t_note(a):
    return game.note(a.get("action", "say"), a.get("text", ""))


def t_wait(a):
    """Let the game run; back early if the other side pauses or our base is hit."""
    until = time.time() + float(a.get("seconds", 20))
    start = game.state()
    hq0 = summary(start).get("headquarters_hp", 0) if start.get("ok") else 0
    why = "time"
    while time.time() < until:
        time.sleep(1.0)
        s = game.state()
        if not s.get("ok"):
            why = "client: " + s.get("error", "?")
            break
        if s["paused"] and s["note"]["from"] != s["side"]:
            why = "the other side paused"
            break
        if summary(s)["headquarters_hp"] < hq0 * 0.9:
            why = "headquarters under attack"
            break
    out = summary(game.state())
    out["woke_because"] = why
    return out


def t_strategies(a):
    for name, on in (a.get("enable") or {}).items():
        game.enable(name, bool(on))
    if "period" in a:
        game.period = max(0.1, float(a["period"]))
    return {"strategies": game.strategies(), "period": game.period, "log": game.log[-15:]}


TOOLS = {
    "join": (t_join, "Join a zone as a side through `rts --agent` (port of the zone server).",
             {"port": {"type": "integer"}, "host": {"type": "string"}, "faction": {"type": "string"}}),
    "state": (t_state, "Our side now: economy, units by type, idle builders, factories, army, enemies seen, strategies. full=true for every unit.",
              {"full": {"type": "boolean"}}),
    "command": (t_command, "Send a command: kind (move, fight, stop, build, produce, entrench, repeat, ...), units (ids, or {type, idle}), x, z, type (unit name), amount, queued, repeat.",
                {"kind": {"type": "string"}, "units": {}, "x": {"type": "number"}, "z": {"type": "number"},
                 "type": {"type": "string"}, "amount": {"type": "integer"}, "queued": {"type": "boolean"},
                 "repeat": {"type": "boolean"}}),
    "note": (t_note, "Say something on the other player's screen; action pause or resume pauses/resumes the zone with it.",
             {"action": {"type": "string", "enum": ["say", "pause", "resume"]}, "text": {"type": "string"}}),
    "wait": (t_wait, "Let the game run for `seconds` (strategies keep playing); returns early if the other side pauses or our headquarters is hit.",
             {"seconds": {"type": "number"}}),
    "strategies": (t_strategies, "List strategies (tools/agent/strategies/*.py, reloaded when edited); enable: {name: bool}; period: seconds between steps.",
                   {"enable": {"type": "object"}, "period": {"type": "number"}}),
}


def reply(id, result=None, error=None):
    msg = {"jsonrpc": "2.0", "id": id}
    if error:
        msg["error"] = error
    else:
        msg["result"] = result
    sys.stdout.write(json.dumps(msg) + "\n")
    sys.stdout.flush()


def main():
    for line in sys.stdin:
        try:
            req = json.loads(line)
        except ValueError:
            continue
        method, id = req.get("method"), req.get("id")
        if id is None:
            continue  # notifications
        if method == "initialize":
            reply(id, {"protocolVersion": req.get("params", {}).get("protocolVersion", "2024-11-05"),
                       "capabilities": {"tools": {}}, "serverInfo": {"name": "rts", "version": "1"}})
        elif method == "ping":
            reply(id, {})
        elif method == "tools/list":
            reply(id, {"tools": [{"name": n, "description": d, "inputSchema": {"type": "object", "properties": p}}
                                 for n, (_, d, p) in TOOLS.items()]})
        elif method == "tools/call":
            name = req["params"]["name"]
            args = req["params"].get("arguments") or {}
            try:
                out = TOOLS[name][0](args)
                reply(id, {"content": [{"type": "text", "text": json.dumps(out)}]})
            except Exception as e:
                reply(id, {"content": [{"type": "text", "text": f"{type(e).__name__}: {e}"}], "isError": True})
        else:
            reply(id, error={"code": -32601, "message": f"no method {method}"})


if __name__ == "__main__":
    main()
