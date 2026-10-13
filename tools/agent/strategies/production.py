"""Factories keep a mix going on repeat: a few engineers first, then the army
(raiders early, tanks and artillery after)."""

ENGINEERS = 4


def step(game, mem):
    factories = game.units("factory")
    if not factories:
        return
    engineers = len(game.units("engineer", done=False))
    army = len(game.units(["raider", "tank", "artillery", "siege"], done=False))
    for f in factories:
        if f.get("queue"):
            continue
        if engineers < ENGINEERS:
            game.cmd("produce", [f], type="engineer", amount=1)
            engineers += 1
        elif army < 10:
            game.cmd("produce", [f], type="raider", amount=4, repeat=True)
        else:
            game.cmd("produce", [f], type="tank", amount=3, repeat=True)
            game.cmd("produce", [f], type="artillery", amount=1, repeat=True)
