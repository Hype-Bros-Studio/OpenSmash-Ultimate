"""Online Tourney (the game's own "convention" mode) served from the lobby.

The game learns about a tourney from Utility.GetGameEvents (110/11) at login: one GameEvent whose second
u32 has bit 4 set becomes the Online Tourney (main+0x171ff00 converts it, main+0x16836e0 stores it at
main+0x5307790). The Online menu's tile is enabled while date1 <= now <= date4, and the tourney is "in
session" while date2 <= now <= date3 too (main+0x1bef310 / main+0x1bef3f0). The rules come from the
event's Map<String, Variant> (keys read by the converter: melee_kind, melee_rule, time_sec, stock, ...,
tournament_entry_max_seconds, competition_aggregate_at, url1, url2).

That "convention" is not the Online Tourney tile, though. The tile (Lua is_in_period_online_tournament,
main+0x1bed840) reads tourney files delivered by BCAT, which the runtime serves from SSBU_BCAT_DIR
(runtime/hle_offline.c): every file in the delivery-cache directory "tournament" is loaded, whatever its
name. Known so far of a tourney file (main+0x1761f80 checks it, main+0x1bf02a0 reads it):
  +0x00 u32  file size (must equal the file's size)
  +0x0c u32  kind, <= 0x1f
  +0x10 u32  1 = needs a save-data flag; else 0
  +0x18 u64  overwritten by the game with the file's id (hash40 of "tournament/<name>")
  +0x20 date start, +0x28 date end: u16 year, u8 month, day, hour, minute, second (now must be inside)
  +0x40 u32  entry count, +0x44 u32 offset of the entries (0x4c bytes each)
  +0x48 (u32 offset, u32 size) per language (index from main+0x523d00c): a text blob, registered by
         main+0x33798d0 (likely the tourney's name and description)
The Tournament protocol (201) that runs entry and rounds is answered here as it is worked out."""
import os
import time

from .streams import StreamIn, StreamOut, datetime_of

EVENT_CONVENTION = 4


class Tourney:
    def __init__(self, nex):
        self.nex = nex
        self.event_id = 1
        self.name = "HypeSmash Tourney"
        self.description = "Best of 3, 1-on-1"
        self.event_type = int(os.environ.get("HS_EVENT_TYPE", "1"), 0)
        self.rules = {"stock": 3, "time_sec": 420}
        # HS_TOURNEY_RULES="stock=1,time_sec=60": the host's own rules (any key the converter reads)
        for kv in filter(None, os.environ.get("HS_TOURNEY_RULES", "").split(",")):
            k, _, v = kv.partition("=")
            self.rules[k.strip()] = int(v, 0)

    def game_event(self, out):
        now = time.time()
        dates = [now - 86400, now - 3600, now + 86400, now + 2 * 86400]
        # Online Tourney sessions: the tourney screen (main+0x16c9130) wants an event with type bit 1
        # whose period covers now; each period is one session (its own rules).
        periods = [(1, now - 3600, now + 86400)]

        def variant_s64(o, v):
            o.u8(1)
            o.s64(v)

        def period(o, p):
            pid, start, end = p

            def body(q):
                q.u32(pid)
                q.u32(0)
                q.datetime(datetime_of(time.gmtime(start)))
                q.datetime(datetime_of(time.gmtime(end)))
                q.map(self.rules, StreamOut.string, variant_s64)   # Map<String, Variant>
                q.map({}, StreamOut.string, StreamOut.string)       # Map<String, List<Variant>>
                q.u32(0)
            o.struct(body)

        def body(o):
            o.u32(self.event_id)
            o.u32(self.event_type)
            o.u16(0)
            o.string(self.name)
            o.string(self.description)
            for t in dates:
                o.datetime(datetime_of(time.gmtime(t)))
            o.map({}, StreamOut.string, variant_s64)                # Map<String, Variant>
            o.map({}, StreamOut.string, StreamOut.string)           # Map<String, List<Variant>>
            o.list(periods, period)                                 # List<GameEventPeriod>
        out.struct(body)

    def game_events(self):
        out = StreamOut()
        if os.environ.get('HS_NO_GAME_EVENTS'):   # (research: the BCAT tourney alone)
            out.u32(0)
            return out.get()
        out.u32(1)
        self.game_event(out)
        return out.get()


    # Tournament protocol (201). The client (TournamentProtocolClient, stubs main+0x2af540..0x2b0100 for
    # methods 9..14, result readers in the table filled at main+0x52b65f0) reads one u32 back from 9, 10,
    # 11, 13 and 14 and a list from 12 (main+0x2b1dc0 -> 0x2b03a0). Game-side callers: main+0x17375d0..
    # The round starts on a server push: Notification (14) method 1 with a type 201000..201999
    # (main+0x171f9d0 takes it): param1 must be the u32 the entry (method 9) returned, param2 goes
    # to the entry task's status byte (0xff until then), and the tourney manager's idle state
    # (main+0x16c3c00) then pushes the round state (main+0x16c5200).
    N_TOURNEY = 201000

    def register(self, nex):
        self.group = 1
        self.entrants = []          # pids, in entry order
        self.size = int(os.environ.get("HS_TOURNEY_SIZE", "2"))
        self.start_at = None
        self.round = 0
        # HS_TOURNEY_ONE_AT_A_TIME=1: one match is played at a time, in bracket order. A game
        # takes one notification per bracket level: the tourney manager moves on only when the
        # notified round is above the last one it saw (main+0x16c3d18), an entrant's match is on
        # when its level is that round (main+0x172be08), and the level is the match's column on
        # the bracket screen (any other level overruns its layout: std::out_of_range). So only
        # the pair whose match is up hears of it; first-round players wait in the entry room
        # (CPU practice) until called, later ones on the bracket screen after their win.
        # (Best of 3 needs a second notification at the same level: not without a game patch.)
        self.one_at_a_time = os.environ.get("HS_TOURNEY_ONE_AT_A_TIME", "") not in ("", "0")
        self.waves = self.one_at_a_time
        self.reset()
        for m in (9, 10, 11, 13, 14):
            nex.register(201, m, self.method_u32(m))
        nex.register(201, 12, self.method_list(12))

    def method_u32(self, m):
        def handler(conn, s):
            body = s.read(s.left())
            self.nex.info(f"Tournament.{m} from {self.nex.name(conn)}: {self.nex.shown(conn, body, 512, 201)}")
            v = 0
            if m == 9:
                v = self.group
                if not self.entrants or getattr(self, "done", False):
                    self.reset()     # a new tourney: forget the last one's entrants, rounds and results
                if conn.pid not in self.entrants and not getattr(self, "started", False):
                    self.entrants.append(conn.pid)
                    self.names[conn.pid] = self.nex.name(conn)
                # (an eliminated player entering again waits for the next tourney; it must not
                # restart this one's round notification)
                if len(self.entrants) >= self.size and self.start_at is None and not getattr(self, "started", False):
                    self.start_at = time.monotonic() + 2.0   # after the entry's reply is through
            elif m == 14:
                self.report(conn, body)
                v = 1
            elif m in (13, 14):
                # match start (13) and result (14): the game reads the reply into its entry
                # task (+0x144) and, if it is 0, shows a communication error (-989).
                v = 1
            v = int(os.environ.get(f"HS_T{m}", v), 0) if isinstance(os.environ.get(f"HS_T{m}"), str) else v
            out = StreamOut()
            out.u32(v)
            return out.get()
        return handler

    def method_list(self, m):
        def handler(conn, s):
            body = s.read(s.left())
            self.nex.info(f"Tournament.{m} from {self.nex.name(conn)}: {self.nex.shown(conn, body, 512, 201)}")
            out = StreamOut()
            self.bracket(out)
            return out.get()
        return handler

    def bracket(self, out):
        """Method 12 reply (reader 0x2b03a0, copied by 0x170a1a0 into the tourney manager):
        struct { u32 group, u32 round, u8 kind, u8, u32,
                 Map<u32 id, match>  (struct { u32 id, u32 round, u8, u8 (0xff), u8, List<u32> slots,
                                        u8 state (<=6), u64 }),
                 Map<u32 id, entrant> (struct { u32 id, u64 pid, u32, u16, u8, u8 (<=2), u8 (<=4),
                                         List<player> (<=2: struct { u8 fighter, u8 color,
                                         u8 state (2 = none), String name, qBuffer x3 }) }) }
        The game wants 8/16/32 entrants when matches are 1v1, so the bracket is padded."""
        slots = 8
        while slots < len(self.entrants):
            slots *= 2
        ids = list(range(1, slots + 1))
        rnd = self.round
        tree = self.tree(slots)

        def match(o, m):
            def body(q):
                q.u32(m["id"])
                # +4: the id of the match its winner goes on to (0 for the final); the bracket
                # screen after a game (main+0x239acc0) follows it from the finished match.
                q.u32(m["next"])
                # the match's level (0 = first round): the bracket screen lays matches out by it
                # (main+0x16c528c) and an entrant's own match is the first one listing it whose
                # level is the bracket's round (main+0x172be08).
                q.u8(m["level"])
                q.u8(0xff)
                q.u8(0)
                # the game copies a match's slots only when the list grows (main+0x170a3dc),
                # so later rounds list just the entrants decided so far.
                shown = m["slots"] if m["level"] == 0 else [e for e in m["slots"] if e]
                q.list(shown, lambda r, v: r.u32(v))
                # state (<=6, kept at +9): 5 = decided; the bracket screen waits for it.
                q.u8(5 if m["winner"] is not None else 0)
                q.u64(0)
            o.struct(body)

        def entrant(o, i):
            pid = self.entrants[i] if i < len(self.entrants) else 0
            conn = self.nex.by_pid.get(pid) if pid else None

            def player(r, _):
                def pb(t):
                    t.u8(int(os.environ.get("HS_TFIGHTER", "0xff"), 0))
                    t.u8(0)
                    t.u8(0)
                    t.string(self.nex.name(conn) if conn else "")
                    t.qbuffer(b"")
                    t.qbuffer(b"")
                    t.qbuffer(b"")
                r.struct(pb)

            def body(q):
                q.u32(ids[i])
                q.u64(self.own_id(pid))
                q.u32(0)
                q.u16(0)
                q.u8(0)
                q.u8(0)
                q.u8(0)
                q.list([0] if pid else [], player)
            o.struct(body)

        def top(o):
            o.u32(self.group)
            o.u32(rnd)
            o.u8(0)
            # the manager keeps this byte at +0x549 and compares it with the notified round
            # (main+0x16c3d0c, main+0x16c54d8): it has to be the round the notification named.
            o.u8(rnd)
            # the champion's entrant id (kept at +0x54c): after the final the bracket screen
            # (main+0x239acc0, final match) waits for it to show up as the final's winner.
            o.u32(getattr(self, "winners", {}).get(1, 0))
            o.u32(len(tree))           # Map<u32 id, match>: the whole tree, by id
            for m in sorted(tree, key=lambda m: m["id"]):
                o.u32(m["id"])
                match(o, m)
            o.u32(slots)               # Map<u32 id, entrant>
            for i in range(slots):
                o.u32(ids[i])
                entrant(o, i)
        out.struct(top)

    def tree(self, slots):
        """The single-elimination tree the game draws (4 + 2 + 1 matches for 8 slots, ids in heap
        order: final 1, semis 2-3, first round 4-7): level 0 pairs the entry order, later levels hold the winners decided so
        far. A side with no entrant (padding) is a bye: its opponent goes through undecided play."""
        winners = getattr(self, "winners", {})
        present = set(range(1, len(self.entrants) + 1))
        levels, n, base = [], slots // 2, 1
        while n >= 1:
            # heap order, as the bracket screen counts them (main+0x239acc0): the final is 1,
            # a level of n matches holds ids n..2n-1, and match k goes on to k // 2.
            levels.append([dict(id=n + j, level=len(levels)) for j in range(n)])
            base += n
            n //= 2
        out = []
        for k, lv in enumerate(levels):
            for j, m in enumerate(lv):
                m["next"] = levels[k + 1][j // 2]["id"] if k + 1 < len(levels) else 0
                if k == 0:
                    m["slots"] = [2 * j + 1, 2 * j + 2]
                else:
                    a, b = levels[k - 1][2 * j], levels[k - 1][2 * j + 1]
                    m["slots"] = [a["winner"] or 0, b["winner"] or 0]
                real = [e for e in m["slots"] if e in present]
                if m["id"] in winners:
                    m["winner"] = winners[m["id"]]
                elif k == 0 and len(real) < 2:
                    # a bye; with nobody real in it a padding entrant goes on, since the bracket
                    # screen (main+0x239acc0) waits for every match's winner to show up next
                    m["winner"] = real[0] if real else m["slots"][0]
                elif k > 0 and len(real) < 2 and all(
                        c["winner"] is not None for c in (levels[k - 1][2 * j], levels[k - 1][2 * j + 1])):
                    m["winner"] = real[0] if real else m["slots"][0]
                else:
                    m["winner"] = None
                out.append(m)
        return out

    def report(self, conn, body):
        """Method 14, a game's result: struct {u32 group, u32 a, u32 b, u64 c} (worker main+0x1737cb0;
        a and b come from the match, b looks like the winner's entrant id). Once every player of the
        match has reported, the next game (best of 3) or round is announced."""
        st = StreamIn(body)
        _v, raw = st.struct()
        q = StreamIn(raw)
        group, a, b, c = q.u32(), q.u32(), q.u32(), q.u64()
        self.nex.info(f"Tournament: result from {self.nex.name(conn)}: group {group} a {a} b {b} c {c}")
        self.reports = getattr(self, "reports", {})
        self.reports[conn.pid] = (a, b, c)
        if self.waves:
            return self.report_game(a, b)
        tree = self.tree(self.slots())
        m = next((m for m in tree if m["id"] == a), None)
        players = [self.entrants[e - 1] for e in (m["slots"] if m else []) if 0 < e <= len(self.entrants)]
        if m is None or m["winner"] is not None or not all(p in self.reports for p in players):
            return
        self.games = getattr(self, "games", [])
        self.games.append({p: self.reports.pop(p) for p in players})
        self.nex.info(f"Tournament: match {a} (level {m['level']}) done: {self.games[-1]}")
        # b is the winner's entrant id, a the match's id (both players agree)
        self.winners = getattr(self, "winners", {})
        self.winners[a] = b
        open_ = [m for m in self.tree(self.slots()) if m["winner"] is None]
        if not open_:
            self.nex.info(f"Tournament: done, champion entrant {b}")
            self.done = True
        elif open_[0]["level"] == self.round:
            return        # the round's other matches are still on
        # the tourney manager (main+0x16c3c00) only moves on when the notified round is above the
        # last one it saw ([+0x549]): the next round is the first level with a match to play.
        self.round = open_[0]["level"] if open_ else self.round + 1
        self.start_at = time.monotonic() + float(os.environ.get("HS_TNEXT_DELAY", "5"))

    def reset(self):
        self.entrants, self.names, self.started, self.done = [], {}, False, False
        self.round, self.winners, self.reports, self.games = 0, {}, {}, []
        self.active = set()       # (one at a time) the match being played

    def players_of(self, m):
        return [self.entrants[e - 1] for e in m["slots"] if 0 < e <= len(self.entrants)]

    def next_wave(self):
        """(one at a time) The match to play next: the first undecided one of the lowest level
        whose two players are known."""
        ready = [m for m in self.tree(self.slots())
                 if m["winner"] is None and len(self.players_of(m)) == 2]
        if not ready:
            return []
        low = min(m["level"] for m in ready)
        ready = sorted((m for m in ready if m["level"] == low), key=lambda m: m["id"])
        return [m["id"] for m in ready[:1]]

    def report_game(self, a, b):
        """(one at a time) Once both players of match a reported, b goes on; the next match is
        announced to its two players."""
        if a not in self.active:
            return
        m = next((m for m in self.tree(self.slots()) if m["id"] == a), None)
        players = self.players_of(m) if m else []
        if not players or not all(p in self.reports for p in players):
            return
        self.games.append({p: self.reports.pop(p) for p in players})
        self.active.discard(a)
        self.winners[a] = b
        self.nex.info(f"Tournament: match {a} (level {m['level']}) won by entrant {b}")
        if not self.next_wave():
            self.nex.info(f"Tournament: done, champion entrant {self.winners.get(1)}")
            self.done = True
            return
        self.start_at = time.monotonic() + float(os.environ.get("HS_TNEXT_DELAY", "5"))

    def view(self):
        """The tourney for the bracket web page (tools/bracket.py's /bracket.json shape), or None
        when nobody has entered."""
        if not self.entrants:
            return None
        name = lambda e: self.names.get(self.entrants[e - 1], f"#{e}") if 0 < e <= len(self.entrants) else ""  # noqa: E731
        tree = self.tree(self.slots())
        depth = max(m["level"] for m in tree)
        rows, current = [], None
        for m in sorted(tree, key=lambda m: (m["level"], m["id"])):
            real = self.players_of(m)
            if m["level"] == 0 and len(real) < 2:
                continue      # a bye
            j = m["id"] - (1 << (depth - m["level"]))
            mid = f"W{m['level'] + 1}-{j + 1}"
            label = ("Final" if m["level"] == depth else "Semifinal" if m["level"] == depth - 1
                     else f"Round {m['level'] + 1}")
            on = m["id"] in self.active if self.waves else (
                getattr(self, "started", False) and m["winner"] is None and m["level"] == self.round and len(real) == 2)
            st = "done" if m["winner"] is not None else "playing" if on else "ready" if len(real) == 2 else "waiting"
            if on and current is None:
                current = mid
            row = {"id": mid, "label": label, "a": name(m["slots"][0]), "b": name(m["slots"][1]), "state": st}
            # (bracket.py's: the side, and where its players come from: the matches below, or seats)
            lv = m["level"]
            row["side"], row["from"] = "winners", ([{"winner": f"W{lv}-{2 * j + k + 1}"} for k in (0, 1)] if lv
                                                   else [{"seat": 2 * j + k} for k in (0, 1)])
            if m["winner"] is not None:
                row["winner"] = name(m["winner"])
            rows.append(row)
        champ = self.winners.get(1)
        return {"source": "tourney", "players": [self.names.get(p, "") for p in self.entrants], "format": "single",
                "one_at_a_time": self.one_at_a_time, "arena": "", "owner": "",
                "current": current, "champion": name(champ) if champ else None, "matches": rows}

    def slots(self):
        n = 8
        while n < len(self.entrants):
            n *= 2
        return n

    def opponents(self, pid):
        """The players a tourney entrant's search (game mode 12) may meet: those of its open match
        this round, or None when it is not in the tourney."""
        if pid not in self.entrants:
            return None
        me = self.entrants.index(pid) + 1
        for m in self.tree(self.slots()):
            on = m["id"] in self.active if self.waves else m["level"] == self.round
            if m["winner"] is None and on and me in m["slots"]:
                return {self.entrants[e - 1] for e in m["slots"] if 0 < e <= len(self.entrants)}
        return {pid}

    def own_id(self, pid):
        """The id an entrant's game knows itself by (0x172bcc0 looks itself up with it): its NSA
        id by default, HS_TOWN=pid for the NEX PID."""
        if not pid or os.environ.get("HS_TOWN") == "pid":
            return pid
        nsa = self.nex.accounts.nsa_of.get(pid)
        return int(nsa, 16) if nsa else pid

    def tick(self, now):
        trig = os.environ.get("HS_TNOTIFY_FILE")
        if trig and os.path.exists(trig):
            # testing: "<round> [kind]" in the file sends that notification to every entrant
            try:
                parts = open(trig).read().split()
                if not parts:
                    return        # still being written
                os.remove(trig)
                if not self.entrants:
                    raise ValueError("nobody has entered yet")
                self.round = int(parts[0], 0)
                self.notify_round(self.entrants[:self.size], self.round,
                                  int(parts[1], 0) if len(parts) > 1 else None)
            except (OSError, ValueError, IndexError) as e:
                self.nex.info(f"Tournament: bad trigger file: {e}")
        if getattr(self, "start_at", None) is None or time.monotonic() < self.start_at:
            return
        self.start_at = None
        self.started = True
        if not self.waves:
            self.notify_round(self.entrants[:self.size], self.round)
            return
        # (one at a time) the round named is the match's level; only its two players hear of it
        self.active = set(self.next_wave())
        pids = []
        for m in self.tree(self.slots()):
            if m["id"] in self.active:
                self.round = m["level"]
                pids += self.players_of(m)
        self.nex.info(f"Tournament: match {sorted(self.active)} is up (round {self.round})")
        if pids:
            self.notify_round(pids, self.round)

    def notify_round(self, pids, rnd, kind=None):
        kind = self.N_TOURNEY if kind is None else kind
        body = StreamOut()

        def event(o):
            o.pid(pids[0])
            o.u32(kind)
            o.u64(self.group)
            o.u64(rnd)
            o.string("")
            o.u64(0)
        body.struct(event)
        for pid in pids:
            conn = self.nex.by_pid.get(pid)
            if conn is not None:
                self.nex.info(f"Tournament: notification {kind} group {self.group} round {rnd} to {self.nex.name(conn)}")
                self.nex.call(conn, 14, 1, body.get())
