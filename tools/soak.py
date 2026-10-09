#!/usr/bin/env python3
"""A long run with things going wrong on purpose.

    tools/soak.py BIN_DIR OUT_DIR --minutes M [--speed S] [--seed N]

A coordinator and two zones (alpha, beta), bots on both sides of each, both
sending waves through transporters between them, and a player who keeps
logging in and out of alpha (so its bot stands down and takes over). Alpha and the
coordinator are killed with SIGKILL at random and restarted; beta is left
alone so its memory can be watched over the whole run.

At the end everything settles, the zones stop cleanly, and it checks:
  - no unit lost or duplicated: each zone's lifetime departures equal the
    coordinator's transfers from it (plus its unacknowledged outbox), and its
    arrivals equal what the coordinator delivered to it;
  - every replay segment verifies, and each begins exactly where the last
    one ended (a crash-cut tail may end a segment early);
  - the player never saw a faction hash mismatch;
  - beta's memory stayed flat after warming up;
  - nothing died on its own.
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
    ap.add_argument("--minutes", type=float, default=5)
    ap.add_argument("--speed", type=float, default=8)
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()
    rng = random.Random(a.seed)
    player = None
    os.makedirs(a.out, exist_ok=True)
    for f in os.listdir(a.out):
        os.remove(os.path.join(a.out, f))
    out = lambda name: os.path.join(a.out, name)
    port = 21000 + os.getpid() % 10000
    coord_port, ports = port, {"alpha": port + 1, "beta": port + 2}
    other = {"alpha": "beta", "beta": "alpha"}

    # Each zone: an economy, bots on both sides, a transporter each, and a
    # wave of raiders through it every simulated minute, both sides, so units
    # keep crossing whichever side is winning. Paired: both zones have both
    # sides' transporters (until they're destroyed, when sends are refused).
    horizon = int(a.minutes * 60 * a.speed * 30) + 10_000
    transporters = {"blue": "130,256", "red": "382,256"}
    with open(out("pair.world"), "w") as f:
        f.write(f"port {coord_port}\nzone alpha 0.3 0.5 home blue\nzone beta 0.7 0.5 home red\nlink alpha beta\n")
    for zone in ports:
        lines = ["scenario economy", "zones alpha beta"]
        lines += [f"at 0 spawn {side} transporter 1 at {at} radius 0" for side, at in transporters.items()]
        for t in range(1800, horizon, 1800):
            for k, side in enumerate(transporters):
                lines += [f"at {t + 2 * k} select {side} raider max 20 as {side}wave",
                          f"at {t + 2 * k + 1} send {side}wave to {other[zone]}"]
        with open(out(f"{zone}.script"), "w") as f:
            f.write("\n".join(lines) + "\n")

    procs, segments, log = {}, {z: 0 for z in ports}, open(out("soak.log"), "w")

    # However this ends, nothing is left running to write into OUT_DIR
    # behind the next run's back.
    def reap():
        for p in list(procs.values()) + ([player] if player else []):
            if p.poll() is None:
                p.kill()
                p.wait()
    atexit.register(reap)
    # A laptop that sleeps freezes everything, then wakes past the deadline:
    # the run would "pass" having done nothing. Keep it awake (macOS), and
    # fail the run if it slept anyway (lid closed): time.time() jumps ahead
    # of the uptime clock, which stops while asleep.
    if sys.platform == "darwin":
        subprocess.Popen(["caffeinate", "-i", "-w", str(os.getpid())])
    # (macOS: CLOCK_UPTIME_RAW; Linux: CLOCK_MONOTONIC, which is monotonic())
    uptime = getattr(time, "CLOCK_UPTIME_RAW", None)
    clock_skew = lambda: time.time() - (time.clock_gettime(uptime) if uptime is not None else time.monotonic())
    skew0 = clock_skew()
    signal.signal(signal.SIGTERM, lambda *_: sys.exit("soak: terminated"))

    def say(msg):
        line = f"{time.strftime('%H:%M:%S')} {msg}"
        print(line, flush=True)
        log.write(line + "\n")
        log.flush()

    def start_coord():
        procs["coord"] = subprocess.Popen(
            [f"{a.bin}/rts-coord", "--port", str(coord_port), "--log", out("coord.log"), "--retry", "0.5",
             "--world", out("pair.world")],
            stdout=open(out("coord.out"), "a"), stderr=subprocess.STDOUT)

    def start_zone(zone):
        segments[zone] += 1
        procs[zone] = subprocess.Popen(
            [f"{a.bin}/rts-server", "--script", out(f"{zone}.script"), "--zone", zone, "--world", out("pair.world"),
             "--coord", f"127.0.0.1:{coord_port}", "--port", str(ports[zone]), "--speed", str(a.speed),
             "--state", out(f"{zone}.state"), "--save-every", "2",
             "--record", out(f"{zone}-{segments[zone]:03d}.replay"), "--stats", out(f"{zone}.txt")],
            stdout=open(out(f"{zone}.out"), "a"), stderr=subprocess.STDOUT)

    def rss_kb(pid):
        r = subprocess.run(["ps", "-o", "rss=", "-p", str(pid)], capture_output=True, text=True)
        return int(r.stdout.strip() or 0)

    start_coord()
    time.sleep(0.3)
    for zone in ports:
        start_zone(zone)
    say(f"soak: {a.minutes} minutes at {a.speed}x, seed {a.seed}")

    end = time.time() + a.minutes * 60
    next_kill = {"alpha": time.time() + rng.uniform(10, 30), "coord": time.time() + rng.uniform(30, 60)}
    sessions, next_session = 0, time.time() + 2
    beta_rss, kills = [], {"alpha": 0, "coord": 0}
    next_sample = time.time() + 5
    while time.time() < end:
        now = time.time()
        for name, p in list(procs.items()):
            if p.poll() is not None and name in ("alpha", "beta", "coord"):
                sys.exit(f"soak: {name} died on its own (exit {p.returncode}); see {out(name + '.out')}")
        if now >= next_kill["alpha"]:
            procs["alpha"].kill()
            procs["alpha"].wait()
            kills["alpha"] += 1
            say(f"killed alpha (#{kills['alpha']}); restarting from its saved state")
            start_zone("alpha")
            next_kill["alpha"] = now + rng.uniform(15, 40)
        if now >= next_kill["coord"]:
            procs["coord"].kill()
            procs["coord"].wait()
            kills["coord"] += 1
            say(f"killed the coordinator (#{kills['coord']}); restarting from its log")
            start_coord()
            next_kill["coord"] = now + rng.uniform(45, 90)
        if player and player.poll() is not None:
            player = None
            next_session = now + rng.uniform(1, 6)
        if not player and now >= next_session:
            sessions += 1
            player = subprocess.Popen(
                [f"{a.bin}/rts", "--connect", f"127.0.0.1:{ports['alpha']}", "--faction", "blue", "--headless",
                 "--auto-orders", "--quit-after", str(rng.randint(200, 900)), "--stats", out(f"player-{sessions:03d}.txt")],
                stdout=open(out("player.out"), "a"), stderr=subprocess.STDOUT)
        if now >= next_sample:
            beta_rss.append(rss_kb(procs["beta"].pid))
            next_sample = now + 5
        time.sleep(0.2)

    # Settle: no more chaos, every process up, transfers delivered.
    if player:
        player.wait(timeout=120)
    say("settling: no more kills")
    time.sleep(10)
    for zone in ports:
        procs[zone].send_signal(signal.SIGTERM)
        procs[zone].wait(timeout=60)
    procs["coord"].kill()
    say(f"stopped: {kills['alpha']} kills of alpha, {kills['coord']} of the coordinator, {sessions} player sessions")

    failures = []
    slept = clock_skew() - skew0
    if slept > 20:
        failures.append(f"the machine slept for {slept:.0f} s; the run doesn't count")
    stats = {z: dict(l.split(maxsplit=1) for l in open(out(f"{z}.txt")) if " " in l) for z in ports}
    # Conservation, from the coordinator's output across all its lives (its
    # log is compacted: delivered transfers leave it).
    sent, delivered = {z: 0 for z in ports}, {z: 0 for z in ports}
    transfers, logged_seqs = {}, {z: set() for z in ports}
    coord_out = open(out("coord.out")).read()
    for m in re.finditer(r"coord: transfer (\d+) (\S+) -> (\S+) \(faction \d, \w+, seq (\d+), (\d+) units\)", coord_out):
        tid, src, dst, seq, n = m[1], m[2], m[3], int(m[4]), int(m[5])
        transfers[tid] = (dst, n)
        if src in ports:
            sent[src] += n
            logged_seqs[src].add(seq)
    for tid in set(re.findall(r"coord: transfer (\d+) delivered", coord_out)):
        if tid in transfers and transfers[tid][0] in delivered:
            delivered[transfers[tid][0]] += transfers[tid][1]
    # Units must actually have crossed, or the run proved little (a side
    # that lost its transporters early sends nothing).
    crossed = sum(delivered.values())
    if crossed < 10 * a.minutes:
        failures.append(f"only {crossed} units crossed between zones in {a.minutes} minutes")
    for z in ports:
        departed, arrived = int(stats[z]["units_out"]), int(stats[z]["units_in"])
        # Outbox entries the coordinator already logged (their ack was lost)
        # are counted once, from the log.
        outbox = sum(int(l.split()[2]) for l in open(out(f"{z}.txt"))
                     if l.startswith("outbox ") and int(l.split()[1]) not in logged_seqs[z])
        say(f"{z}: departed {departed} (coordinator logged {sent[z]}, {outbox} more in its outbox), "
            f"arrived {arrived} (coordinator delivered {delivered[z]})")
        if departed != sent[z] + outbox:
            failures.append(f"{z}: {departed} departed but {sent[z]} logged + {outbox} in its outbox")
        if arrived != delivered[z]:
            failures.append(f"{z}: {arrived} arrived but {delivered[z]} delivered")
    # Replay segments chain and verify.
    for z in ports:
        chain_segments(a.bin, [out(f"{z}-{k:03d}.replay") for k in range(1, segments[z] + 1)], failures)
        say(f"{z}: {segments[z]} replay segments verified and chained")
    # The player, the memory, crashes.
    for i in range(1, sessions + 1):
        p = out(f"player-{i:03d}.txt")
        if os.path.exists(p) and "digest_mismatches 0" not in open(p).read():
            failures.append(f"player session {i} saw a faction hash mismatch")
    if len(beta_rss) >= 6:
        warm = beta_rss[len(beta_rss) // 3]
        say(f"beta memory: {warm // 1024} MB after warm-up, {max(beta_rss[len(beta_rss) // 3:]) // 1024} MB at most, "
            f"{beta_rss[-1] // 1024} MB at the end")
        if beta_rss[-1] > warm * 1.25 + 8 * 1024:
            failures.append(f"beta's memory grew: {warm} KB to {beta_rss[-1]} KB")
    for z in list(ports) + ["coord"]:
        if "fatal" in open(out(f"{z}.out")).read():
            failures.append(f"{z} logged a fatal error")
    for f in failures:
        say("FAIL " + f)
    say("soak: ok" if not failures else f"soak: {len(failures)} failures")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
