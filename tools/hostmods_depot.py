"""The lobby's side of host-only mods (docs/HOST_MODS.md): it holds a host's mod pack, in memory
only and only while that host stays connected, and hands it to whoever asks for it by its id.

The lobby can't open a sealed pack: it is sealed with the room's invite code, which the lobby never
sees (the id is a hash of the code, tools/hostmods.py pack_id). An open pack (hostmods.py serve
--open: the plain pack, unsealed, which players may save) it takes as well, under the same limits. A TCP connection on the lobby's port that
starts with "SSBUPAK1" is this service (tools/ssbu_online_lobby.py stream):

    "SSBUPAK1" "PUT " u32 id length (big-endian) id  u64 size
        -> "GO  ", or "TAKN" (an id that is up) or "BIG " (over the limits) and the end
    the pack (sealed, or open)
        -> "OK  " and the pack is up until the connection closes, or "BAD " (not a pack)
    "SSBUPAK1" "GET " u32 id length id
        -> u64 size and the pack (size 0: no pack by that id)
    "SSBUPAK1" "SUM " u32 id length id
        -> u64 size and the pack's sha256 (32 bytes; size 0: no pack by that id, and no hash) - a player
           whose game keeps the pack already (runtime/hle_hostmods.c's cache) asks this, not GET

The limits are the lobby's (tools/ssbu_online_lobby.py --pack-max-mib/--pack-total-mib); the packs are
held as they came up, with no copy of them.
"""
import hashlib
import asyncio
import struct
import time

MAGIC = b"SSBUPAK1"
SEALED_MAGIC = b"SSBUHME1"   # a sealed pack's first bytes (tools/hostmods.py seal)
PLAIN_MAGIC = b"SSBUHMP1"    # an open one's (tools/hostmods.py build_plain, serve --open)
MAX_ID = 64
MAX_PACK = 1 << 30           # bytes one pack may be
MAX_TOTAL = 2 << 30          # bytes all the packs up may be, together
MAX_PACKS = 32
IDLE_TIME = 30               # seconds a PUT may go without a byte
SEND_TIME = 120              # seconds a GET may go without taking a byte of its pack
SEND_CHUNK = 4 << 20


class PackDepot:
    def __init__(self, log=print, max_pack=MAX_PACK, max_total=MAX_TOTAL, max_packs=MAX_PACKS):
        self.log = log
        self.max_pack, self.max_total, self.max_packs = max_pack, max_total, max_packs
        self.packs = {}   # id -> the pack (a bytearray, never changed while up): sealed, or open
        self.sums = {}    # id -> its sha256
        self.coming = 0   # bytes of packs on their way up
        self.gets = 0

    def total(self):
        return sum(len(p) for p in self.packs.values()) + self.coming

    async def serve(self, head, reader, writer, peer="?"):
        """One connection, its 16-byte head read already (head[:8] == MAGIC)."""
        op = head[8:12]
        n = struct.unpack(">I", head[12:16])[0]
        if not 0 < n <= MAX_ID:
            return
        pid = (await asyncio.wait_for(reader.readexactly(n), IDLE_TIME)).decode("ascii", "replace")
        if op == b"SUM ":
            pack, digest = self.packs.get(pid), self.sums.get(pid)
            writer.write(struct.pack(">Q", len(pack)) + digest if pack and digest else struct.pack(">Q", 0))
            await asyncio.wait_for(writer.drain(), IDLE_TIME)
            return
        if op == b"GET ":
            pack = self.packs.get(pid)
            writer.write(struct.pack(">Q", len(pack) if pack else 0))
            if pack:
                view = memoryview(pack)
                t0 = time.monotonic()
                for at in range(0, len(view), SEND_CHUNK):
                    writer.write(view[at:at + SEND_CHUNK])
                    await asyncio.wait_for(writer.drain(), SEND_TIME)
                self.gets += 1
                self.log(f"host mods: pack {pid} ({len(pack)} B) to {peer} in {time.monotonic() - t0:.0f} s")
            await asyncio.wait_for(writer.drain(), SEND_TIME)
            return
        if op != b"PUT ":
            return
        size = struct.unpack(">Q", await asyncio.wait_for(reader.readexactly(8), IDLE_TIME))[0]
        if pid in self.packs:
            writer.write(b"TAKN")
            await writer.drain()
            return
        if size > self.max_pack or self.total() + size > self.max_total or len(self.packs) >= self.max_packs:
            writer.write(b"BIG ")
            await writer.drain()
            self.log(f"host mods: pack {pid} from {peer}: {size} B is over the limits")
            return
        writer.write(b"GO  ")
        self.coming += size
        try:
            body = bytearray(size)
            at = 0
            while at < size:
                got = await asyncio.wait_for(reader.read(min(1 << 20, size - at)), IDLE_TIME)
                if not got:
                    return
                body[at:at + len(got)] = got
                at += len(got)
            digest = await asyncio.to_thread(lambda: hashlib.sha256(body).digest())
        finally:
            self.coming -= size
        if body[:8] not in (SEALED_MAGIC, PLAIN_MAGIC) or pid in self.packs:
            writer.write(b"TAKN" if pid in self.packs else b"BAD ")
            await writer.drain()
            return
        body_open = body[:8] == PLAIN_MAGIC
        self.packs[pid], self.sums[pid] = body, digest
        del body
        t0 = time.monotonic()
        self.log(f"host mods: pack {pid} ({size} B{', open' if body_open else ''}) up from {peer}")
        try:
            writer.write(b"OK  ")
            await writer.drain()
            while await reader.read(4096):   # (the host says nothing more: it is up until it goes)
                pass
        finally:
            self.packs.pop(pid, None)
            self.sums.pop(pid, None)
            self.log(f"host mods: pack {pid} down after {time.monotonic() - t0:.0f} s")
