"""Online Tourney (the game's own "convention" mode) served from the lobby.

The game learns about a tourney from Utility.GetGameEvents (110/11) at login: one GameEvent whose second
u32 has bit 4 set becomes the Online Tourney (main+0x171ff00 converts it, main+0x16836e0 stores it at
main+0x5307790). The Online menu's tile is enabled while date1 <= now <= date4, and the tourney is "in
session" while date2 <= now <= date3 too (main+0x1bef310 / main+0x1bef3f0). The rules come from the
event's Map<String, Variant> (keys read by the converter: melee_kind, melee_rule, time_sec, stock, ...,
tournament_entry_max_seconds, competition_aggregate_at, url1, url2).

The Tournament protocol (201) that runs entry and rounds is answered here as it is worked out."""
import time

from .streams import StreamOut, datetime_of

EVENT_CONVENTION = 4


class Tourney:
    def __init__(self, nex):
        self.nex = nex
        self.event_id = 1
        self.name = "HypeSmash Tourney"
        self.description = "Best of 3, 1-on-1"

    def game_event(self, out):
        now = time.time()
        dates = [now - 86400, now - 3600, now + 86400, now + 2 * 86400]

        def body(o):
            o.u32(self.event_id)
            o.u32(EVENT_CONVENTION)
            o.u16(0)
            o.string(self.name)
            o.string(self.description)
            for t in dates:
                o.datetime(datetime_of(time.gmtime(t)))
            o.map({}, StreamOut.string, StreamOut.string)    # Map<String, Variant>
            o.map({}, StreamOut.string, StreamOut.string)    # Map<String, List<Variant>>
            o.list([], None)                                  # List<GameEventPeriod>
        out.struct(body)

    def game_events(self):
        out = StreamOut()
        out.u32(1)
        self.game_event(out)
        return out.get()
