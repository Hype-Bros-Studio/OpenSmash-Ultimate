"""Mod sync between two PCs over HTTP: on the same network, or over Tailscale.

One PC shares its mods (`serve`); the other brings its own mods to match (`pull`). Only files that
differ are sent, each checked by its sha256. What a pull changes can be undone (`undo`).

    python tools/modsync.py serve [--port 39440] [--all] [--key KEY]
    python tools/modsync.py pull HOST[:PORT] KEY [--dry-run] [--no-apply]
    python tools/modsync.py undo [--no-apply]

`serve` prints the addresses to give a friend and a short key; nothing is reachable without it.
It only ever sends files from the mods it lists (the enabled ones; every mod with --all), and only
reads: a pull never changes the PC it pulls from.

A pull, on the PC pulling:
  - adds the mods it lacks and updates the ones that differ, file by file (a file the other PC's mod
    doesn't have is removed from the same mod here);
  - takes the other PC's order and on/off for those mods; its own other mods are turned off, never
    deleted;
  - downloads everything first, into mods/.modsync/incoming, so a broken connection changes nothing;
  - keeps every file it replaces or removes, with the mods.json from before, in
    mods/.modsync/backup/<time>/, which `undo` puts back;
  - then applies the mods (as the launcher's Apply does, plugins built included) unless --no-apply.
A mod folder here that is a link to a folder elsewhere is left alone (writing into it would change
the folder it points at).

Output follows tools/os_mods.py, for the launcher: `LOG <text>` lines, then `RESULT {json}`.
"""
import argparse
import hashlib
import hmac
import http.server
import json
import os
import secrets
import shutil
import socket
import socketserver
import stat
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mods  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT = 39440
API = "/modsync/v1"
FORMAT = "hypesmash-modsync"
VERSION = 1
KEY_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"   # (no 0/O, 1/I: it is read out to a friend)
CHUNK = 1 << 20
STATE_KEYS = ("code_patches",)                        # mods.json's own switches, besides order and mods


def log(text):
    print("LOG " + str(text), flush=True)


def result(ok, **extra):
    print("RESULT " + json.dumps({"ok": ok, **extra}), flush=True)
    return 0 if ok else 1


# ---- what a PC has ----------------------------------------------------------------------------
def safe_name(name):
    """A mod folder name from the other PC that is one plain folder in mods/."""
    return (isinstance(name, str) and name not in ("", ".", "..") and not name.startswith(".")
            and not any(c in name for c in '/\\:*?"<>|\0') and name == name.strip())


def safe_path(path):
    """A file path inside a mod's folder: relative, forward slashes, no `..`, no hidden parts."""
    if not isinstance(path, str) or not path or any(c in path for c in "\\:\0"):
        return False
    parts = path.split("/")
    return not path.startswith("/") and all(p and p not in (".", "..") and not p.startswith(".") for p in parts)


def is_link(folder):
    try:
        st = os.lstat(folder)
    except OSError:
        return False
    reparse = getattr(st, "st_file_attributes", 0) & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0)
    return stat.S_ISLNK(st.st_mode) or bool(reparse)


def folder_files(folder):
    """(path, full path) of every file a mod's folder holds, hidden ones left out."""
    out = []
    for dp, dirs, files in os.walk(folder):
        dirs[:] = sorted(d for d in dirs if not d.startswith("."))
        for n in sorted(files):
            if n.startswith("."):
                continue
            full = os.path.join(dp, n)
            out.append((os.path.relpath(full, folder).replace(os.sep, "/"), full))
    return out


class Hashes:
    """sha256 of files, kept between runs by (size, mtime) so a second look at a big mod is quick."""

    def __init__(self, tree):
        self.path = os.path.join(tree.root, "build", "work", "modsync_hashes.json")
        try:
            with open(self.path, encoding="utf-8") as f:
                self.known = json.load(f)
        except (OSError, ValueError):
            self.known = {}
        self.dirty = False

    def of(self, full):
        st = os.stat(full)
        key = os.path.normcase(os.path.abspath(full))
        got = self.known.get(key)
        if got and got[0] == st.st_size and got[1] == st.st_mtime_ns:
            return st.st_size, got[2]
        h = hashlib.sha256()
        with open(full, "rb") as f:
            for block in iter(lambda: f.read(CHUNK), b""):
                h.update(block)
        self.known[key] = [st.st_size, st.st_mtime_ns, h.hexdigest()]
        self.dirty = True
        return st.st_size, h.hexdigest()

    def save(self):
        if not self.dirty:
            return
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        tmp = self.path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(self.known, f)
        os.replace(tmp, self.path)
        self.dirty = False


def mod_entry(folder, hashes):
    files = []
    for path, full in folder_files(folder):
        size, sha = hashes.of(full)
        files.append({"path": path, "size": size, "sha256": sha})
    return files


def manifest(tree, every=False, hashes=None):
    """This PC's mods as the other one sees them: order, on/off, GameBanana details, every file."""
    hashes = hashes or Hashes(tree)
    state = tree.load()
    out = {"format": FORMAT, "version": VERSION, "from": socket.gethostname(), "order": [], "mods": {}}
    for key in STATE_KEYS:
        if key in state:
            out[key] = state[key]
    for name in state["order"]:
        info = state["mods"].get(name, {})
        if not (info.get("enabled") or every):
            continue
        out["order"].append(name)
        out["mods"][name] = {"enabled": bool(info.get("enabled")),
                             "meta": {k: v for k, v in info.items() if k != "enabled"},
                             "files": mod_entry(os.path.join(tree.mods, name), hashes)}
    hashes.save()
    return out


# ---- sharing ----------------------------------------------------------------------------------
def new_key():
    return "".join(secrets.choice(KEY_ALPHABET) for _ in range(8))


def addresses():
    """This PC's IPv4 addresses a friend could reach: LAN and Tailscale (100.64/10) ones first."""
    found = []
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            found.append(info[4][0])
    except OSError:
        pass
    try:   # the address the default route goes out of (no packet is sent)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("10.255.255.255", 1))
            found.append(s.getsockname()[0])
    except OSError:
        pass
    seen = []
    for a in found:
        if a not in seen and not a.startswith("127.") and not a.startswith("169.254."):
            seen.append(a)

    def rank(a):
        first, second = (int(x) for x in a.split(".")[:2])
        tailscale = first == 100 and 64 <= second < 128
        private = first == 10 or (first == 192 and second == 168) or (first == 172 and 16 <= second < 32)
        return (0 if private else 1 if tailscale else 2)
    return sorted(seen, key=rank)


class Share:
    """What `serve` hands out: the manifest, rebuilt when asked for (mods may change while it runs),
    and the files it lists - nothing else."""

    def __init__(self, tree, key, every=False):
        self.tree, self.key, self.every = tree, key, every
        self.hashes = Hashes(tree)
        self.current = None

    def manifest(self):
        self.current = manifest(self.tree, self.every, self.hashes)
        return self.current

    def file(self, mod, path):
        """The full path of a listed file, or None."""
        if self.current is None:
            self.manifest()
        entry = self.current["mods"].get(mod)
        if not entry or not safe_name(mod) or not safe_path(path):
            return None
        if not any(f["path"] == path for f in entry["files"]):
            return None
        full = os.path.join(self.tree.mods, mod, *path.split("/"))
        return full if os.path.isfile(full) else None


def handler_for(share):
    class Handler(http.server.BaseHTTPRequestHandler):
        server_version = "HypeSmashModSync/1"

        def log_message(self, fmt, *args):
            log(f"{self.client_address[0]} {fmt % args}")

        def send_json(self, code, body):
            data = json.dumps(body).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            url = urllib.parse.urlsplit(self.path)
            query = urllib.parse.parse_qs(url.query)
            key = self.headers.get("X-ModSync-Key") or (query.get("key") or [""])[0]
            if not hmac.compare_digest(key.upper().encode(), share.key.encode()):
                return self.send_json(403, {"error": "wrong key"})
            if url.path == API + "/manifest":
                return self.send_json(200, share.manifest())
            if url.path == API + "/file":
                full = share.file((query.get("mod") or [""])[0], (query.get("path") or [""])[0])
                if not full:
                    return self.send_json(404, {"error": "not a shared file"})
                size = os.path.getsize(full)
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(size))
                self.end_headers()
                with open(full, "rb") as f:
                    shutil.copyfileobj(f, self.wfile, CHUNK)
                return None
            return self.send_json(404, {"error": "unknown"})
    return Handler


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def serve(tree, port=PORT, key=None, every=False, bind="0.0.0.0", ready=None):
    share = Share(tree, (key or new_key()).upper(), every)
    listing = share.manifest()
    server = Server((bind, port), handler_for(share))
    port = server.server_address[1]
    total = sum(f["size"] for m in listing["mods"].values() for f in m["files"])
    log(f"sharing {len(listing['order'])} mod(s), {total / 1e6:.1f} MB")
    for a in addresses():
        log(f"address {a}:{port}")
    log(f"key {share.key}")
    if ready:
        ready(server, share)
    try:
        server.serve_forever()
    finally:
        server.server_close()
    return share


# ---- pulling ----------------------------------------------------------------------------------
class Remote:
    def __init__(self, address, key, timeout=30):
        address = address.strip()
        if "://" not in address:
            address = "http://" + address
        url = urllib.parse.urlsplit(address)
        if not url.port:
            address = f"{url.scheme}://{url.hostname}:{PORT}"
        self.base = address.rstrip("/").split("/modsync")[0]
        self.key = key.strip().upper()
        self.timeout = timeout

    def open(self, endpoint, **query):
        url = self.base + API + endpoint + ("?" + urllib.parse.urlencode(query) if query else "")
        req = urllib.request.Request(url, headers={"X-ModSync-Key": self.key})
        try:
            return urllib.request.urlopen(req, timeout=self.timeout)
        except urllib.error.HTTPError as e:
            e.close()
            if e.code == 403:
                raise mods.Error("the other PC refused the key - check it, or start sharing again for a new one")
            raise mods.Error(f"the other PC answered {e.code} for {endpoint}")
        except (urllib.error.URLError, OSError) as e:
            raise mods.Error(f"can't reach {self.base}: {getattr(e, 'reason', e)} - is it sharing, and on "
                             "the same network or Tailscale?")

    def manifest(self):
        with self.open("/manifest") as r:
            body = json.load(r)
        if body.get("format") != FORMAT or body.get("version") != VERSION:
            raise mods.Error("the other PC runs a different mod sync version - update both")
        for name in body.get("order", []):
            if not safe_name(name) or name not in body["mods"]:
                raise mods.Error(f"the other PC listed a mod this one can't take: {name!r}")
            for f in body["mods"][name]["files"]:
                if not safe_path(f.get("path")) or not isinstance(f.get("size"), int) or \
                        not isinstance(f.get("sha256"), str) or len(f["sha256"]) != 64:
                    raise mods.Error(f"the other PC listed a file this one can't take: {name}/{f.get('path')!r}")
        return body

    def fetch(self, mod, path, size, sha, dest):
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        h = hashlib.sha256()
        got = 0
        with self.open("/file", mod=mod, path=path) as r, open(dest, "wb") as out:
            for block in iter(lambda: r.read(CHUNK), b""):
                got += len(block)
                if got > size:
                    break
                h.update(block)
                out.write(block)
        if got != size or h.hexdigest() != sha:
            os.remove(dest)
            raise mods.Error(f"{mod}/{path} arrived damaged (it may have changed on the other PC while "
                             "sending) - pull again")
        return got


def plan(tree, remote, hashes=None):
    """What a pull would do, as plain data (shown to the person before anything changes)."""
    hashes = hashes or Hashes(tree)
    state = tree.load()
    out = {"add": [], "update": {}, "same": [], "skipped": [], "turn_off": [], "bytes": 0, "files": 0}
    for name in remote["order"]:
        theirs = {f["path"]: f for f in remote["mods"][name]["files"]}
        folder = os.path.join(tree.mods, name)
        if not os.path.isdir(folder):
            out["add"].append(name)
            out["bytes"] += sum(f["size"] for f in theirs.values())
            out["files"] += len(theirs)
            continue
        if is_link(folder):
            out["skipped"].append(name)
            continue
        mine = {p: full for p, full in folder_files(folder)}
        get = []
        for path, f in theirs.items():
            have = mine.get(path)
            if have is None or hashes.of(have) != (f["size"], f["sha256"]):
                get.append(path)
        drop = sorted(p for p in mine if p not in theirs)
        if get or drop:
            out["update"][name] = {"get": get, "drop": drop}
            out["bytes"] += sum(theirs[p]["size"] for p in get)
            out["files"] += len(get)
        else:
            out["same"].append(name)
    out["turn_off"] = [n for n in state["order"] if n not in remote["mods"] and state["mods"][n].get("enabled")]
    hashes.save()
    out["state_changes"] = state_after(state, remote) != state
    return out


def state_after(state, remote):
    """mods.json after a pull: the other PC's order and on/off for its mods, this PC's others off
    and after them."""
    new = json.loads(json.dumps(state))
    theirs = [n for n in remote["order"]]
    rest = [n for n in state["order"] if n not in remote["mods"]]
    new["order"] = theirs + rest
    for n in theirs:
        info = dict(remote["mods"][n].get("meta") or {})
        info["enabled"] = bool(remote["mods"][n]["enabled"])
        new["mods"][n] = info
    for n in rest:
        new["mods"][n] = dict(new["mods"].get(n, {}), enabled=False)
    for key in STATE_KEYS:
        if key in remote:
            new[key] = remote[key]
    return new


def sync_root(tree):
    return os.path.join(tree.mods, ".modsync")


def pull(tree, address, key, dry_run=False, apply=True):
    remote_side = Remote(address, key)
    log(f"asking {remote_side.base} for its mods")
    remote = remote_side.manifest()
    log(f"{remote.get('from', 'the other PC')} shares {len(remote['order'])} mod(s)")
    p = plan(tree, remote)
    describe(p)
    if dry_run:
        return p

    # 1. everything into incoming/, checked; nothing here changes until it all arrived
    incoming = os.path.join(sync_root(tree), "incoming")
    shutil.rmtree(incoming, ignore_errors=True)
    want = [(n, f["path"]) for n in p["add"] for f in remote["mods"][n]["files"]]
    want += [(n, path) for n, u in p["update"].items() for path in u["get"]]
    sizes = {(n, f["path"]): f for n in remote["order"] for f in remote["mods"][n]["files"]}
    done = 0
    last = -1
    for n, path in want:
        f = sizes[(n, path)]
        done += remote_side.fetch(n, path, f["size"], f["sha256"], os.path.join(incoming, n, *path.split("/")))
        pct = int(done * 100 / p["bytes"]) if p["bytes"] else 100
        if pct != last:
            log(f"percent {pct}")
            last = pct

    # 2. swap in, keeping what is replaced
    state = tree.load()
    stamp = time.strftime("%Y%m%d-%H%M%S")
    backup = os.path.join(sync_root(tree), "backup", stamp)
    os.makedirs(backup, exist_ok=True)
    record = {"from": remote.get("from"), "address": remote_side.base, "state": state,
              "added_mods": [], "added_files": [], "kept": []}
    for n in p["add"]:
        shutil.move(os.path.join(incoming, n), os.path.join(tree.mods, n))
        record["added_mods"].append(n)
    for n, u in p["update"].items():
        folder = os.path.join(tree.mods, n)
        for path in u["get"] + u["drop"]:
            here = os.path.join(folder, *path.split("/"))
            if os.path.exists(here):
                keep = os.path.join(backup, "files", n, *path.split("/"))
                os.makedirs(os.path.dirname(keep), exist_ok=True)
                shutil.move(here, keep)
                record["kept"].append([n, path])
            elif path in u["get"]:
                record["added_files"].append([n, path])
        for path in u["get"]:
            dest = os.path.join(folder, *path.split("/"))
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            shutil.move(os.path.join(incoming, n, *path.split("/")), dest)
        prune_empty(folder)
    tree.save(state_after(tree.load(), remote))
    with open(os.path.join(backup, "undo.json"), "w", encoding="utf-8") as f:
        json.dump(record, f, indent=1)
    shutil.rmtree(incoming, ignore_errors=True)
    log(f"matched {remote.get('from', 'the other PC')}'s mods (undo keeps {stamp})")
    if apply:
        mods.apply(tree, log)
    return p


def prune_empty(folder):
    for dp, dirs, files in os.walk(folder, topdown=False):
        if dp != folder and not os.listdir(dp):
            os.rmdir(dp)


def describe(p):
    if p["add"]:
        log(f"new: {', '.join(p['add'])}")
    for n, u in p["update"].items():
        log(f"update {n}: {len(u['get'])} file(s) to get, {len(u['drop'])} to remove")
    if p["turn_off"]:
        log(f"turned off (kept): {', '.join(p['turn_off'])}")
    if p["skipped"]:
        log(f"left alone (linked folders): {', '.join(p['skipped'])}")
    if not (p["add"] or p["update"] or p["turn_off"] or p["state_changes"]):
        log("already the same")
    log(f"to download: {p['files']} file(s), {p['bytes'] / 1e6:.1f} MB")


def undo(tree, apply=True):
    """Put back what the last pull changed."""
    root = os.path.join(sync_root(tree), "backup")
    stamps = sorted(d for d in os.listdir(root) if os.path.isfile(os.path.join(root, d, "undo.json"))) \
        if os.path.isdir(root) else []
    if not stamps:
        raise mods.Error("no mod sync to undo")
    backup = os.path.join(root, stamps[-1])
    with open(os.path.join(backup, "undo.json"), encoding="utf-8") as f:
        record = json.load(f)
    for n in record["added_mods"]:
        folder = os.path.join(tree.mods, n)
        if os.path.isdir(folder) and not is_link(folder):
            shutil.rmtree(folder)
    for n, path in record["added_files"]:
        here = os.path.join(tree.mods, n, *path.split("/"))
        if os.path.isfile(here):
            os.remove(here)
    for n, path in record["kept"]:
        here = os.path.join(tree.mods, n, *path.split("/"))
        os.makedirs(os.path.dirname(here), exist_ok=True)
        if os.path.exists(here):
            os.remove(here)
        shutil.move(os.path.join(backup, "files", n, *path.split("/")), here)
    for n in {n for n, _ in record["added_files"]}:
        if os.path.isdir(os.path.join(tree.mods, n)):
            prune_empty(os.path.join(tree.mods, n))
    tree.save(record["state"])
    shutil.rmtree(backup)
    log(f"undid the mod sync from {record.get('from') or record.get('address')}")
    if apply:
        mods.apply(tree, log)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("serve", help="share this PC's mods")
    s.add_argument("--port", type=int, default=PORT)
    s.add_argument("--key")
    s.add_argument("--all", action="store_true", help="share turned-off mods too")
    s.add_argument("--bind", default="0.0.0.0")
    pl = sub.add_parser("pull", help="match another PC's mods")
    pl.add_argument("address")
    pl.add_argument("key")
    pl.add_argument("--dry-run", action="store_true")
    pl.add_argument("--no-apply", action="store_true")
    u = sub.add_parser("undo", help="put back what the last pull changed")
    u.add_argument("--no-apply", action="store_true")
    a = ap.parse_args(argv)
    tree = mods.Tree(ROOT)
    try:
        if a.cmd == "serve":
            serve(tree, a.port, a.key, a.all, a.bind)
            return result(True)
        if a.cmd == "pull":
            p = pull(tree, a.address, a.key, a.dry_run, not a.no_apply)
            return result(True, plan=p, dry_run=a.dry_run)
        undo(tree, not a.no_apply)
        return result(True)
    except mods.Error as e:
        return result(False, error=str(e))
    except KeyboardInterrupt:
        return result(True, stopped=True)


if __name__ == "__main__":
    sys.exit(main())
