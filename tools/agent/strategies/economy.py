"""Builders never idle: metal on the nearest free spots, power when short,
factories as income allows. Each idle builder gets one job a step."""

from game import BUILDERS, RADIUS

MAX_FACTORIES = 4


def step(game, mem):
    e = game.economy()
    if not e:
        return
    idle = [u for u in game.units(idle=True) if u["type"] in BUILDERS]
    if not idle:
        return
    factories = len(game.units("factory", done=False))
    generators = len(game.units("generator", done=False))
    extractors = len(game.units("extractor", done=False))
    spots = sorted(game.free_spots(), key=lambda p: game.dist(p, game.base()))
    taken = []
    want_factories = min(MAX_FACTORIES, 1 + int(e["metal_income"] / 8))
    for b in idle:
        power_short = e["energy"] < 1500 and e["energy_income"] < 1.2 * e["energy_wanted"] + 10
        if (factories == 0 and e["metal"] >= 250) or (e["metal"] > 400 and factories < want_factories):
            kind = "factory"   # metal piling up: spend it on production first
        elif power_short and generators < 2 * extractors + 6:
            kind = "generator"
        elif spots:
            kind = "extractor"
        elif generators < 2 * extractors + 6:
            kind = "generator"
        else:
            continue
        if kind == "extractor":
            p = min(spots, key=lambda p: game.dist(p, b))
            spots.remove(p)
            site = (p["x"], p["z"])
        else:
            site = game.site_near(kind, game.base(), RADIUS[kind], taken)
            if not site:
                continue
            taken.append((site[0], site[1], RADIUS[kind]))
        game.cmd("build", [b], site[0], site[1], type=kind)
        factories += kind == "factory"
        generators += kind == "generator"
        extractors += kind == "extractor"
