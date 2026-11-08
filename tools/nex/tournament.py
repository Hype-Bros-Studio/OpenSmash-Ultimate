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

from .streams import StreamOut, datetime_of

EVENT_CONVENTION = 4


class Tourney:
    def __init__(self, nex):
        self.nex = nex
        self.event_id = 1
        self.name = "HypeSmash Tourney"
        self.description = "Best of 3, 1-on-1"
        self.event_type = int(os.environ.get("HS_EVENT_TYPE", "1"), 0)
        self.rules = {"stock": 3, "time_sec": 420}

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
    def register(self, nex):
        self.next_entry = 1
        for m in (9, 10, 11, 13, 14):
            nex.register(201, m, self.method_u32(m))
        nex.register(201, 12, self.method_list(12))

    def method_u32(self, m):
        def handler(conn, s):
            body = s.read(s.left())
            self.nex.info(f"Tournament.{m} from {self.nex.name(conn)}: {self.nex.shown(conn, body, 512, 201)}")
            v = 0
            if m == 9:
                v = self.next_entry
                self.next_entry += 1
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
            out.u32(0)
            return out.get()
        return handler
