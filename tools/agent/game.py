"""A side of a zone, played through `rts --agent` (src/client.c).

Game wraps the client: state() is our side as the client simulates it and
the enemy as it sees them; cmd() sends anything a player can send. It's an
interface, not a cheat: the client knows what a player's client knows.

Strategies (tools/agent/strategies/*.py) each define step(game, memory),
called a few times a second by the runner with fresh state; edit a file and
it reloads. They're how this side acts faster than a person (or an agent's
tool calls) could.
"""

import importlib.util
import json
import math
import os
import subprocess
import threading
import time
import traceback

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
STRATEGIES = os.path.join(ROOT, "tools", "agent", "strategies")


class Game:
    def __init__(self):
        self.proc = None
        self.lock = threading.Lock()
        self.s = None            # the last state
        self.log = []            # strategy errors and notes, newest last
        self.enabled = {}        # strategy name -> on
        self.modules = {}        # name -> (mtime, module)
        self.memory = {}         # name -> dict kept between steps
        self.runner = None
        self.period = 0.5

    # ------------------------------------------------------------ the client

    def join(self, port, host="127.0.0.1", faction="red"):
        if self.proc and self.proc.poll() is None:
            self.proc.kill()
        exe = os.path.join(ROOT, "build", "rts")
        self.proc = subprocess.Popen([exe, "--connect", f"{host}:{port}", "--faction", faction, "--agent"],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                     text=True, bufsize=1, cwd=ROOT)
        for _ in range(100):
            s = self.request("state")
            if s.get("ok"):
                self.s = s
                break
            time.sleep(0.1)
        if not self.runner:
            self.runner = threading.Thread(target=self._run, daemon=True)
            self.runner.start()
        return self.s

    def request(self, line):
        with self.lock:
            if not self.proc or self.proc.poll() is not None:
                return {"ok": False, "error": "not joined"}
            self.proc.stdin.write(line.replace("\n", " ") + "\n")
            self.proc.stdin.flush()
            while True:
                out = self.proc.stdout.readline()
                if not out:
                    return {"ok": False, "error": "client gone"}
                if out.startswith("{"):
                    return json.loads(out)

    def state(self):
        s = self.request("state")
        if s.get("ok"):
            self.s = s
        return s

    def cmd(self, kind, units=(), x=0.0, z=0.0, type="-", amount=0, queued=False, repeat=False, to=0):
        flags = ",".join(f for f, on in (("queued", queued), ("repeat", repeat)) if on) or "-"
        ids = " ".join(str(u["id"] if isinstance(u, dict) else u) for u in units)
        return self.request(f"cmd {kind} {flags} {type} {int(amount)} {x:.1f} {z:.1f} {to} {ids}")

    def note(self, action, text=""):
        return self.request(f"note {action} {text}")

    # ------------------------------------------------------------ helpers for strategies

    def units(self, type=None, idle=None, done=True):
        out = []
        for u in (self.s or {}).get("units", []):
            if type and u["type"] != type and not (isinstance(type, (list, tuple, set)) and u["type"] in type):
                continue
            if idle is not None and (u["order"] == "idle" and u["later"] == 0) != idle:
                continue
            if done and u["progress"] < 1.0:
                continue
            out.append(u)
        return out

    def building(self):
        return [u for u in (self.s or {}).get("units", []) if u["progress"] < 1.0]

    def economy(self):
        return (self.s or {}).get("economy", {})

    def enemies(self, type=None):
        return [e for e in (self.s or {}).get("enemies", []) if not type or e["type"] == type]

    def free_spots(self):
        return [p for p in (self.s or {}).get("spots", []) if p["free"]]

    def base(self):
        return tuple((self.s or {}).get("base", (0, 0)))

    def enemy_base(self):
        return tuple((self.s or {}).get("enemy_base", (0, 0)))

    @staticmethod
    def dist(a, b):
        ax, az = (a["x"], a["z"]) if isinstance(a, dict) else a
        bx, bz = (b["x"], b["z"]) if isinstance(b, dict) else b
        return math.hypot(ax - bx, az - bz)

    @staticmethod
    def centre(units):
        if not units:
            return None
        return (sum(u["x"] for u in units) / len(units), sum(u["z"] for u in units) / len(units))

    def site_near(self, type, near, radius, taken=()):
        """A spot for a structure of `radius` near `near`: clear of our structures,
        anything planned (`taken`: (x, z, r)), and the map's edge."""
        size = (self.s or {}).get("map", 512)
        structures = [(u["x"], u["z"], 5.0) for u in (self.s or {}).get("units", [])
                      if u["type"] in STRUCTURES] + list(taken)
        for ring in range(1, 12):
            r = 10.0 + 7.0 * ring
            steps = 8 + 4 * ring
            for k in range(steps):
                a = 2 * math.pi * k / steps
                x, z = near[0] + r * math.cos(a), near[1] + r * math.sin(a)
                if not (radius + 4 < x < size - radius - 4 and radius + 4 < z < size - radius - 4):
                    continue
                if all(math.hypot(x - sx, z - sz) > radius + sr + 2.0 for sx, sz, sr in structures):
                    return (x, z)
        return None

    # ------------------------------------------------------------ the strategy runner

    def strategies(self):
        names = sorted(f[:-3] for f in os.listdir(STRATEGIES) if f.endswith(".py") and not f.startswith("_"))
        return [{"name": n, "enabled": self.enabled.get(n, False)} for n in names]

    def enable(self, name, on=True):
        if not os.path.exists(os.path.join(STRATEGIES, name + ".py")):
            return False
        self.enabled[name] = on
        return True

    def _module(self, name):
        path = os.path.join(STRATEGIES, name + ".py")
        mtime = os.path.getmtime(path)
        have = self.modules.get(name)
        if have and have[0] == mtime:
            return have[1]
        spec = importlib.util.spec_from_file_location(f"strategy_{name}", path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        self.modules[name] = (mtime, mod)
        self._say(f"loaded strategy {name}")
        return mod

    def _say(self, text):
        self.log.append(f"{time.strftime('%H:%M:%S')} {text}")
        del self.log[:-50]

    def _run(self):
        while True:
            time.sleep(self.period)
            try:
                s = self.state()
            except Exception:
                continue
            if not s.get("ok") or s.get("paused"):
                continue
            for name, on in list(self.enabled.items()):
                if not on:
                    continue
                try:
                    self._module(name).step(self, self.memory.setdefault(name, {}))
                except Exception as e:
                    self._say(f"{name}: {e!r} at {traceback.extract_tb(e.__traceback__)[-1].lineno}")


STRUCTURES = {"extractor", "advanced-extractor", "generator", "factory", "advanced-factory", "transporter", "turret",
              "metal-vault", "energy-vault", "headquarters"}
COMBAT = {"raider", "tank", "artillery", "siege", "heavy-tank", "howitzer"}
BUILDERS = {"engineer", "commander", "advanced-engineer"}
RADIUS = {"extractor": 1.6, "generator": 1.8, "factory": 4.0, "transporter": 5.0, "turret": 1.6}
