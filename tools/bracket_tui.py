"""bracket.json (tools/bracket.py's state) as the game's tourney table: the lines the runtime's
SSBU_TOURNEY_UI_FILE takes after "table" (runtime/hle_tourney_ui.c).

  python3 tools/bracket_tui.py STATE/bracket.json NAME [OUT]

NAME is this game's player (their online name); OUT gets "table" and the bracket (stdout without it).

The game draws fixed trees (main+0x239d810's classes); the 1v1 ones are 8, 16 and 32 entrants with
3, 4 and 5 rounds. The bracket's winners side goes in the last rounds of the smallest one it fits, each
of its seats under a chain of matches already decided, filled out with blank entrants: the table wants
an entrant in each first-round match and a winner in each match below the one being played. Byes are
blank entrants that lose."""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bracket import BYE, Bracket, norm  # noqa: E402

CLASSES = [(3, 1), (4, 3), (5, 5)]     # 1v1 classes: (rounds, class)


def table(state, me):
    b = Bracket()
    b.state.update(state)
    b.order = b._order()
    ms = b.state["matches"]
    k = max((int(mid[1:].split("-")[0]) for mid in ms if mid.startswith("W")), default=0)
    if not k:
        raise ValueError("no winners matches")
    fit = [c for c in CLASSES if c[0] >= k]
    if not fit:
        raise ValueError(f"{k} rounds: the game's 1v1 tables have 5 at most")
    rounds, cls = fit[0]
    off = rounds - k
    size = 1 << k

    # the seats (entrants 0..size-1): round 1's sides in order
    names = []
    for s in range(size):
        src = ms[f"W1-{s // 2 + 1}"]["a" if s % 2 == 0 else "b"]
        p = b.side(src)
        names.append("" if p is BYE or p is None else p)
    seat = {norm(p): s for s, p in enumerate(names) if p}
    local = seat.get(norm(me), -1)

    match = {}      # (round, index) -> (winner, entrants)
    for g in range(rounds):
        for i in range(1 << (rounds - 1 - g)):
            if g < off:
                depth = off - 1 - g          # rounds left before the seat's own
                if g == 0:
                    if i % (1 << depth) == 0:
                        e = i >> depth
                    else:
                        e = len(names)
                        names.append("")
                    match[g, i] = (e, [e])
                else:
                    match[g, i] = (match[g - 1, 2 * i][0], [])
                continue
            mid = f"W{g - off + 1}-{i + 1}"
            r = b.outcome(mid) if mid in ms else None
            w = seat.get(norm(r[0]), -1) if r and r[0] is not BYE else -1
            match[g, i] = (w, [2 * i, 2 * i + 1] if g == 0 else [])

    # this player's match in the round being played (the first with one open)
    cur = next((g for g in range(rounds) if any(match[g, i][0] < 0 for i in range(1 << (rounds - 1 - g)))), rounds - 1)
    mine = 0
    for i in range(1 << (rounds - 1 - cur)):
        sides = match[cur, i][1] if cur == 0 else [match[cur - 1, 2 * i][0], match[cur - 1, 2 * i + 1][0]]
        if local >= 0 and local in sides:
            mine = i

    out = [f"class {cls}", f"events {len(names)}", f"local {local}", f"me {mine}"]
    for (g, i), (w, es) in sorted(match.items()):
        out.append(" ".join(map(str, ["match", g, i, w] + es)))
    out += [f"name {e} {p}" for e, p in enumerate(names) if p]
    return "\n".join(out) + "\n"


def main(argv):
    if len(argv) not in (3, 4):
        sys.exit(__doc__)
    with open(argv[1], encoding="utf-8") as f:
        text = table(json.load(f), argv[2])
    if len(argv) == 4:
        tmp = argv[3] + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            f.write("table\n" + text)
        os.replace(tmp, argv[3])
    else:
        sys.stdout.write(text)


if __name__ == "__main__":
    main(sys.argv)
