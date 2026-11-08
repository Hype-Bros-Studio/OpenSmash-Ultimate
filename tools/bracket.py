"""A tournament bracket for Battle Arenas, kept by the online lobby (tools/ssbu_online_lobby.py).

One match at a time, everyone in one arena: the bracket says who plays next, the players' games
(their runtime) ask who their opponent is, walk into the ring, and report who won. The organizer
sets the bracket up and fixes results on a web page the lobby serves.

HTTP, on the lobby's TCP port (the lobby hands over any connection that starts with "GET " or
"POST"):

  GET  /arena/assignment?name=N  {"match": id|null, "opponent": name, "owner": bool, "plus": bool,
                                  "arena": arena ID}. "plus": N owns the arena and a match is ready.
  POST /arena/result             {"match", "winner", "loser", "reporter", "stocks"?, "percent"?}
                                 -> {"ok", "state": "pending"|"confirmed"|"disputed"}; 409 when the
                                 match isn't being played.
  GET  /bracket.json             the whole bracket, for the in-game overlay and the page.
  GET  /bracket                  the page (with ?key=K: the organizer's controls).
  POST /bracket/admin            {"key", "action": "setup"|"result"|"clear"|"arena", ...}

A result is confirmed when both players report the same winner, or when one report has stood for
CONFIRM_AFTER seconds without the other player disagreeing. Reports that disagree leave the match
"disputed" until the organizer enters the result.

Formats: "double" (double elimination, with a grand-final reset) and "single". Any number of
players from 2: the bracket is filled to a power of two with byes, seeded the usual way (1 v 8,
4 v 5, ...). Names compare without case or surrounding spaces.

The state is one JSON file (bracket.json in the lobby's --state folder), rewritten on each change;
the organizer's key is bracket.key beside it, made on first use."""
import json
import os
import re
import secrets
import time
import urllib.parse

CONFIRM_AFTER = 60.0
BYE = None          # a side that is a bye (no player)
MAX_BODY = 16384
MAX_PLAYERS = 64
MAX_NAME = 40


def norm(name):
    return (name or "").strip().casefold()


def seed_order(size):
    """Seeds 1..size in bracket order: 1 meets size, and the top seeds meet as late as can be."""
    order = [1]
    while len(order) < size:
        n = len(order) * 2 + 1
        order = [x for s in order for x in (s, n - s)]
    return order


def build(players, fmt="double"):
    """The matches for these players (in seed order): {id: match}. A match's sides "a"/"b" say
    where its players come from: {"seed": i} (0-based), {"winner": id} or {"loser": id}."""
    if fmt not in ("double", "single"):
        raise ValueError(f"format {fmt!r}: double or single")
    n = len(players)
    if n < 2:
        raise ValueError("a bracket needs two players")
    if n == 2 and fmt == "double":
        fmt = "single"     # (a double for two is a best-of with a reset: the organizer's call)
    size, k = 2, 1
    while size < n:
        size, k = size * 2, k + 1
    order = seed_order(size)
    m = {}

    def add(mid, a, b, label):
        m[mid] = {"id": mid, "a": a, "b": b, "label": label}

    w = lambda r, i: f"W{r}-{i + 1}"   # noqa: E731
    lb = lambda r, i: f"L{r}-{i + 1}"  # noqa: E731
    for i in range(size // 2):
        add(w(1, i), {"seed": order[2 * i] - 1}, {"seed": order[2 * i + 1] - 1}, "Round 1")
    for r in range(2, k + 1):
        name = "Final" if r == k and fmt == "single" else ("Winners final" if r == k else f"Round {r}")
        for i in range(size >> r):
            add(w(r, i), {"winner": w(r - 1, 2 * i)}, {"winner": w(r - 1, 2 * i + 1)}, name)
    if fmt == "single":
        return m
    # losers: round 1 pairs winners-round-1 losers; even rounds take the next winners round's losers
    # (in reverse, to put off rematches), odd rounds halve
    rounds = 2 * (k - 1)
    for i in range(size // 4):
        add(lb(1, i), {"loser": w(1, 2 * i)}, {"loser": w(1, 2 * i + 1)}, "Losers round 1")
    count = size // 4
    for r in range(2, rounds + 1):
        last = r == rounds
        if r % 2 == 0:
            wr = r // 2 + 1
            for i in range(count):
                add(lb(r, i), {"winner": lb(r - 1, i)}, {"loser": w(wr, count - 1 - i)},
                    "Losers final" if last else f"Losers round {r}")
        else:
            count //= 2
            for i in range(count):
                add(lb(r, i), {"winner": lb(r - 1, 2 * i)}, {"winner": lb(r - 1, 2 * i + 1)}, f"Losers round {r}")
    lfinal = lb(rounds, 0) if rounds else None
    add("GF", {"winner": w(k, 0)}, {"winner": lfinal} if lfinal else {"loser": w(k, 0)}, "Grand final")
    add("GF2", {"winner": "GF"}, {"loser": "GF"}, "Grand final reset")
    m["GF2"]["reset"] = True
    return m


class Bracket:
    def __init__(self, folder=None, clock=time.monotonic, wall=time.time):
        self.folder = folder
        self.clock, self.wall = clock, wall
        self.state = {"players": [], "format": "double", "matches": {}, "results": {}, "arena": "", "owner": ""}
        self.reports = {}        # match -> {reporter: {"winner", "loser", "at", ...}}
        self.disputed = set()
        self.key = None
        if folder:
            path = os.path.join(folder, "bracket.json")
            if os.path.exists(path):
                with open(path, encoding="utf-8") as f:
                    self.state.update(json.load(f))
            kp = os.path.join(folder, "bracket.key")
            if os.path.exists(kp):
                with open(kp, encoding="utf-8") as f:
                    self.key = f.read().strip() or None
            if not self.key:
                self.key = secrets.token_urlsafe(12)
                with open(kp, "w", encoding="utf-8") as f:
                    f.write(self.key + "\n")
        self.order = self._order()

    # --- state -------------------------------------------------------------------------------
    def save(self):
        if not self.folder:
            return
        path = os.path.join(self.folder, "bracket.json")
        tmp = path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(self.state, f, indent=1)
        os.replace(tmp, path)

    def setup(self, players, fmt="double", arena=None, owner=None):
        players = [p.strip() for p in players if p and p.strip()]
        if len(players) > MAX_PLAYERS or any(len(p) > MAX_NAME for p in players):
            raise ValueError("too many players or a name too long")
        if len({norm(p) for p in players}) != len(players):
            raise ValueError("two players with the same name")
        matches = build(players, fmt)
        self.state.update(players=players, format=fmt if len(players) > 2 else "single", matches=matches, results={})
        if arena is not None:
            self.state["arena"] = str(arena).strip()[:32]
        if owner is not None:
            self.state["owner"] = owner.strip()[:MAX_NAME]
        self.reports.clear()
        self.disputed.clear()
        self.order = self._order()
        self.save()

    def set_arena(self, arena=None, owner=None):
        if arena is not None:
            self.state["arena"] = str(arena).strip()[:32]
        if owner is not None:
            self.state["owner"] = owner.strip()[:MAX_NAME]
        self.save()

    # --- who plays ---------------------------------------------------------------------------
    def _order(self):
        """Match ids in the order they are played: by how deep their sources go, winners before
        losers, then by number."""
        ms = self.state["matches"]
        depth = {}

        def d(mid):
            if mid not in depth:
                depth[mid] = 0   # (no cycles; this only stops a bad file from recursing forever)
                depth[mid] = 1 + max((d(s[t]) for s in (ms[mid]["a"], ms[mid]["b"])
                                      for t in ("winner", "loser") if t in s and s[t] in ms), default=0)
            return depth[mid]
        kind = lambda mid: {"W": 0, "L": 1}.get(mid[0], 2)   # noqa: E731
        num = lambda mid: [int(x) for x in mid[1:].replace("-", " ").split() if x.isdigit()]  # noqa: E731
        return sorted(ms, key=lambda mid: (d(mid), kind(mid), num(mid)))

    def side(self, src):
        """The player (name) on a side, BYE, or "" while it isn't known yet."""
        if "seed" in src:
            i = src["seed"]
            return self.state["players"][i] if i < len(self.state["players"]) else BYE
        mid = src.get("winner") or src.get("loser")
        if mid not in self.state["matches"]:
            return ""
        r = self.outcome(mid)
        if r is None:
            return ""
        return r[0] if "winner" in src else r[1]

    def players_of(self, mid):
        m = self.state["matches"][mid]
        return self.side(m["a"]), self.side(m["b"])

    def needed(self, mid):
        """Whether the match is played at all: the grand-final reset only when the losers' side
        won the grand final."""
        m = self.state["matches"][mid]
        if m.get("reset"):
            r = self.outcome("GF")
            return r is not None and r[0] is not BYE and r[0] == self.side(self.state["matches"]["GF"]["b"])
        return True

    def outcome(self, mid):
        """(winner, loser) once known (byes decided by themselves), else None."""
        if mid in self.state["results"]:
            r = self.state["results"][mid]
            return r["winner"], r["loser"]
        if not self.needed(mid):
            return None
        a, b = self.players_of(mid)
        if a == "" or b == "":
            return None
        if a is BYE:
            return b, BYE
        if b is BYE:
            return a, BYE
        return None

    def ready(self, mid):
        if mid in self.state["results"] or not self.needed(mid):
            return False
        a, b = self.players_of(mid)
        return bool(a) and bool(b)     # (both known, neither a bye)

    def current(self):
        self.tick()
        for mid in self.order:
            if self.ready(mid):
                return mid
        return None

    def champion(self):
        ms = self.state["matches"]
        if not ms:
            return None
        if "GF" in ms:
            if "GF2" in self.state["results"]:
                return self.state["results"]["GF2"]["winner"]
            r = self.outcome("GF")
            if r and not self.needed("GF2"):
                return r[0]
            return None
        last = self.order[-1]
        r = self.outcome(last)
        return r[0] if r else None

    def assignment(self, name):
        me = norm(name)
        mid = self.current()
        owner = bool(me) and me == norm(self.state.get("owner"))
        out = {"match": None, "opponent": "", "owner": owner, "plus": False, "arena": self.state.get("arena", "")}
        if mid:
            a, b = self.players_of(mid)
            out["plus"] = owner
            if me == norm(a):
                out.update(match=mid, opponent=b)
            elif me == norm(b):
                out.update(match=mid, opponent=a)
        return out

    # --- results -----------------------------------------------------------------------------
    def _name_in(self, name, mid):
        a, b = self.players_of(mid)
        n = norm(name)
        return a if n == norm(a) else b if n == norm(b) else None

    def report(self, mid, winner, loser, reporter, extra=None):
        """A player's report. Returns (http status, state)."""
        self.tick()
        if mid != self.current():
            return 409, "not being played"
        w, lo, by = self._name_in(winner, mid), self._name_in(loser, mid), self._name_in(reporter, mid)
        if not w or not lo or w == lo:
            return 400, "winner and loser must be this match's two players"
        if not by:
            return 403, "only the match's players report"
        rep = {"winner": w, "loser": lo, "at": self.clock()}
        for k in ("stocks", "percent"):
            if extra and isinstance(extra.get(k), (int, float, list)):
                rep[k] = extra[k]
        mine = self.reports.setdefault(mid, {})
        mine[by] = rep
        wins = {r["winner"] for r in mine.values()}
        if len(wins) > 1:
            self.disputed.add(mid)
            return 200, "disputed"
        self.disputed.discard(mid)
        if len(mine) == 2:
            self._decide(mid, w, lo, "players", rep)
            return 200, "confirmed"
        return 200, "pending"

    def tick(self):
        now = self.clock()
        for mid, mine in list(self.reports.items()):
            if mid in self.disputed or mid in self.state["results"] or len(mine) != 1:
                continue
            (rep,) = mine.values()
            if now - rep["at"] >= CONFIRM_AFTER:
                self._decide(mid, rep["winner"], rep["loser"], "one report", rep)

    def _decide(self, mid, winner, loser, how, rep=None):
        r = {"winner": winner, "loser": loser, "how": how, "when": self.wall()}
        for k in ("stocks", "percent"):
            if rep and k in rep:
                r[k] = rep[k]
        self.state["results"][mid] = r
        self.reports.pop(mid, None)
        self.disputed.discard(mid)
        self.save()

    def dependents(self, mid):
        out, todo = set(), [mid]
        while todo:
            x = todo.pop()
            for y, m in self.state["matches"].items():
                if y not in out and any(s.get("winner") == x or s.get("loser") == x for s in (m["a"], m["b"])):
                    out.add(y)
                    todo.append(y)
        return out

    def admin_result(self, mid, winner):
        """The organizer's result for any match whose players are known; matches that followed
        from the old result are cleared."""
        if mid not in self.state["matches"]:
            raise ValueError(f"no match {mid}")
        a, b = self.players_of(mid)
        if not a or not b:
            raise ValueError(f"{mid}: its players aren't known yet")
        w = a if norm(winner) == norm(a) else b if norm(winner) == norm(b) else None
        if not w:
            raise ValueError(f"{winner!r} doesn't play {mid}")
        old = self.state["results"].get(mid)
        if old and old["winner"] != w:
            self.clear(mid, keep_self=True)
        self._decide(mid, w, b if w == a else a, "organizer")

    def clear(self, mid, keep_self=False):
        if mid not in self.state["matches"]:
            raise ValueError(f"no match {mid}")
        for x in self.dependents(mid) | (set() if keep_self else {mid}):
            self.state["results"].pop(x, None)
            self.reports.pop(x, None)
            self.disputed.discard(x)
        self.save()

    # --- views -------------------------------------------------------------------------------
    def view(self):
        # an Online Tourney on the lobby's NEX server shows instead while it has entrants
        # (tools/nex/tournament.py; the lobby sets .live)
        live = getattr(self, "live", None)
        v = live() if live else None
        if v:
            return v
        cur = self.current()
        rows = []
        for mid in self.order:
            m = self.state["matches"][mid]
            if not self.needed(mid) and mid not in self.state["results"]:
                continue
            a, b = self.players_of(mid)
            if a is BYE or b is BYE:
                continue
            res = self.state["results"].get(mid)
            st = ("done" if res else "playing" if mid == cur else "disputed" if mid in self.disputed
                  else "ready" if self.ready(mid) else "waiting")
            if mid == cur and mid in self.disputed:
                st = "disputed"
            row = {"id": mid, "label": m["label"], "a": a or "", "b": b or "", "state": st}
            if res:
                row.update({k: res[k] for k in ("winner", "how", "stocks", "percent") if k in res})
            if mid in self.reports:
                row["reports"] = {k: v["winner"] for k, v in self.reports[mid].items()}
            rows.append(row)
        return {"players": self.state["players"], "format": self.state["format"], "arena": self.state.get("arena", ""),
                "owner": self.state.get("owner", ""), "current": cur, "champion": self.champion(), "matches": rows}


# --- HTTP --------------------------------------------------------------------------------------

def _json(status, obj):
    return status, "application/json", json.dumps(obj).encode()


def route(b, method, target, body):
    """(status, content type, body bytes) for one request."""
    url = urllib.parse.urlsplit(target)
    q = dict(urllib.parse.parse_qsl(url.query))
    path = url.path.rstrip("/") or "/"
    try:
        data = json.loads(body.decode("utf-8")) if method == "POST" and body else {}
        if not isinstance(data, dict):
            raise ValueError("a JSON object")
    except ValueError as e:
        return _json(400, {"error": f"bad JSON: {e}"})
    if method == "GET" and path == "/arena/assignment":
        return _json(200, b.assignment(q.get("name", "")))
    if method == "POST" and path == "/arena/result":
        status, st = b.report(str(data.get("match", "")), data.get("winner"), data.get("loser"),
                              data.get("reporter"), data)
        return _json(status, {"ok": status == 200, "state": st} if status == 200 else {"ok": False, "error": st})
    if method == "GET" and path == "/bracket.json":
        return _json(200, b.view())
    if method == "GET" and path in ("/bracket", "/"):
        admin = bool(b.key) and secrets.compare_digest(q.get("key", ""), b.key)
        return 200, "text/html; charset=utf-8", page(b, q.get("key", "") if admin else "").encode()
    if method == "POST" and path == "/bracket/admin":
        if not b.key or not secrets.compare_digest(str(data.get("key", "")), b.key):
            return _json(403, {"error": "wrong key"})
        try:
            act = data.get("action")
            if act == "setup":
                b.setup(list(data.get("players") or []), data.get("format", "double"), data.get("arena"), data.get("owner"))
            elif act == "result":
                b.admin_result(str(data.get("match", "")), data.get("winner"))
            elif act == "clear":
                b.clear(str(data.get("match", "")))
            elif act == "arena":
                b.set_arena(data.get("arena"), data.get("owner"))
            else:
                return _json(400, {"error": f"action {act!r}"})
        except (ValueError, TypeError) as e:
            return _json(400, {"error": str(e)})
        return _json(200, {"ok": True, "bracket": b.view()})
    return _json(404, {"error": "not found"})


async def serve_http(b, reader, writer, head, timeout=10.0):
    """One HTTP/1.1 request whose first bytes (head) the caller already read; then close."""
    import asyncio
    status, ctype, out = 400, "text/plain", b"bad request"
    try:
        rest = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), timeout)
        req = head + rest
        if len(req) > 8192:
            raise ValueError("headers too long")
        lines = req.decode("latin-1").split("\r\n")
        method, target, _ver = lines[0].split(" ", 2)
        length = 0
        for line in lines[1:]:
            k, _, v = line.partition(":")
            if k.strip().lower() == "content-length":
                length = int(v.strip())
        if length < 0 or length > MAX_BODY:
            raise ValueError("body too long")
        body = await asyncio.wait_for(reader.readexactly(length), timeout) if length else b""
        status, ctype, out = route(b, method.upper(), target, body)
    except (asyncio.IncompleteReadError, asyncio.TimeoutError, asyncio.LimitOverrunError, ConnectionError):
        writer.close()
        return
    except ValueError as e:
        status, ctype, out = 400, "text/plain", str(e).encode()
    reason = {200: "OK", 400: "Bad Request", 403: "Forbidden", 404: "Not Found", 409: "Conflict"}.get(status, "")
    writer.write(f"HTTP/1.1 {status} {reason}\r\nContent-Type: {ctype}\r\nContent-Length: {len(out)}\r\n"
                 f"Cache-Control: no-store\r\nConnection: close\r\n\r\n".encode() + out)
    try:
        await asyncio.wait_for(writer.drain(), timeout)
    except (asyncio.TimeoutError, ConnectionError):
        pass
    writer.close()


def page(b, key):
    """The bracket page. It reloads /bracket.json every few seconds; with the key, it has the
    organizer's setup form and a winner button on every match."""
    if key and not re.fullmatch(r"[A-Za-z0-9_-]+", key):
        key = ""     # (a hand-made key with other characters: no controls rather than a broken script)
    return PAGE.replace("{{KEY}}", key).replace("{{ADMIN}}", "1" if key else "")


PAGE = r"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Bracket</title>
<style>
:root{--bg:#0f1115;--card:#181b22;--line:#2a2f3a;--text:#e8eaf0;--dim:#9aa1b2;--accent:#ffb020;--win:#3ccf7a;--bad:#ff5d5d}
@media (prefers-color-scheme: light){:root{--bg:#f4f5f8;--card:#fff;--line:#d9dce4;--text:#1a1d24;--dim:#5d6475;--accent:#c97a00;--win:#1f9d55;--bad:#d63c3c}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:15px/1.4 system-ui,sans-serif;padding:16px}
h1{font-size:20px;margin:0 0 4px}.sub{color:var(--dim);margin-bottom:16px}
.now{background:var(--card);border:2px solid var(--accent);border-radius:10px;padding:14px;margin-bottom:18px}
.now b{font-size:22px}.cols{display:flex;gap:16px;overflow-x:auto;padding-bottom:8px}
.col{min-width:190px}.col h2{font-size:13px;text-transform:uppercase;letter-spacing:.05em;color:var(--dim);margin:0 0 8px}
.m{background:var(--card);border:1px solid var(--line);border-radius:8px;margin-bottom:10px;overflow:hidden}
.m.playing{border-color:var(--accent)}.m.disputed{border-color:var(--bad)}
.p{display:flex;justify-content:space-between;padding:6px 10px;gap:8px}.p+.p{border-top:1px solid var(--line)}
.p.w{color:var(--win);font-weight:600}.p.l{color:var(--dim);text-decoration:line-through}
.tag{font-size:11px;color:var(--dim);padding:4px 10px;border-top:1px solid var(--line)}
button{background:var(--line);color:var(--text);border:0;border-radius:5px;padding:2px 8px;cursor:pointer;font:inherit;font-size:12px}
textarea,input,select{background:var(--card);color:var(--text);border:1px solid var(--line);border-radius:6px;padding:6px;font:inherit;width:100%}
.admin{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:14px;margin-top:20px;max-width:520px}
.admin label{display:block;margin:8px 0 4px;color:var(--dim);font-size:13px}.err{color:var(--bad)}
</style></head><body>
<h1 id="title">Bracket</h1><div class="sub" id="sub"></div>
<div class="now" id="now"></div>
<div class="cols" id="cols"></div>
<div class="admin" id="admin" hidden>
 <b>Organizer</b>
 <label>Players, best seed first, one per line (their online names)</label><textarea id="players" rows="8"></textarea>
 <label>Format</label><select id="fmt"><option value="double">Double elimination</option><option value="single">Single elimination</option></select>
 <label>Arena ID</label><input id="arena">
 <label>Arena owner (online name; presses + to start)</label><input id="owner">
 <p><button id="setup">Start a new bracket</button> <button id="savearena">Save arena only</button> <span id="msg" class="err"></span></p>
</div>
<script>
const KEY="{{KEY}}", ADMIN="{{ADMIN}}"==="1";
const $=id=>document.getElementById(id);
function esc(s){return String(s??"").replace(/[&<>"']/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;","'":"&#39;"}[c]))}
async function admin(body){const r=await fetch("/bracket/admin",{method:"POST",body:JSON.stringify(Object.assign({key:KEY},body))});
 const j=await r.json();$("msg").textContent=j.error||"";if(j.bracket)draw(j.bracket);}
function col(m){return m.id.startsWith("W")?"Winners "+m.id.split("-")[0].slice(1):m.id.startsWith("L")?"Losers "+m.id.split("-")[0].slice(1):"Finals"}
function draw(b){
 $("sub").textContent=b.source==="tourney"?"Online Tourney · single elimination"+(b.one_at_a_time?", one match at a time":"")+", "+b.players.length+" players":(b.arena?"Arena ID "+b.arena:"No arena ID yet")+(b.owner?" · owner "+b.owner:"")+" · "+(b.format==="double"?"double":"single")+" elimination, "+b.players.length+" players";
 const cur=b.matches.find(m=>m.id===b.current);
 $("now").innerHTML=b.champion?"Champion: <b>"+esc(b.champion)+"</b>":cur?"Now playing ("+esc(cur.label)+")<br><b>"+esc(cur.a)+"</b> vs <b>"+esc(cur.b)+"</b>"+(cur.state==="disputed"?' <span class="err">(reports disagree: the organizer decides)</span>':""):b.players.length?"Waiting":"No bracket yet";
 const groups={};for(const m of b.matches)(groups[col(m)]=groups[col(m)]||[]).push(m);
 $("cols").innerHTML=Object.entries(groups).map(([t,ms])=>'<div class="col"><h2>'+esc(t)+"</h2>"+ms.map(m=>{
  const cls=p=>m.winner?(m.winner===p?"w":"l"):"";
  const btn=p=>ADMIN&&p?' <button data-m="'+esc(m.id)+'" data-w="'+esc(p)+'">win</button>':"";
  return '<div class="m '+m.state+'"><div class="p '+cls(m.a)+'"><span>'+esc(m.a||"—")+"</span>"+btn(m.a)+'</div><div class="p '+cls(m.b)+'"><span>'+esc(m.b||"—")+"</span>"+btn(m.b)+'</div><div class="tag">'+esc(m.label)+(m.how?" · "+esc(m.how):"")+(m.state==="playing"?" · now":"")+(m.state==="disputed"?" · disputed":"")+"</div></div>"}).join("")+"</div>").join("");
 if(ADMIN&&!$("players").value){$("players").value=b.players.join("\n");$("fmt").value=b.format;$("arena").value=b.arena;$("owner").value=b.owner}
}
document.addEventListener("click",e=>{const t=e.target;if(t.dataset&&t.dataset.m)admin({action:"result",match:t.dataset.m,winner:t.dataset.w})});
if(ADMIN){$("admin").hidden=false;
 $("setup").onclick=()=>{if(confirm("Start a new bracket? Results so far are cleared."))admin({action:"setup",players:$("players").value.split("\n"),format:$("fmt").value,arena:$("arena").value,owner:$("owner").value})};
 $("savearena").onclick=()=>admin({action:"arena",arena:$("arena").value,owner:$("owner").value});}
async function load(){try{draw(await (await fetch("/bracket.json",{cache:"no-store"})).json())}catch(e){}}
load();setInterval(load,3000);
</script></body></html>
"""
