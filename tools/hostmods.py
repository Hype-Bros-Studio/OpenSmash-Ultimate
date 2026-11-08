"""Host-only mods: a host's applied mods, sealed and sent through the lobby to the room's other
players, whose games read them from memory and never write them to disk (docs/HOST_MODS.md).
With --open the host lets them keep the mods: the pack goes up unsealed, with each turned-on mod's
folder, which a player who ticked Download host's mods saves into their own mods/.

    python tools/hostmods.py code
        a new invite code (give it to the players you play with, not in the room's name)
    python tools/hostmods.py serve --lobby HOST[:PORT] --code CODE [--tree .]
        seal this tree's applied mods (tools/mods.py apply) and keep them up on the lobby for as
        long as this runs; players start their game with SSBU_HOST_MODS=CODE
    python tools/hostmods.py serve --open ...
        the same, unsealed, with the mods' folders: players who tick F1 > Online > Download host's
        mods (or set SSBU_HOST_MODS_DOWNLOAD=1) get them saved into their mods/, turned off

This is a deterrent, not protection: a player's game holds the key and the plain mods in its
memory while it runs, and anyone who can read that memory can take them. An open pack (--open) is no
deterrent at all: anyone with the code, and the lobby, can read it. The pack has the data mods (what
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
  open pack     the plain pack as it is, unsealed (serve --open), under the same pack id
  plain pack    "SSBUHMP1", u32 version (3 open, 2 sealed; 1 has no plugins); u32 mod count, then each mod's u32 hash (the runtime's
                mod_tree_hash folded to 32 bits, net_relay.c) and u16 length and UTF-8 name;
                the sha256 of the game's own layer map (romfs_vanilla.cfg's); u32 source
                count and each source's u64 size; u32 length and the modded layer map; u64
                length and the overlay (the layer map's last source); then (version 2) u32 length
                and the host's plugins.txt; u32 plugin count, then each plugin's u16 length and
                module name, u8 system (PLUGIN_OS), u64 length and library, u64 length and image
                (dump/modules/<module>.reloc.bin); then (version 3) u32 folder count, then each
                turned-on mod's folder: u16 length and UTF-8 name (mods/<name>), u32 file count,
                then each file's u16 length and UTF-8 path in the folder ('/' between folders),
                u64 length and its bytes. A name or path that could leave mods/<name> (.., '\\', ':',
                an absolute path, ...: safe_name, safe_path) makes the pack refused.
All numbers little-endian but the lobby's (tools/hostmods_depot.py)."""
import argparse
import base64
import bisect
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
VERSION = 3            # (an open pack's; a sealed one stays SEALED_VERSION, without the mods' folders)
SEALED_VERSION = 2
MODS_INDEX_MAGIC = b"SSBUHMI1"   # (the pack's last section, when there: the mods' files as mods:/ sees them)
MODS_INDEX_DATA_MAX = 4 << 10    # (the bytes of files this small are in it: plugins' marker files)
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
def build_plain(mods, vanilla_map_sha, source_sizes, modded_map, overlay, plugin_list=b"", plugins=(), folders=None,
                mods_index=None):
    """plugins: [(module name, system (PLUGIN_OS), library bytes, image bytes)]. folders (an open pack,
    version 3): [(mod folder name, [(path in the folder, bytes)])]; None for a sealed pack (version 2).
    (Names aren't checked here: tests build bad packs with it. parse_plain checks them.)"""
    version = SEALED_VERSION if folders is None else VERSION
    out = [PLAIN_MAGIC, struct.pack("<II", version, len(mods))]
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
    if folders is not None:
        out.append(struct.pack("<I", len(folders)))
        for name, files in folders:
            b = name.encode("utf-8")
            out.append(struct.pack("<H", len(b)) + b + struct.pack("<I", len(files)))
            for path, data in files:
                pb = path.encode("utf-8")
                out.append(struct.pack("<H", len(pb)) + pb + struct.pack("<Q", len(data)))
                out.append(data)
    if mods_index is not None:
        out.append(MODS_INDEX_MAGIC + struct.pack("<I", len(mods_index)))
        for path, size, data in mods_index:
            pb = path.encode("utf-8")
            out.append(struct.pack("<HQI", len(pb), size, len(data)) + pb + data)
    return b"".join(out)


# ---- an open pack's folders: names and paths that stay in mods/<name> (runtime: hle_hostmods.c safe_part) ----
RESERVED = {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"} | {f"{d}{i}" for d in ("COM", "LPT") for i in range(10)}


def _safe_part(part):
    """One folder or file name: not '.' or '..', no '/', '\\', ':' or control character, not ending in a
    dot or a space, and not a Windows device name (CON, NUL, COM1, ...)."""
    if not part or part in (".", "..") or len(part.encode("utf-8")) > 255:
        return False
    if any(c in "/\\:" or ord(c) < 32 or ord(c) == 127 for c in part) or part[-1] in ". ":
        return False
    return part.split(".")[0].upper().rstrip(" ") not in RESERVED


def safe_name(name):
    """A mod folder's name: one safe part, not hidden (a leading dot: mods/.packs and the like are tools/mods.py's)."""
    return _safe_part(name) and not name.startswith(".")


def safe_path(path):
    """A file's path in a mod's folder: '/' between safe parts, relative (so no drive letter, no leading '/')."""
    return 0 < len(path.encode("utf-8")) <= 1024 and all(_safe_part(p) for p in path.split("/"))


def parse_plain(p):
    try:
        return _parse_plain(p)
    except (struct.error, UnicodeDecodeError):
        raise Error("a mod pack cut short or broken") from None


def _parse_plain(p):
    if p[:8] != PLAIN_MAGIC:
        raise Error("not a mod pack")
    version, n = struct.unpack_from("<II", p, 8)
    if version not in (1, 2, VERSION):
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
    folders = None
    if version >= 3:
        folders = []
        count = struct.unpack_from("<I", p, at)[0]
        at += 4
        for _ in range(count):
            nl = struct.unpack_from("<H", p, at)[0]
            name = p[at + 2:at + 2 + nl].decode("utf-8")
            nf = struct.unpack_from("<I", p, at + 2 + nl)[0]
            at += 6 + nl
            if not safe_name(name):
                raise Error(f"a mod folder named {name!r} in the pack")
            files = []
            for _ in range(nf):
                pl = struct.unpack_from("<H", p, at)[0]
                path = p[at + 2:at + 2 + pl].decode("utf-8")
                dl = struct.unpack_from("<Q", p, at + 2 + pl)[0]
                at += 10 + pl
                if not safe_path(path):
                    raise Error(f"a file at {path!r} in the pack's {name}")
                if at + dl > len(p):
                    raise Error("a mod pack of the wrong size")
                files.append((path, p[at:at + dl]))
                at += dl
            folders.append((name, files))
    mods_index = None
    if p[at:at + 8] == MODS_INDEX_MAGIC:
        mods_index = []
        count = struct.unpack_from("<I", p, at + 8)[0]
        at += 12
        for _ in range(count):
            pl, size, dl = struct.unpack_from("<HQI", p, at)
            path = p[at + 14:at + 14 + pl].decode("utf-8")
            at += 14 + pl
            if not safe_path(path) or dl > size or at + dl > len(p):
                raise Error(f"a mods: file at {path!r} in the pack")
            mods_index.append((path, size, p[at:at + dl]))
            at += dl
    if at != len(p):
        raise Error("a mod pack of the wrong size")
    return {"mods": mods, "vanilla_map_sha256": sha, "source_sizes": sizes, "map": modded_map, "overlay": overlay,
            "plugin_list": plugin_list, "plugins": plugins, "folders": folders, "mods_index": mods_index}


def compact_overlay(modded_map, overlay, src):
    """mods.py writes the overlay sparse (its pieces at scattered offsets, holes between them):
    send only the bytes the map reads from it, packed, with the map's offsets moved to match."""
    size, count, _ = struct.unpack_from("<QII", modded_map, 8)
    ents = list(struct.iter_unpack("<QQII", modded_map[24:]))
    spans = []
    for i, (at, off, s, pad) in enumerate(ents):
        if s == src:
            end = ents[i + 1][0] if i + 1 < len(ents) else size
            spans.append((off, off + end - at))
    merged = []
    for lo, hi in sorted(spans):
        if merged and lo <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], hi)
        else:
            merged.append([lo, hi])
    if any(hi > len(overlay) for _, hi in merged):
        raise Error("the layer map reads past the end of the overlay: apply the mods again")
    moved, packed, out = {}, bytearray(), []
    for lo, hi in merged:
        moved[lo] = len(packed)
        packed += overlay[lo:hi]
    starts = [lo for lo, _ in merged]
    for at, off, s, pad in ents:
        if s == src:
            lo = starts[bisect.bisect_right(starts, off) - 1]
            off = moved[lo] + off - lo
        out.append(struct.pack("<QQII", at, off, s, pad))
    return modded_map[:24] + b"".join(out), bytes(packed)


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


def mod_folder(folder):
    """Every file of a mod's folder, with its path in it ('/' between folders), sorted. Links are left out."""
    files = []
    for top, dirs, names in os.walk(folder):
        dirs[:] = sorted(d for d in dirs if not os.path.islink(os.path.join(top, d)))
        for n in sorted(names):
            full = os.path.join(top, n)
            if os.path.islink(full) or not os.path.isfile(full):
                continue
            rel = os.path.relpath(full, folder).replace(os.sep, "/")
            if not safe_path(rel):
                raise Error(f"{full}: a name players' games won't save (rename it)")
            with open(full, "rb") as f:
                files.append((rel, f.read()))
    return files


def mods_index(root, names):
    """The turned-on mods' files as ARCropolis' mods:/ shows them (one tree, a later mod's file over an
    earlier one's): [(path, size, bytes - only for a file of MODS_INDEX_DATA_MAX or less)]. Players' games
    serve mods:/ from it under host mods (plugins look for their marker files there)."""
    files = {}
    for name in names:
        folder = os.path.join(root, "mods", name)
        for top, dirs, fnames in os.walk(folder):
            dirs[:] = sorted(d for d in dirs if not os.path.islink(os.path.join(top, d)))
            for n in sorted(fnames):
                full = os.path.join(top, n)
                if os.path.islink(full) or not os.path.isfile(full):
                    continue
                rel = os.path.relpath(full, folder).replace(os.sep, "/")
                if not safe_path(rel):
                    continue
                files[rel.lower()] = (rel, full)
    out = []
    for _, (rel, full) in sorted(files.items()):
        size = os.path.getsize(full)
        data = b""
        if size <= MODS_INDEX_DATA_MAX:
            with open(full, "rb") as f:
                data = f.read()
            size = len(data)
        out.append((rel, size, data))
    return out


def check_csk(root, applied):
    """The select-screen entries the plugins add (dump/skyline/csk.json, written as this tree's game
    starts) go into the game's databases when the mods are applied: players get the databases as
    applied, so entries recorded since then would be missing from their select screens."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import csk
    if not os.path.exists(os.path.join(root, "dump", "skyline", "csk.json")):
        print("note: the plugins haven't recorded their select-screen entries here yet (dump/skyline/csk.json):"
              " if they add characters, start this game once, then tools/mods.py apply again", file=sys.stderr)
        return
    if applied.get("csk") != csk.digest(csk.load(root)):
        raise Error("the plugins' select-screen entries changed since the mods were applied"
                    " (dump/skyline/csk.json): tools/mods.py apply again")


def pack_tree(root, with_plugins=True, open_pack=False):
    """The plain pack of this tree's applied mods; open_pack: version 3, with the mods' folders."""
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
    modded_map, overlay = compact_overlay(modded_map, overlay, len(sources))
    mods = [(name, mod_hash32(os.path.join(root, "mods", name))) for name in applied["mods"]]
    plugin_list, plugins = tree_plugins(root) if with_plugins else (b"", [])
    if plugins:
        check_csk(root, applied)
    folders = None
    if open_pack:
        folders = []
        for name, _ in mods:
            if not safe_name(name):
                raise Error(f"mods/{name}: a name players' games won't save (rename it)")
            folders.append((name, mod_folder(os.path.join(root, "mods", name))))
    plain = build_plain(mods, vanilla_sha, [os.path.getsize(s) for s in sources], modded_map, overlay, plugin_list, plugins,
                        folders, mods_index(root, [name for name, _ in mods]))
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
    sv.add_argument("--open", action="store_true", help="send the pack unsealed, with the mods' folders, so players can "
                    "save them (F1 > Online > Download host's mods): no deterrent at all")
    o = ap.parse_args(argv)
    try:
        if o.cmd == "code":
            print(new_code())
            return 0
        code = o.code or new_code()
        plain, mods, plugins = pack_tree(o.tree, not o.no_plugins, o.open)
        pack = plain if o.open else seal(code, plain)
        del plain
        pid = pack_id(code)
        s = put(o.lobby, pid, pack)
        print(f"up: {len(mods)} mod(s), {len(pack) / 1048576:.1f} MB {'open (not sealed)' if o.open else 'sealed'}, pack {pid}")
        for name, h in mods:
            print(f"  {name}  {h:08x}")
        if plugins:
            print(f"plugins (Windows players): {', '.join(plugins)}")
        print(f"invite code: {code}\nplayers: set SSBU_HOST_MODS={code} and SSBU_NET_RELAY={o.lobby}, then start the game")
        if o.open:
            print("open: players who tick F1 > Online > Download host's mods (or set SSBU_HOST_MODS_DOWNLOAD=1) keep the mods")
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
