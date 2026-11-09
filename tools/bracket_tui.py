"""bracket.json (tools/bracket.py's state) as the game's tourney table: the lines the runtime's
SSBU_TOURNEY_UI_FILE takes after "table" (runtime/hle_tourney_ui.c).

  python3 tools/bracket_tui.py STATE/bracket.json NAME [OUT]

NAME is this game's player (their online name); OUT gets "table" and the bracket (stdout without it).

The table is made from the bracket's view (GET /bracket.json), as the runtime makes it itself with
SSBU_ARENA_BRACKET=1 (runtime/hle_tourney_table.c, the same steps in C; this one is for
SSBU_TOURNEY_UI_FILE and for checking that one):

The game draws fixed trees (main+0x239d810's classes); the 1v1 ones are 8, 16 and 32 entrants with
3, 4 and 5 rounds. The tree shown is the side (winners, losers or finals) of the match being played,
or of the last match once none is: from that side's last match down through the matches whose
winners feed it on the same side (a finals match takes in the winners' and the losers' sides too).
Its height picks the smallest class it fits; one too high for 5 rounds starts lower down, at the
match 4 rounds above the one being played, and the matches deepest down then show just their two
players. A side whose player comes from anywhere else (a seed, a pool's place, the loser of another
match, a match with a bye, which the view leaves out) is a leaf: that player at the top slot of the
leaf's part of the tree, under a chain of matches already decided, the other slots blank entrants.
The table wants an entrant in each first-round match and a winner in each match below the one being
played. Once the tree's last match is decided the table moves on past it, which a 5-round class has
no room for: no table then (nor with can_end false: the runtime without its hook for that).

The lines: class C, events N, local E (this player's entrant: the one that got furthest, the later
on a tie), me M (its match in the round being played), match R M W E... (round R's match M: winner
entrant W, -1 not played, then in round 0 its entrants), name E TEXT, fighter E 0x<hash>[/<costume>]."""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bracket import Bracket, norm  # noqa: E402

CLASSES = {3: 1, 4: 3, 5: 5}     # 1v1 classes: rounds -> class
MAX_ROUNDS = 5


def table_from_view(v, me, can_end=True):
    """The table's lines for player `me` from a bracket view, or None when there is none to show."""
    listed = [r for r in v.get("matches", []) if "pool" not in r and r.get("side")]
    rows = {r["id"]: r for r in listed}
    if not rows:
        return None
    cur = v.get("current")
    if cur and cur not in rows:
        return None     # (a pool's match being played)
    focus = cur or listed[-1]["id"]

    def kids(mid):      # the rows a row's two sides come from in its tree, None for a leaf
        r, out = rows[mid], []
        for src in (r.get("from") or [{}, {}])[:2]:
            x = src.get("winner") if isinstance(src, dict) else None
            out.append(x if x in rows and (rows[x]["side"] == r["side"] or r["side"] == "finals") else None)
        return out

    parent = {x: mid for mid in rows for x in kids(mid) if x and rows[x]["side"] == rows[mid]["side"]}
    height = {}

    def high(mid):
        if mid not in height:
            height[mid] = 0     # (no cycles; this only stops a bad view from recursing forever)
            height[mid] = 1 + max((high(x) for x in kids(mid) if x), default=0)
        return height[mid]

    path = [focus]
    while path[-1] in parent and len(path) <= len(rows):
        path.append(parent[path[-1]])
    root = path[min(len(path), MAX_ROUNDS) - 1]
    rounds = max(min(CLASSES), min(MAX_ROUNDS, high(root)))

    names = [""] * (1 << rounds)
    match = {}          # (round, index) -> [winner, entrants]

    def chain(name, g, i):      # a leaf at round g's match i
        names[i << (g + 1)] = name
        for h in range(g + 1):
            for j in range(i << (g - h), (i + 1) << (g - h)):
                match[h, j] = [j << (h + 1), [2 * j] if h == 0 else []]
        return i << (g + 1)

    def place(mid, g, i):       # a row at round g's match i: returns its winner's entrant (-1 not played)
        r, ks, got = rows[mid], kids(mid), []
        sides = [r.get("a") or "", r.get("b") or ""]
        for k in (0, 1):
            if g == 0:
                names[2 * i + k] = sides[k]
                got.append(2 * i + k)
            elif ks[k]:
                got.append(place(ks[k], g - 1, 2 * i + k))
            else:
                got.append(chain(sides[k], g - 1, 2 * i + k))
        w = norm(r.get("winner"))
        win = next((got[k] for k in (0, 1) if w and norm(sides[k]) == w), -1)
        match[g, i] = [win, [2 * i, 2 * i + 1] if g == 0 else []]
        return win

    place(root, rounds - 1, 0)
    if match[rounds - 1, 0][0] >= 0 and (rounds == MAX_ROUNDS or not can_end):
        return None
    count = lambda g: 1 << (rounds - 1 - g)   # noqa: E731
    now = next((g for g in range(rounds) if any(match[g, i][0] < 0 for i in range(count(g)))), rounds - 1)
    local, best = -1, -1
    for e, p in enumerate(names):
        if p and norm(p) == norm(me):
            won = sum(match[g, i][0] == e for g in range(rounds) for i in range(count(g)))
            if won >= best:
                local, best = e, won
    mine = 0
    for i in range(count(now)):
        sides = match[now, i][1] if now == 0 else [match[now - 1, 2 * i][0], match[now - 1, 2 * i + 1][0]]
        if local >= 0 and local in sides:
            mine = i

    fighters = {norm(p): f for p, f in (v.get("fighters") or {}).items()}
    out = [f"class {CLASSES[rounds]}", f"events {len(names)}", f"local {local}", f"me {mine}"]
    for (g, i), (w, es) in sorted(match.items()):
        out.append(" ".join(map(str, ["match", g, i, w] + es)))
    out += [f"name {e} {p}" for e, p in enumerate(names) if p]
    out += [f"fighter {e} {fighters[norm(p)]}" for e, p in enumerate(names) if p and norm(p) in fighters]
    return "\n".join(out) + "\n"


def table(state, me):
    b = Bracket()
    b.state.update(state)
    b.order = b._order()
    text = table_from_view(b.view(), me)
    if text is None:
        raise ValueError("no table for this bracket now (a pool being played, too many rounds, or its end)")
    return text


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
