"""The army: defend when the base is threatened, gather in front of it
otherwise, and attack-move on the enemy once it's big enough."""

from game import COMBAT

ATTACK_AT = 18      # units gathered before attacking
RETREAT_AT = 6      # an attack this small comes home


def step(game, mem):
    army = game.units(list(COMBAT))
    if not army:
        mem["attacking"] = False
        return
    base, enemy = game.base(), game.enemy_base()
    near = [e for e in game.enemies() if game.dist(e, base) < 100]
    if near:  # defend: everyone fights at the threat
        c = game.centre(near)
        game.cmd("fight", army, c[0], c[1])
        mem["attacking"] = False
        return
    if mem.get("attacking"):
        if len(army) < RETREAT_AT:
            mem["attacking"] = False
        else:
            # Keep pushing: at the nearest enemy seen, else their base.
            seen = game.enemies()
            target = min(((e["x"], e["z"]) for e in seen), key=lambda p: game.dist(p, game.centre(army)),
                         default=enemy)
            idle = [u for u in army if u["order"] == "idle"]
            if idle or mem.get("target") != target:
                game.cmd("fight", army, target[0], target[1])
                mem["target"] = target
            return
    if len(army) >= ATTACK_AT:
        mem["attacking"] = True
        game.cmd("fight", army, enemy[0], enemy[1])
        mem["target"] = enemy
        return
    # Gather a little toward the enemy, in front of the base.
    rally = (base[0] + (enemy[0] - base[0]) * 0.18, base[1] + (enemy[1] - base[1]) * 0.18)
    idle = [u for u in army if u["order"] == "idle" and game.dist(u, rally) > 15]
    if idle:
        game.cmd("move", idle, rally[0], rally[1])
