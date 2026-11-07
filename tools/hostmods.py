"""Host-only mods: a host's applied mods, sealed and sent through the lobby to the room's other
players, whose games read them from memory and never write them to disk (docs/HOST_MODS.md).

    python tools/hostmods.py code
        a new invite code (give it to the players you play with, not in the room's name)
    python tools/hostmods.py serve --lobby HOST[:PORT] --code CODE [--tree .]
        seal this tree's applied mods (tools/mods.py apply) and keep them up on the lobby for as
        long as this runs; players start their game with SSBU_HOST_MODS=CODE

This is a deterrent, not protection: a player's game holds the key and the plain mods in its
memory while it runs, and anyone who can read that memory can take them. The pack has the data mods (what
tools/mods.py apply puts in the layer map) and the turned-on mods' Skyline plugins as this host's game
translated them (tools/skyline.py: build/plugins/<module>.dll, Windows), which players load from memory.

The formats (runtime/hle_hostmods.c reads them):
  invite code   20 base32 letters, shown in groups of five; case, spaces and '-' don't count
  pack id       hex of sha256("HypeSmash host mods id\\0" + code)[:10] - all the lobby is told
  sealed pack   "SSBUHME1", salt (16), u64 plain size, u32 chunk size, u32 0; then the plain pack
                in chunks of chunk size (the last one shorter), each ChaCha20-Poly1305 (RFC 8439)
                with key sha256("HypeSmash host mods v1\\0" + salt + code), nonce 4 zero bytes
                and the chunk's index (u64), additional data the 40-byte head and a byte that
                is 1 for the last chunk, 0 before; each chunk's 16-byte tag after it
  plain pack    "SSBUHMP1", u32 version (2; 1 has no plugins); u32 mod count, then each mod's u32 hash (the runtime's
                mod_tree_hash folded to 32 bits, net_relay.c) and u16 length and UTF-8 name;
                the sha256 of the game's own layer map (romfs_vanilla.cfg's); u32 source
                count and each source's u64 size; u32 length and the modded layer map; u64
                length and the overlay (the layer map's last source); then (version 2) u32 length
                and the host's plugins.txt; u32 plugin count, then each plugin's u16 length and
                module name, u8 system (PLUGIN_OS), u64 length and library, u64 length and image
                (dump/modules/<module>.reloc.bin)
All numbers little-endian but the lobby's (tools/hostmods_depot.py)."""
import argparse
import base64
import hashlib
import json
import os
import secrets
import socket
import struct
import sys

SEALED_MAGIC = b"SSBUHME1"
PLAIN_MAGIC = b"SSBUHMP1"
LAYER_MAGIC = b"SSBUROM1"
VERSION = 2
PLUGIN_OS = {".dll": 1, ".so": 2, ".dylib": 3}   # (runtime/hle_hostmods.c HM_OS: Windows x64, Linux x64, macOS arm64)
CHUNK = 1 << 20
HEAD = 40
TAG = 16
DEFAULT_PORT = 39420  # (runtime/net.h NET_DEFAULT_PORT; the lobby's --port)
FNV0 = 0xCBF29CE484222325
MASK64 = (1 << 64) - 1
NATIVE = (".dll", ".so", ".dylib")
MODDED_MAP = "romfs_mods.layers"


class Error(Exception):
    pass


# ---- the invite code -----------------------------------------------------------------------
def new_code():
    raw = base64.b32encode(secrets.token_bytes(15)).decode()[:20]
    return "-".join(raw[i:i + 5] for i in range(0, 20, 5))


def normalize(code):
    c = "".join(ch for ch in code.upper() if ch not in "- \t")
    if not c or not c.isascii():
        raise Error("not an invite code")
    return c.encode()


def pack_id(code):
    return hashlib.sha256(b"HypeSmash host mods id\0" + normalize(code)).digest()[:10].hex()


def key_of(code, salt):
    return hashlib.sha256(b"HypeSmash host mods v1\0" + salt + normalize(code)).digest()


# ---- sealing -------------------------------------------------------------------------------
def _cipher(key, index):
    from Cryptodome.Cipher import ChaCha20_Poly1305
    return ChaCha20_Poly1305.new(key=key, nonce=b"\0" * 4 + struct.pack("<Q", index))


def seal(code, plain, chunk=CHUNK, salt=None):
    salt = salt if salt is not None else secrets.token_bytes(16)
    head = SEALED_MAGIC + salt + struct.pack("<QII", len(plain), chunk, 0)
    key = key_of(code, salt)
    n = max(1, -(-len(plain) // chunk))
    out = [head]
    view = memoryview(plain)
    for i in range(n):
        c = _cipher(key, i)
        c.update(head + bytes([i == n - 1]))
        ct, tag = c.encrypt_and_digest(view[i * chunk:(i + 1) * chunk])
        out += [ct, tag]
    return b"".join(out)


def unseal(code, sealed):
    if len(sealed) < HEAD or sealed[:8] != SEALED_MAGIC:
        raise Error("not a sealed pack")
    head = sealed[:HEAD]
    size, chunk, _ = struct.unpack_from("<QII", head, 24)
    if not chunk:
        raise Error("not a sealed pack")
    n = max(1, -(-size // chunk))
    if len(sealed) != HEAD + size + n * TAG:
        raise Error("a sealed pack of the wrong size")
    key = key_of(code, head[8:24])
    out = bytearray()
    at = HEAD
    for i in range(n):
        m = min(chunk, size - i * chunk)
        c = _cipher(key, i)
        c.update(head + bytes([i == n - 1]))
        try:
            out += c.decrypt_and_verify(sealed[at:at + m], sealed[at + m:at + m + TAG])
        except ValueError:
            raise Error("the pack doesn't open with that code (or was changed)") from None
        at += m + TAG
    return bytes(out)


# ---- the plain pack ------------------------------------------------------------------------
def build_plain(mods, vanilla_map_sha, source_sizes, modded_map, overlay, plugin_list=b"", plugins=()):
    """plugins: [(module name, system (PLUGIN_OS), library bytes, image bytes)]."""
    out = [PLAIN_MAGIC, struct.pack("<II", VERSION, len(mods))]
    for name, h in mods:
        b = name.encode("utf-8")
        out.append(struct.pack("<IH", h & 0xFFFFFFFF, len(b)) + b)
    out.append(vanilla_map_sha)
    out.append(struct.pack("<I", len(source_sizes)) + b"".join(struct.pack("<Q", s) for s in source_sizes))
    out.append(struct.pack("<I", len(modded_map)) + modded_map)
    out.append(struct.pack("<Q", len(overlay)))
    out.append(overlay)
    out.append(struct.pack("<I", len(plugin_list)) + plugin_list)
    out.append(struct.pack("<I", len(plugins)))
    for name, system, lib, image in plugins:
        b = name.encode("utf-8")
        out.append(struct.pack("<H", len(b)) + b + bytes([system]))
        out.append(struct.pack("<Q", len(lib)) + lib)
        out.append(struct.pack("<Q", len(image)) + image)
    return b"".join(out)


def parse_plain(p):
    if p[:8] != PLAIN_MAGIC:
        raise Error("not a mod pack")
    version, n = struct.unpack_from("<II", p, 8)
    if version not in (1, VERSION):
        raise Error(f"a mod pack of version {version}")
    at, mods = 16, []
    for _ in range(n):
        h, ln = struct.unpack_from("<IH", p, at)
        mods.append((p[at + 6:at + 6 + ln].decode("utf-8"), h))
        at += 6 + ln
    sha = p[at:at + 32]
    at += 32
    ns = struct.unpack_from("<I", p, at)[0]
    sizes = list(struct.unpack_from(f"<{ns}Q", p, at + 4))
    at += 4 + 8 * ns
    ml = struct.unpack_from("<I", p, at)[0]
    modded_map = p[at + 4:at + 4 + ml]
    at += 4 + ml
    ol = struct.unpack_from("<Q", p, at)[0]
    overlay = p[at + 8:at + 8 + ol]
    at += 8 + ol
    plugin_list, plugins = b"", []
    if version >= 2:
        ll = struct.unpack_from("<I", p, at)[0]
        plugin_list = p[at + 4:at + 4 + ll]
        at += 4 + ll
        count = struct.unpack_from("<I", p, at)[0]
        at += 4
        for _ in range(count):
            nl = struct.unpack_from("<H", p, at)[0]
            name = p[at + 2:at + 2 + nl].decode("utf-8")
            system = p[at + 2 + nl]
            at += 3 + nl
            lib_len = struct.unpack_from("<Q", p, at)[0]
            lib = p[at + 8:at + 8 + lib_len]
            at += 8 + lib_len
            il = struct.unpack_from("<Q", p, at)[0]
            image = p[at + 8:at + 8 + il]
            at += 8 + il
            plugins.append((name, system, lib, image))
    if at != len(p):
        raise Error("a mod pack of the wrong size")
    return {"mods": mods, "vanilla_map_sha256": sha, "source_sizes": sizes, "map": modded_map, "overlay": overlay,
            "plugin_list": plugin_list, "plugins": plugins}


def check_map(modded_map, nsources):
    """The modded layer map reads the game's sources and the overlay (the last) only."""
    if modded_map[:8] != LAYER_MAGIC:
        raise Error("not a layer map")
    size, count, _ = struct.unpack_from("<QII", modded_map, 8)
    if len(modded_map) != 24 + 24 * count or not count:
        raise Error("a layer map of the wrong size")
    for at, off, src, _ in struct.iter_unpack("<QQII", modded_map[24:]):
        if src != 0xFFFFFFFF and src > nsources:
            raise Error(f"the layer map reads source {src} of {nsources + 1}")


# ---- the mods' hashes, as the runtime takes them (net_relay.c mod_tree_hash) ----------------
def fnv64(h, data):
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & MASK64
    return h


def hashed(name):
    low = name.lower()
    if name.startswith(".") or low == "info.toml" or low.startswith("preview.") or low.startswith("readme."):
        return False
    return not any(len(low) > len(e) and low.endswith(e) for e in NATIVE)


def tree_hash(folder, rel="", depth=0):
    total = 0
    try:
        entries = list(os.scandir(folder))
    except OSError:
        return 0
    for e in entries:
        r = f"{rel}/{e.name}" if rel else e.name
        try:
            isdir = e.is_dir()
            size = 0 if isdir else e.stat().st_size
        except OSError:
            continue
        if isdir:
            if depth < 16:
                total += tree_hash(e.path, r, depth + 1)
        elif hashed(e.name):
            total += fnv64(fnv64(FNV0, r.encode("utf-8").lower()), struct.pack("<Q", size))
    return total & MASK64


def mod_hash32(folder):
    h = tree_hash(folder)
    return (h ^ h >> 32) & 0xFFFFFFFF


# ---- this tree's plugins ---------------------------------------------------------------------
def tree_plugins(root):
    """The host's plugins.txt and the turned-on mods' translated plugins (dump/skyline/plugin_dlls.txt,
    written by tools/skyline.py): (list bytes, [(module, system, library, image)])."""
    skyline = os.path.join(root, "dump", "skyline")
    try:
        with open(os.path.join(skyline, "plugins.txt"), "rb") as f:
            plugin_list = f.read()
    except OSError:
        return b"", []
    listed = {l.split(b"#")[0].strip().decode("utf-8", "replace") for l in plugin_list.splitlines()} - {""}
    try:
        rows = _cfg_lines(os.path.join(skyline, "plugin_dlls.txt"))
    except OSError:
        rows = []
    plugins = []
    for row in rows:
        if row.startswith("#"):
            continue
        path = row if os.path.isabs(row) else os.path.join(root, row)
        name, ext = os.path.splitext(os.path.basename(path))
        if name not in listed:
            continue
        if ext.lower() not in PLUGIN_OS:
            raise Error(f"{path}: not a plugin library")
        image = os.path.join(root, "dump", "modules", name + ".reloc.bin")
        try:
            with open(path, "rb") as f:
                lib = f.read()
            with open(image, "rb") as f:
                img = f.read()
        except OSError as e:
            raise Error(f"plugin {name}: {e} (build the game with the mods turned on first)") from None
        plugins.append((name, PLUGIN_OS[ext.lower()], lib, img))
    return plugin_list, plugins


# ---- this tree's applied mods, as a pack -----------------------------------------------------
def _cfg_lines(path):
    with open(path, encoding="utf-8-sig") as f:
        return [l.strip() for l in f.read().splitlines() if l.strip()]


def pack_tree(root, with_plugins=True):
    root = os.path.abspath(root)
    modules = os.path.join(root, "dump", "modules")
    cfg = _cfg_lines(os.path.join(modules, "romfs.cfg"))
    try:
        with open(os.path.join(root, "dump", "mods", "applied.json"), encoding="utf-8") as f:
            applied = json.load(f)
    except (OSError, ValueError):
        raise Error("no mods applied here (dump/mods/applied.json): tools/mods.py apply first") from None
    if applied.get("arcropolis"):
        raise Error("mods applied through ARCropolis can't be sent: apply them with tools/mods.py apply")
    if len(cfg) < 4 or cfg[0] != "layered" or os.path.basename(cfg[1]) != MODDED_MAP or not applied.get("mods"):
        raise Error("the game here doesn't run with mods applied (dump/modules/romfs.cfg)")
    vanilla = _cfg_lines(os.path.join(modules, "romfs_vanilla.cfg"))
    if vanilla[0] != "layered":
        raise Error("romfs_vanilla.cfg isn't a layered one")
    sources, overlay_path = cfg[2:-1], cfg[-1]
    if [os.path.normcase(os.path.abspath(s)) for s in sources] != [os.path.normcase(os.path.abspath(s)) for s in vanilla[2:]]:
        raise Error("romfs.cfg's sources aren't romfs_vanilla.cfg's: apply the mods again")
    with open(vanilla[1], "rb") as f:
        vanilla_sha = hashlib.sha256(f.read()).digest()
    with open(cfg[1], "rb") as f:
        modded_map = f.read()
    check_map(modded_map, len(sources))
    with open(overlay_path, "rb") as f:
        overlay = f.read()
    mods = [(name, mod_hash32(os.path.join(root, "mods", name))) for name in applied["mods"]]
    plugin_list, plugins = tree_plugins(root) if with_plugins else (b"", [])
    plain = build_plain(mods, vanilla_sha, [os.path.getsize(s) for s in sources], modded_map, overlay, plugin_list, plugins)
    return plain, mods, [name for name, *_ in plugins]


# ---- the lobby -----------------------------------------------------------------------------
def _address(lobby):
    host, _, port = lobby.rpartition(":") if ":" in lobby else (lobby, "", "")
    return host, int(port) if port else DEFAULT_PORT


def put(lobby, pid, sealed, timeout=30):
    """Up on the lobby: the open socket (the pack is up while it is open)."""
    s = socket.create_connection(_address(lobby), timeout=timeout)
    s.sendall(b"SSBUPAK1PUT " + struct.pack(">I", len(pid)) + pid.encode() + struct.pack(">Q", len(sealed)))
    answer = _exact(s, 4)
    if answer == b"GO  ":
        s.sendall(sealed)
        answer = _exact(s, 4)
    if answer != b"OK  ":
        s.close()
        raise Error({b"TAKN": "that code's pack is up already (another host with it?)",
                     b"BIG ": "the lobby won't take a pack that big", b"BAD ": "the lobby didn't take it as a pack"}.get(answer, f"the lobby said {answer!r}"))
    s.settimeout(None)
    return s


def get(lobby, pid, timeout=30):
    with socket.create_connection(_address(lobby), timeout=timeout) as s:
        s.sendall(b"SSBUPAK1GET " + struct.pack(">I", len(pid)) + pid.encode())
        n = struct.unpack(">Q", _exact(s, 8))[0]
        return _exact(s, n) if n else None


def _exact(s, n):
    buf = bytearray()
    while len(buf) < n:
        got = s.recv(min(1 << 20, n - len(buf)))
        if not got:
            raise Error("the lobby closed the connection")
        buf += got
    return bytes(buf)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("code", help="a new invite code")
    sv = sub.add_parser("serve", help="keep this tree's applied mods up on the lobby")
    sv.add_argument("--lobby", required=True, help="host[:port] (the players' SSBU_NET_RELAY)")
    sv.add_argument("--code", help="the invite code (default: a new one, printed)")
    sv.add_argument("--tree", default=".", help="the port's folder (default: here)")
    sv.add_argument("--no-plugins", action="store_true", help="the data mods only (players use their own plugins)")
    o = ap.parse_args(argv)
    try:
        if o.cmd == "code":
            print(new_code())
            return 0
        code = o.code or new_code()
        plain, mods, plugins = pack_tree(o.tree, not o.no_plugins)
        sealed = seal(code, plain)
        del plain
        pid = pack_id(code)
        s = put(o.lobby, pid, sealed)
        print(f"up: {len(mods)} mod(s), {len(sealed) / 1048576:.1f} MB sealed, pack {pid}")
        for name, h in mods:
            print(f"  {name}  {h:08x}")
        if plugins:
            print(f"plugins (Windows players): {', '.join(plugins)}")
        print(f"invite code: {code}\nplayers: set SSBU_HOST_MODS={code} and SSBU_NET_RELAY={o.lobby}, then start the game")
        print("(the pack is up while this runs: Ctrl+C takes it down)")
        try:
            while s.recv(4096):
                pass
            print("the lobby closed the connection: the pack is down")
            return 1
        except KeyboardInterrupt:
            return 0
        finally:
            s.close()
    except (Error, OSError) as e:
        print(f"hostmods: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
