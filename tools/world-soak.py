#!/usr/bin/env python3
"""A self-playing world, for a long time, with things going wrong on purpose.

    tools/world-soak.py BIN_DIR OUT_DIR --minutes M [--world FILE] [--speed S] [--seed N]

`rts world` starts a world (worlds/five.world by default) with bots playing
both sides everywhere. Every 20-60 s a random zone is killed with SIGKILL, and
every 60-120 s the coordinator; `rts world` brings back whatever is down. Now
and then a player (the client's --bot) launches into a zone for a while.

At the end the world stops cleanly, and it checks:
  - no unit lost or duplicated: each zone's lifetime departures equal what
    the coordinator logged from it (plus its unacknowledged outbox), and its
    arrivals what the coordinator delivered to it (counted from the
    coordinator's output: its log is compacted as it goes);
  - every zone's replay segments verify and chain;
  - every process stayed within its memory after warming up;
  - nothing died on its own, no player saw a faction hash mismatch;
  - both sides expanded beyond their home zones, units crossed, and the
    coordinator compacted its log (on runs long enough to need it).
"""

import argparse
import atexit
import os
import random
import re
import signal
import subprocess
import sys
import time

def chain_segments(bin_dir, paths, failures):
    """Verify each replay segment and check it carries on from the one
    before: it starts at a tick the last one reached, in the same state.
    (A zone flushes its replay before writing its state, so one killed in
    between restarts from a state a little behind its replay's end.)"""
    prev = None  # (path, end tick, end hash)
    for path in paths:
        r = subprocess.run([f"{bin_dir}/rts-headless", "verify", path, "--partial"], capture_output=True, text=True)
        m = re.search(r"segment from tick (\d+) \((\w+)\) to tick (\d+) \((\w+)\)", r.stdout)
        if r.returncode or not m:
            failures.append(f"{path} doesn't verify: {r.stderr.strip()[:200]}")
            prev = None
            continue
        start, start_hash = int(m[1]), m[2]
        if prev:
            prev_path, prev_end, prev_hash = prev
            if start > prev_end:
                failures.append(f"{path} starts at tick {start} but {prev_path} ended at tick {prev_end}")
            else:
                h = prev_hash
                if start < prev_end:
                    rr = subprocess.run([f"{bin_dir}/rts-headless", "verify", prev_path, "--partial", "--hash-at", str(start)],
                                        capture_output=True, text=True)
                    hm = re.search(r"hash at tick \d+: (\w+)", rr.stdout)
                    h = hm[1] if hm else None
                if h != start_hash:
                    failures.append(f"{path} starts at tick {start} in a state {prev_path} wasn't in then")
        prev = (path, int(m[3]), m[4])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("bin")
    ap.add_argument("out")
    ap.add_argument("--minutes", type=float, default=10)
    ap.add_argument("--world", default="worlds/five.world")
    ap.add_argument("--speed", type=float, default=8)
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()
    rng = random.Random(a.seed)
    subprocess.run(["rm", "-rf", a.out], check=True)
    os.makedirs(a.out)
    live = os.path.join(a.out, "w", "live")
    log = open(os.path.join(a.out, "soak.log"), "w")

    def say(msg):
        line = f"{time.strftime('%H:%M:%S')} {msg}"
        print(line, flush=True)
        log.write(line + "\n")
        log.flush()

    port = 50000 + os.getpid() % 1500 * 8  # its own band, clear of the check stages
    world_file = os.path.join(a.out, "soak.world")
    with open(a.world) as src, open(world_file, "w") as dst:
        dst.write(re.sub(r"^port .*$", f"port {port}", src.read(), flags=re.M))
    zones = re.findall(r"^zone (\S+)", open(world_file).read(), flags=re.M)
    homes = dict(re.findall(r"^zone (\S+) .* home (\w+)", open(world_file).read(), flags=re.M))
    world = [f"{a.bin}/rts", "world", "--dir", os.path.join(a.out, "w")]

    def pid(name):
        try:
            return int(open(os.path.join(live, f"{name}.pid")).read())
        except (OSError, ValueError):
            return 0

    def alive(p):
        if p <= 0:
            return False
        try:
            os.kill(p, 0)
            return True
        except OSError:
            return False

    def rss_kb(p):
        r = subprocess.run(["ps", "-o", "rss=", "-p", str(p)], capture_output=True, text=True)
        return int(r.stdout.strip() or 0)

    player = None

    def reap():
        if player and player.poll() is None:
            player.kill()
        subprocess.run(world + ["--stop"], capture_output=True)
    atexit.register(reap)
    signal.signal(signal.SIGTERM, lambda *_: sys.exit("world-soak: terminated"))
    if sys.platform == "darwin":
        subprocess.Popen(["caffeinate", "-i", "-w", str(os.getpid())])
    uptime = getattr(time, "CLOCK_UPTIME_RAW", None)
    clock_skew = lambda: time.time() - (time.clock_gettime(uptime) if uptime is not None else time.monotonic())
    skew0 = clock_skew()

    def start(*extra):
        r = subprocess.run(world + list(extra) + ["--headless", "--speed", str(a.speed)], capture_output=True, text=True)
        if r.returncode:
            sys.exit(f"world-soak: rts world failed: {r.stderr.strip()[:300]}")

    def status():
        r = subprocess.run(world + ["--status"], capture_output=True, text=True)
        return r.stdout

    start("--new", world_file)
    say(f"world-soak: {a.minutes} minutes of {a.world} at {a.speed}x, seed {a.seed}")
    end = time.time() + a.minutes * 60
    next_kill = {"zone": time.time() + rng.uniform(20, 60), "coord": time.time() + rng.uniform(60, 120)}
    killed = {"zone": 0, "coord": 0}
    expected_down = set()
    next_sample, next_player, sessions = time.time() + 30, time.time() + 30, 0
    rss, held_beyond_home, failures = {}, {"blue": set(), "red": set()}, []
    while time.time() < end:
        now = time.time()
        for name in zones + ["coord"]:
            if name not in expected_down and not alive(pid(name)):
                failures.append(f"{name} died on its own; see {os.path.join(live, name + '.out')}")
                say("FAIL " + failures[-1])
                expected_down.add(name)
        if now >= next_kill["zone"]:
            victim = rng.choice(zones)
            os.kill(pid(victim), signal.SIGKILL)
            killed["zone"] += 1
            say(f"killed zone {victim} (#{killed['zone']})")
            time.sleep(0.2)
            start()
            next_kill["zone"] = now + rng.uniform(20, 60)
        if now >= next_kill["coord"]:
            os.kill(pid("coord"), signal.SIGKILL)
            killed["coord"] += 1
            say(f"killed the coordinator (#{killed['coord']})")
            time.sleep(0.2)
            start()
            next_kill["coord"] = now + rng.uniform(60, 120)
        if player and player.poll() is not None:
            player = None
        if not player and now >= next_player:
            sessions += 1
            zone = rng.choice(zones)
            player = subprocess.Popen(
                [f"{a.bin}/rts", "--coord", f"127.0.0.1:{port}", "--headless", "--faction", rng.choice(["blue", "red"]),
                 "--bot", "--journey", f"map:1,{zone}:{rng.randint(10, 60)}",
                 "--stats", os.path.join(a.out, f"player-{sessions:03d}.txt")],
                stdout=open(os.path.join(a.out, "player.out"), "a"), stderr=subprocess.STDOUT)
            next_player = now + rng.uniform(30, 90)
        if now >= next_sample:
            next_sample = now + 30
            for name in zones + ["coord"]:
                # Per process: a restarted zone has a different baseline.
                rss.setdefault((name, pid(name)), []).append(rss_kb(pid(name)))
            st = status()
            for line in st.splitlines():
                f = line.split()
                if len(f) > 3 and f[0] in zones:
                    m = re.search(r"blue (\d+) units (\d+) bldg.*\| red (\d+) units (\d+) bldg", line)
                    if not m:
                        continue
                    b, r = int(m[1]) + int(m[2]), int(m[3]) + int(m[4])
                    if b and homes.get(f[0]) != "blue":
                        held_beyond_home["blue"].add(f[0])
                    if r and homes.get(f[0]) != "red":
                        held_beyond_home["red"].add(f[0])
            log.write(st)
        time.sleep(0.25)

    if player:
        player.wait(timeout=120)
    say("settling: no more kills")
    time.sleep(10)
    say("final map:\n" + status())
    stop = subprocess.run(world + ["--stop"], capture_output=True, text=True)
    if stop.returncode:
        failures.append(f"the world didn't stop cleanly: {stop.stdout.strip()}")
    say(f"stopped: {killed['zone']} zone kills, {killed['coord']} coordinator kills, {sessions} player sessions")

    # Conservation, from the coordinator's output across all its lives.
    sent, delivered, units_of, logged_seqs = {}, {}, {}, {}
    out = open(os.path.join(live, "coord.out")).read()
    for m in re.finditer(r"coord: transfer (\d+) (\S+) -> (\S+) \(faction \d, \w+, seq (\d+), (\d+) units\)", out):
        tid, src, dst, seq, n = m[1], m[2], m[3], int(m[4]), int(m[5])
        units_of[tid] = (dst, n)
        sent[src] = sent.get(src, 0) + n
        logged_seqs.setdefault(src, set()).add(seq)
    done = set(re.findall(r"coord: transfer (\d+) delivered", out))
    for tid in done:
        if tid in units_of:
            dst, n = units_of[tid]
            delivered[dst] = delivered.get(dst, 0) + n
    crossed = sum(delivered.values())
    for z in zones:
        st = dict(l.split(maxsplit=1) for l in open(os.path.join(live, f"{z}.txt")) if " " in l)
        departed, arrived = int(st["units_out"]), int(st["units_in"])
        outbox = sum(int(l.split()[2]) for l in open(os.path.join(live, f"{z}.txt"))
                     if l.startswith("outbox ") and int(l.split()[1]) not in logged_seqs.get(z, set()))
        say(f"{z}: departed {departed} (logged {sent.get(z, 0)}, {outbox} more in its outbox), "
            f"arrived {arrived} (delivered {delivered.get(z, 0)})")
        if departed != sent.get(z, 0) + outbox:
            failures.append(f"{z}: {departed} departed but {sent.get(z, 0)} logged + {outbox} in its outbox")
        if arrived != delivered.get(z, 0):
            failures.append(f"{z}: {arrived} arrived but {delivered.get(z, 0)} delivered")
    # Replay segments chain and verify.
    for z in zones:
        paths, k = [], 1
        while os.path.exists(os.path.join(live, f"{z}-{k:03d}.replay")):
            paths.append(os.path.join(live, f"{z}-{k:03d}.replay"))
            k += 1
        chain_segments(a.bin, paths, failures)
        say(f"{z}: {len(paths)} replay segments verified and chained")
    # Memory, over each process's life: after warming up (the first third),
    # within 25% (+16 MB). Lives too short to say are skipped.
    for (name, p), samples in sorted(rss.items()):
        samples = [s for s in samples if s > 0]
        if len(samples) < 6:
            continue
        warm, last = samples[len(samples) // 3], samples[-1]
        say(f"{name} (pid {p}, {len(samples)} samples): {warm // 1024} MB after warm-up, "
            f"{max(samples[len(samples) // 3:]) // 1024} MB at most, {last // 1024} MB at the end")
        if last > warm * 1.25 + 16 * 1024:
            failures.append(f"{name}'s memory grew: {warm} KB to {last} KB")
    for i in range(1, sessions + 1):
        p = os.path.join(a.out, f"player-{i:03d}.txt")
        if os.path.exists(p) and "digest_mismatches 0" not in open(p).read():
            failures.append(f"player session {i} saw a faction hash mismatch")
    say(f"held beyond home: blue {sorted(held_beyond_home['blue'])}, red {sorted(held_beyond_home['red'])}; "
        f"{crossed} units crossed")
    for side, held in held_beyond_home.items():
        if not held:
            failures.append(f"{side} never got beyond its home zone")
    if crossed < 10 * a.minutes:
        failures.append(f"only {crossed} units crossed in {a.minutes} minutes")
    compactions = out.count("compacted")
    say(f"the coordinator compacted its log {compactions} times")
    slept = clock_skew() - skew0
    if slept > 20:
        failures.append(f"the machine slept for {slept:.0f} s; the run doesn't count")
    for f in failures:
        say("FAIL " + f)
    say("world-soak: ok" if not failures else f"world-soak: {len(failures)} failures")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
