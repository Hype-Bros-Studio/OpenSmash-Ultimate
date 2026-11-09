"""tools/bracket.py: the arena bracket the online lobby keeps. Brackets of every size from 2 to 16
played out (single and double elimination, byes, the grand-final reset), players' reports (agreeing,
disagreeing, one left alone), best-of sets, the organizer's fixes, the saved file, and the HTTP the players' games
and the page use, over loopback. Made-up names only."""
import asyncio
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'tools'))
import bracket as B  # noqa: E402

NAMES = [f"P{i}" for i in range(1, 17)]


class Clock:
    def __init__(self):
        self.t = 1000.0

    def __call__(self):
        return self.t


def play_out(b, pick):
    """Plays every match with both reports; pick(a, b) -> winner. Returns the matches in order."""
    played = []
    for _ in range(200):
        mid = b.current()
        if mid is None:
            return played
        a, c = b.players_of(mid)
        w = pick(a, c)
        lo = c if w == a else a
        assert b.report(mid, w, lo, a)[0] == 200
        assert b.report(mid, w, lo, c) == (200, "confirmed")
        played.append((mid, a, c, w))
    raise AssertionError("never ends")


def seed_of(name):
    return int(name[1:])


class Build(unittest.TestCase):
    def test_seed_order(self):
        self.assertEqual(B.seed_order(8), [1, 8, 4, 5, 2, 7, 3, 6])
        self.assertEqual(sorted(B.seed_order(16)), list(range(1, 17)))

    def test_every_size_plays_out(self):
        for fmt in ("single", "double"):
            for n in range(2, 17):
                with self.subTest(fmt=fmt, n=n):
                    b = B.Bracket()
                    b.setup(NAMES[:n], fmt)
                    played = play_out(b, lambda a, c: min(a, c, key=seed_of))
                    self.assertEqual(b.champion(), "P1")
                    self.assertIsNone(b.current())
                    # every player loses at most once (single) or twice (double), and only P1 never
                    losses = {}
                    for _mid, a, c, w in played:
                        lo = c if w == a else a
                        losses[lo] = losses.get(lo, 0) + 1
                    limit = 1 if fmt == "single" or n == 2 else 2
                    self.assertTrue(all(v <= limit for v in losses.values()), losses)
                    self.assertEqual(set(losses), set(NAMES[:n]) - {"P1"})
                    if limit == 2:
                        self.assertTrue(all(v == 2 for v in losses.values()), losses)
                    # nobody plays twice at once: a player is never in a match before their last one ended
                    self.assertEqual(len(played), len({p[0] for p in played}))

    def test_eight_double_order_and_count(self):
        b = B.Bracket()
        b.setup(NAMES[:8], "double")
        played = play_out(b, lambda a, c: min(a, c, key=seed_of))
        self.assertEqual(len(played), 14)   # 7 winners, 6 losers, grand final, no reset
        self.assertEqual([p[0] for p in played[:4]], ["W1-1", "W1-2", "W1-3", "W1-4"])
        self.assertEqual(played[0][1:3], ("P1", "P8"))
        self.assertEqual(played[-1][0], "GF")

    def test_grand_final_reset(self):
        b = B.Bracket()
        b.setup(NAMES[:4], "double")
        # P2 loses early, comes through losers, and beats P1 twice
        def pick(a, c):
            if {a, c} == {"P1", "P2"}:
                return "P1" if b.current() == "W2-1" else "P2"
            return min(a, c, key=seed_of)
        played = play_out(b, pick)
        self.assertEqual([p[0] for p in played][-2:], ["GF", "GF2"])
        self.assertEqual(b.champion(), "P2")

    def test_reset_not_played_when_winners_side_wins(self):
        b = B.Bracket()
        b.setup(NAMES[:4], "double")
        played = play_out(b, lambda a, c: min(a, c, key=seed_of))
        self.assertNotIn("GF2", [p[0] for p in played])
        self.assertNotIn("GF2", [m["id"] for m in b.view()["matches"]])

    def test_byes_not_shown_or_played(self):
        b = B.Bracket()
        b.setup(NAMES[:5], "double")
        v = b.view()
        self.assertTrue(all(m["a"] is not None and m["b"] is not None for m in v["matches"]))
        self.assertEqual(v["current"], "W1-2")             # (W1-1 is P1's bye)
        self.assertEqual(b.players_of("W1-2"), ("P4", "P5"))
        self.assertNotIn("W1-1", [m["id"] for m in v["matches"]])

    def test_bad_setups(self):
        b = B.Bracket()
        for players in (["A"], ["A", " a "], ["X" * 41, "B"]):
            with self.assertRaises(ValueError):
                b.setup(players)
        with self.assertRaises(ValueError):
            b.setup(["A", "B"], "swiss")


class Pools(unittest.TestCase):
    def test_round_robin_everyone_meets_once(self):
        for n in range(2, 10):
            pairs = [frozenset(x) for r in B.round_robin(n) for x in r]
            self.assertEqual(len(pairs), n * (n - 1) // 2)
            self.assertEqual(len(set(pairs)), len(pairs))
            for r in B.round_robin(n):   # nobody twice in a round
                flat = [i for x in r for i in x]
                self.assertEqual(len(flat), len(set(flat)))

    def test_snake_seeding_and_bad_setups(self):
        _m, groups = B.build_pools(8, 2, 2)
        self.assertEqual(groups, {"A": [0, 3, 4, 7], "B": [1, 2, 5, 6]})
        for n, pools, adv in ((3, 2, 1), (8, 1, 2), (8, 9, 1), (8, 2, 5), (8, 2, 0)):
            with self.assertRaises(ValueError):
                B.build_pools(n, pools, adv)

    def test_pools_then_bracket(self):
        for fmt in ("single", "double"):
            for n, pools, adv in ((8, 2, 2), (10, 3, 2), (16, 4, 2), (9, 2, 3), (6, 2, 1), (12, 4, 1)):
                with self.subTest(fmt=fmt, n=n, pools=pools, adv=adv):
                    b = B.Bracket()
                    b.setup(NAMES[:n], fmt, pools=pools, advance=adv)
                    played = play_out(b, lambda a, c: min(a, c, key=seed_of))
                    self.assertEqual(b.champion(), "P1")
                    ids = [p[0] for p in played]
                    first_bracket = next(i for i, x in enumerate(ids) if not x.startswith("P"))
                    self.assertTrue(all(x.startswith("P") for x in ids[:first_bracket]))
                    self.assertFalse(any(x.startswith("P") for x in ids[first_bracket:]))
                    v = b.view()
                    self.assertEqual(len(v["pools"]), pools)
                    self.assertTrue(all(g["done"] for g in v["pools"]))
                    went_on = {p for x in played[first_bracket:] for p in x[1:3]}
                    tops = {r["name"] for g in v["pools"] for r in g["standings"][:adv]}
                    self.assertEqual(went_on, tops)

    def test_bracket_waits_for_the_pool(self):
        b = B.Bracket()
        b.setup(NAMES[:8], "double", pools=2, advance=2)
        self.assertTrue(b.current().startswith("PA"))
        first = [r for r in b.view()["matches"] if not r["id"].startswith("P")]
        self.assertTrue(all(r["a"] == "" and r["b"] == "" for r in first))
        self.assertTrue(all(r.get("pool") for r in b.view()["matches"] if r["id"].startswith("P")))

    def test_tie_broken_head_to_head_then_games(self):
        b = B.Bracket()
        b.setup(["A", "B", "C", "D"], "single", pools=2, advance=1)   # pools: A,D / B,C
        rows, done = b.standings("A")
        self.assertFalse(done)
        self.assertEqual([r["name"] for r in rows], ["A", "D"])        # (seed before any results)
        b.admin_result("PA-1", "D")
        rows, done = b.standings("A")
        self.assertTrue(done)
        self.assertEqual([r["name"] for r in rows], ["D", "A"])
        self.assertIn("D", b.players_of("W1-1"))      # pool A is done: its winner is in the final
        self.assertIn("", b.players_of("W1-1"))       # (pool B isn't)

    def test_three_way_tie_on_games(self):
        b = B.Bracket()
        b.state.update(players=["A", "B", "C"], pools={"A": ["A", "B", "C"]}, results={}, games={},
                       matches={"PA-1": {"a": {"seed": 0}, "b": {"seed": 1}, "pool": "A", "label": "Pool A"},
                                "PA-2": {"a": {"seed": 1}, "b": {"seed": 2}, "pool": "A", "label": "Pool A"},
                                "PA-3": {"a": {"seed": 2}, "b": {"seed": 0}, "pool": "A", "label": "Pool A"}})
        # a cycle: A beat B, B beat C, C beat A, but C won 2-0 and A only 2-1
        for mid, w, lo, sc in (("PA-1", "A", "B", [2, 1]), ("PA-2", "B", "C", [2, 1]), ("PA-3", "C", "A", [2, 0])):
            b.state["results"][mid] = {"winner": w, "loser": lo, "score": sc}
        rows, done = b.standings("A")
        self.assertTrue(done)
        self.assertEqual([r["name"] for r in rows], ["C", "B", "A"])   # games +1, 0, -1
        self.assertEqual((rows[0]["games_won"], rows[0]["games_lost"]), (3, 2))

    def test_route_setup_with_pools(self):
        b = B.Bracket()
        b.key = "k"
        st, _ct, body = B.route(b, "POST", "/bracket/admin", json.dumps(
            {"key": "k", "action": "setup", "players": NAMES[:8], "pools": 2, "advance": 2}).encode())
        self.assertEqual(st, 200, body)
        v = json.loads(B.route(b, "GET", "/bracket.json", b"")[2])
        self.assertEqual([g["name"] for g in v["pools"]], ["A", "B"])
        self.assertEqual(v["advance"], 2)


class Reports(unittest.TestCase):
    def setUp(self):
        self.clock = Clock()
        self.b = B.Bracket(clock=self.clock)
        self.b.setup(["Ann", "Bo", "Cy", "Di"], "double", arena="12345", owner="Ann")

    def test_assignment(self):
        b = self.b
        self.assertEqual(b.assignment(" ann "), {"match": "W1-1", "opponent": "Di", "owner": True, "plus": True, "arena": "12345",
                                                 "place": 1, "role": "start", "game": 1})
        self.assertEqual(b.assignment("DI")["opponent"], "Ann")
        self.assertEqual((b.assignment("DI")["place"], b.assignment("DI")["role"]), (1, "join"))
        self.assertEqual((b.assignment("Zed")["place"], b.assignment("Zed")["role"]), (0, ""))
        self.assertEqual(b.assignment("Bo"), {"match": None, "opponent": "", "owner": False, "plus": False, "arena": "12345",
                                              "place": 3, "role": "", "game": 0})
        self.assertFalse(b.assignment("")["owner"])

    def test_owner_not_playing_still_presses_plus(self):
        b = self.b
        b.report("W1-1", "Ann", "Di", "Ann")
        b.report("W1-1", "Ann", "Di", "Di")
        a = b.assignment("Ann")
        self.assertEqual((a["match"], a["plus"], a["owner"]), (None, True, True))

    def test_both_agree(self):
        b = self.b
        self.assertEqual(b.report("W1-1", "Di", "Ann", "Ann"), (200, "pending"))
        self.assertEqual(b.current(), "W1-1")
        self.assertEqual(b.report("W1-1", "di", "ANN", "Di", {"stocks": 2, "percent": 37.5}), (200, "confirmed"))
        self.assertEqual(b.state["results"]["W1-1"]["winner"], "Di")
        self.assertEqual(b.state["results"]["W1-1"]["stocks"], 2)
        self.assertEqual(b.current(), "W1-2")

    def test_one_report_confirms_after_a_while(self):
        b = self.b
        b.report("W1-1", "Ann", "Di", "Ann")
        self.clock.t += B.CONFIRM_AFTER - 1
        self.assertEqual(b.current(), "W1-1")
        self.clock.t += 1
        self.assertEqual(b.current(), "W1-2")
        self.assertEqual(b.state["results"]["W1-1"]["how"], "one report")

    def test_disagreement_waits_for_the_organizer(self):
        b = self.b
        b.report("W1-1", "Ann", "Di", "Ann")
        self.assertEqual(b.report("W1-1", "Di", "Ann", "Di"), (200, "disputed"))
        self.clock.t += 1000
        self.assertEqual(b.current(), "W1-1")
        self.assertEqual(b.view()["matches"][0]["state"], "disputed")
        b.admin_result("W1-1", "Di")
        self.assertEqual(b.current(), "W1-2")
        self.assertEqual(b.state["results"]["W1-1"]["how"], "organizer")

    def test_refused_reports(self):
        b = self.b
        self.assertEqual(b.report("W1-2", "Bo", "Cy", "Bo")[0], 409)        # not being played
        self.assertEqual(b.report("W1-1", "Ann", "Bo", "Ann")[0], 400)      # Bo isn't in it
        self.assertEqual(b.report("W1-1", "Ann", "Ann", "Ann")[0], 400)
        self.assertEqual(b.report("W1-1", "Ann", "Di", "Bo")[0], 403)       # only the players
        self.assertEqual(b.report("W1-1", None, "Di", "Ann")[0], 400)

    def test_organizer_changes_an_old_result(self):
        b = self.b
        for mid in ("W1-1", "W1-2"):
            a, c = b.players_of(mid)
            b.admin_result(mid, a)
        b.admin_result("W2-1", "Ann")      # Ann beat Bo
        self.assertIn("L2-1", b.state["matches"])
        b.admin_result("L1-1", "Cy")
        self.assertEqual(b.players_of("L2-1"), ("Cy", "Bo"))
        b.admin_result("W1-2", "Cy")        # fixed: Cy beat Bo in round 1
        self.assertEqual(set(b.state["results"]), {"W1-1", "W1-2"})
        self.assertEqual(b.players_of("W2-1"), ("Ann", "Cy"))
        self.assertEqual(b.current(), "W2-1")             # (winners' side first)
        with self.assertRaises(ValueError):
            b.admin_result("GF", "Ann")      # players not known
        with self.assertRaises(ValueError):
            b.admin_result("W1-1", "Bo")

    def test_clear(self):
        b = self.b
        b.admin_result("W1-1", "Ann")
        b.clear("W1-1")
        self.assertEqual(b.current(), "W1-1")


class Sets(unittest.TestCase):
    """Best-of sets: each game reported as a match was, the set to whoever won more than half."""

    def setUp(self):
        self.clock = Clock()
        self.b = B.Bracket(clock=self.clock)
        self.b.setup(["Ann", "Bo", "Cy", "Di"], "single", best=3, finals_best=5)

    def game(self, mid, w, lo, g=None):
        extra = {"game": g} if g is not None else None
        self.assertEqual(self.b.report(mid, w, lo, w, extra), (200, "pending"))
        return self.b.report(mid, w, lo, lo, extra)

    def test_lengths(self):
        self.assertEqual({k: m["best_of"] for k, m in self.b.state["matches"].items()}, {"W1-1": 3, "W1-2": 3, "W2-1": 5})
        b = B.Bracket()
        b.setup(NAMES[:4], "double", best=1, finals_best=3)
        self.assertEqual({k for k, m in b.state["matches"].items() if m["best_of"] == 3}, {"W2-1", "L2-1", "GF", "GF2"})
        self.assertTrue(all(m["label"] in B.FINALS for m in b.state["matches"].values() if m["best_of"] == 3))
        for n in (0, 2, 11, "x"):
            with self.assertRaises(ValueError):
                b.setup(NAMES[:4], best=n)

    def test_set_over_at_two_of_three(self):
        b = self.b
        self.assertEqual(self.game("W1-1", "Ann", "Di"), (200, "confirmed"))
        self.assertEqual((b.current(), b.assignment("Di")["game"]), ("W1-1", 2))
        row = b.view()["matches"][0]
        self.assertEqual((row["state"], row["game"], row["games"], row["score"]), ("playing", 2, ["Ann"], [1, 0]))
        self.game("W1-1", "Di", "Ann", 2)
        self.assertEqual(b.assignment("Ann")["game"], 3)
        self.game("W1-1", "Ann", "Di", 3)
        self.assertEqual(b.state["results"]["W1-1"]["winner"], "Ann")
        self.assertEqual(b.state["results"]["W1-1"]["score"], [2, 1])
        self.assertEqual(b.current(), "W1-2")
        self.assertEqual(b.view()["matches"][0]["score"], [2, 1])

    def test_late_and_future_game_reports(self):
        b = self.b
        self.game("W1-1", "Ann", "Di", 1)
        self.assertEqual(b.report("W1-1", "Ann", "Di", "Di", {"game": 1}), (200, "confirmed"))   # late: game 1 stands
        self.assertEqual(b.report("W1-1", "Di", "Ann", "Di", {"game": 1}), (200, "confirmed"))
        self.assertEqual(b.games("W1-1")[0]["winner"], "Ann")
        self.assertEqual(b.report("W1-1", "Ann", "Di", "Ann", {"game": 3})[0], 409)              # not on yet
        self.assertNotIn("W1-1", b.reports)
        self.assertEqual(b.report("W1-1", "Ann", "Di", "Ann", {"game": 2}), (200, "pending"))
        self.clock.t += B.CONFIRM_AFTER
        self.assertEqual(b.current(), "W1-2")                                                     # one report stood
        self.assertEqual(b.state["results"]["W1-1"]["how"], "one report")

    def test_organizer_game_and_set(self):
        b = self.b
        self.game("W1-1", "Ann", "Di")
        b.report("W1-1", "Di", "Ann", "Di")
        self.assertEqual(b.report("W1-1", "Ann", "Di", "Ann"), (200, "disputed"))
        b.admin_game("W1-1", "Di")
        self.assertEqual((b.current(), b.game("W1-1")), ("W1-1", 3))
        self.assertNotIn("W1-1", b.disputed)
        with self.assertRaises(ValueError):
            b.admin_game("W2-1", "Ann")      # players not known
        b.admin_result("W1-1", "Ann")        # the organizer's set at 1-1: no score
        self.assertNotIn("score", b.state["results"]["W1-1"])
        row = b.view()["matches"][0]
        self.assertEqual((row["winner"], row["games"]), ("Ann", ["Ann", "Di"]))
        self.assertNotIn("score", row)
        with self.assertRaises(ValueError):
            b.admin_game("W1-1", "Ann")      # decided

    def test_clear_drops_games(self):
        b = self.b
        self.game("W1-1", "Ann", "Di")
        self.game("W1-1", "Ann", "Di")
        self.game("W1-2", "Bo", "Cy")
        b.clear("W1-1")
        self.assertEqual((b.games("W1-1"), b.current(), b.game("W1-1")), ([], "W1-1", 1))
        self.assertEqual(len(b.games("W1-2")), 1)          # (not a dependent)

    def test_final_best_of_five(self):
        b = self.b
        for mid in ("W1-1", "W1-2"):
            a, c = b.players_of(mid)
            b.admin_result(mid, a)
        for g, w in enumerate(["Ann", "Bo", "Ann", "Bo"], 1):
            lo = "Bo" if w == "Ann" else "Ann"
            self.game("W2-1", w, lo, g)
            self.assertIsNone(b.champion())
        self.game("W2-1", "Bo", "Ann", 5)
        self.assertEqual((b.champion(), b.state["results"]["W2-1"]["score"]), ("Bo", [3, 2]))


class Saved(unittest.TestCase):
    def test_survives_a_restart(self):
        with tempfile.TemporaryDirectory() as d:
            b = B.Bracket(d)
            key = b.key
            self.assertTrue(key and len(key) >= 12)
            b.setup(NAMES[:6], "double", arena="777", owner="P1")
            b.admin_result("W1-2", "P4")
            again = B.Bracket(d)
            self.assertEqual(again.key, key)
            self.assertEqual(again.view(), b.view())
            self.assertFalse(os.path.exists(os.path.join(d, "bracket.json.tmp")))


class Http(unittest.TestCase):
    """Through serve_http as the lobby calls it: the first four bytes already read."""

    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.b = B.Bracket(self.dir.name)
        self.b.setup(["Ann", "Bo", "Cy"], "double", arena="4242", owner="Ann")

    def tearDown(self):
        self.dir.cleanup()

    def exchange(self, raw):
        async def go():
            async def handle(reader, writer):
                head = await reader.readexactly(4)
                await B.serve_http(self.b, reader, writer, head, 2.0)
            server = await asyncio.start_server(handle, "127.0.0.1", 0)
            port = server.sockets[0].getsockname()[1]
            reader, writer = await asyncio.open_connection("127.0.0.1", port)
            writer.write(raw)
            await writer.drain()
            data = await asyncio.wait_for(reader.read(), 5)
            writer.close()
            server.close()
            await server.wait_closed()
            return data
        data = asyncio.run(go())
        head, _, body = data.partition(b"\r\n\r\n")
        return int(head.split()[1]), head.decode(), body

    def get(self, path):
        return self.exchange(f"GET {path} HTTP/1.1\r\nHost: x\r\n\r\n".encode())

    def post(self, path, obj):
        body = json.dumps(obj).encode()
        return self.exchange(f"POST {path} HTTP/1.1\r\nHost: x\r\nContent-Length: {len(body)}\r\n\r\n".encode() + body)

    def test_player_flow(self):
        st, _, body = self.get("/arena/assignment?name=Bo")
        self.assertEqual(st, 200)
        self.assertEqual(json.loads(body), {"match": "W1-2", "opponent": "Cy", "owner": False, "plus": False, "arena": "4242",
                                            "place": 1, "role": "start", "game": 1})
        st, _, body = self.post("/arena/result", {"match": "W1-2", "winner": "Cy", "loser": "Bo", "reporter": "Bo"})
        self.assertEqual((st, json.loads(body)), (200, {"ok": True, "state": "pending"}))
        st, _, body = self.post("/arena/result", {"match": "W1-2", "winner": "Cy", "loser": "Bo", "reporter": "Cy"})
        self.assertEqual(json.loads(body)["state"], "confirmed")
        st, _, body = self.post("/arena/result", {"match": "W1-2", "winner": "Cy", "loser": "Bo", "reporter": "Cy"})
        self.assertEqual(st, 409)
        v = json.loads(self.get("/bracket.json")[2])
        self.assertEqual(v["current"], "W2-1")
        self.assertEqual(B.Bracket(self.dir.name).state["results"]["W1-2"]["winner"], "Cy")

    def test_page_and_admin(self):
        st, head, body = self.get("/bracket")
        self.assertEqual(st, 200)
        self.assertIn("text/html", head)
        self.assertNotIn(self.b.key.encode(), body)
        self.assertIn(b'const KEY="",', body)
        st, _, body = self.get("/bracket?key=" + self.b.key)
        self.assertIn(b'const KEY="' + self.b.key.encode() + b'", ADMIN="1"==="1"', body)
        self.assertEqual(self.post("/bracket/admin", {"key": "nope", "action": "clear", "match": "W1-2"})[0], 403)
        st, _, body = self.post("/bracket/admin", {"key": self.b.key, "action": "result", "match": "W1-2", "winner": "Bo"})
        self.assertEqual(st, 200)
        self.assertEqual(json.loads(body)["bracket"]["current"], "W2-1")
        st, _, body = self.post("/bracket/admin", {"key": self.b.key, "action": "setup", "players": ["X", "Y", "", "Z", "W"],
                                                   "format": "single", "arena": "9", "owner": "W"})
        v = json.loads(body)["bracket"]
        self.assertEqual((v["players"], v["format"], v["arena"], v["current"]), (["X", "Y", "Z", "W"], "single", "9", "W1-1"))
        self.assertEqual(self.post("/bracket/admin", {"key": self.b.key, "action": "setup", "players": ["X"]})[0], 400)
        self.assertEqual(self.post("/bracket/admin", {"key": self.b.key, "action": "explode"})[0], 400)

    def test_bad_requests(self):
        self.assertEqual(self.get("/nothing")[0], 404)
        self.assertEqual(self.exchange(b"POST /arena/result HTTP/1.1\r\nContent-Length: 5\r\n\r\n{nope")[0], 400)
        self.assertEqual(self.exchange(b"POST /arena/result HTTP/1.1\r\nContent-Length: 99999\r\n\r\n")[0], 400)
        self.assertEqual(self.exchange(b"POST /arena/result HTTP/1.1\r\nContent-Length: 2\r\n\r\n[]")[0], 400)


class ThroughTheLobby(unittest.TestCase):
    """The online lobby's TCP port hands HTTP to the bracket and keeps NEX streams' preface check."""

    def test_lobby_port(self):
        import ssbu_online_lobby as online
        with tempfile.TemporaryDirectory() as tmp:
            lobby = online.OnlineLobby(online.parse_args(["--state", tmp, "--nex-log", "0"]))
            lobby.say = lambda *a, **k: None
            lobby.bracket.setup(["Ann", "Bo"], "single", arena="1", owner="Bo")

            async def go(raw):
                server = await asyncio.start_server(lobby.stream, "127.0.0.1", 0)
                port = server.sockets[0].getsockname()[1]
                reader, writer = await asyncio.open_connection("127.0.0.1", port)
                writer.write(raw)
                await writer.drain()
                data = await asyncio.wait_for(reader.read(), 5)
                writer.close()
                server.close()
                await server.wait_closed()
                return data
            data = asyncio.run(go(b"GET /arena/assignment?name=bo HTTP/1.1\r\n\r\n"))
            self.assertTrue(data.startswith(b"HTTP/1.1 200"), data[:40])
            self.assertEqual(json.loads(data.partition(b"\r\n\r\n")[2])["opponent"], "Ann")
            self.assertEqual(asyncio.run(go(b"SSBUNEX1" + bytes(8))), b"")    # (not a known player: closed)
            self.assertEqual(lobby.handshakes, {})
            self.assertTrue(os.path.exists(os.path.join(tmp, "bracket.key")))


if __name__ == "__main__":
    unittest.main()
