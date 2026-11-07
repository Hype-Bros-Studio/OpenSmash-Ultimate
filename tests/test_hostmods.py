"""Host-only mods (docs/HOST_MODS.md): the sealed pack (tools/hostmods.py), the lobby's pack service
(tools/hostmods_depot.py) through the online lobby's TCP port (tools/ssbu_online_lobby.py), and the
runtime's C crypto (runtime/hostmods_crypto.h) against Python's when a C compiler is there."""
import asyncio
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))

import hostmods as hm   # noqa: E402

CODE = "ABCDE-FGHIJ-KLMNO-PQRST"


def layer_map(entries, size):
    return hm.LAYER_MAGIC + struct.pack("<QII", size, len(entries), 0) + b"".join(
        struct.pack("<QQII", at, off, src, 0) for at, src, off in entries)


class Seal(unittest.TestCase):
    def test_round_trip_in_chunks(self):
        plain = os.urandom(10_000)
        for chunk in (1000, 4096, 1 << 20):
            sealed = hm.seal(CODE, plain, chunk=chunk)
            self.assertEqual(hm.unseal(CODE, sealed), plain)
        self.assertEqual(hm.unseal(CODE, hm.seal(CODE, b"")), b"")

    def test_code_is_case_and_dash_blind(self):
        sealed = hm.seal(CODE, b"mods")
        self.assertEqual(hm.unseal("abcdefghij klmnopqrst", sealed), b"mods")
        self.assertEqual(hm.pack_id(CODE), hm.pack_id("abcde fghij-klmno pqrst"))
        self.assertNotEqual(hm.pack_id(CODE), hm.pack_id("ABCDE-FGHIJ-KLMNO-PQRSU"))
        self.assertEqual(len(hm.pack_id(CODE)), 20)

    def test_wrong_code_or_changes_fail(self):
        plain = os.urandom(5000)
        sealed = bytearray(hm.seal(CODE, plain, chunk=1000))
        with self.assertRaises(hm.Error):
            hm.unseal("ABCDE-FGHIJ-KLMNO-PQRSU", bytes(sealed))
        bad = bytearray(sealed)
        bad[hm.HEAD + 1500] ^= 1   # a byte of a chunk
        with self.assertRaises(hm.Error):
            hm.unseal(CODE, bytes(bad))
        bad = bytearray(sealed)
        bad[30] ^= 1               # the head's size
        with self.assertRaises(hm.Error):
            hm.unseal(CODE, bytes(bad))
        # two whole chunks swapped
        one, two = hm.HEAD, hm.HEAD + 1000 + hm.TAG
        bad = bytearray(sealed)
        bad[one:two], bad[two:two + 1016] = sealed[two:two + 1016], sealed[one:two]
        with self.assertRaises(hm.Error):
            hm.unseal(CODE, bytes(bad))
        # cut after two whole chunks, its head saying so (the head is in each tag)
        cut = bytes(sealed[:hm.HEAD + 2 * (1000 + hm.TAG)])
        cut = cut[:24] + struct.pack("<Q", 2000) + cut[32:]
        with self.assertRaises(hm.Error):
            hm.unseal(CODE, cut)

    def test_salt_makes_each_seal_new(self):
        self.assertNotEqual(hm.seal(CODE, b"same"), hm.seal(CODE, b"same"))


class Pack(unittest.TestCase):
    def test_plain_round_trip(self):
        m = layer_map([(0, 0, 0), (0x100, 2, 0), (0x180, 0, 0x180)], 0x400)
        p = hm.build_plain([("Some Mod", 0x12345678), ("Ünïcode, mod", 0xFFFFFFFF)], b"\x11" * 32, [5, 1 << 33], m, b"overlay")
        got = hm.parse_plain(p)
        self.assertEqual(got["mods"], [("Some Mod", 0x12345678), ("Ünïcode, mod", 0xFFFFFFFF)])
        self.assertEqual(got["source_sizes"], [5, 1 << 33])
        self.assertEqual(got["map"], m)
        self.assertEqual(got["overlay"], b"overlay")
        hm.check_map(m, 2)
        with self.assertRaises(hm.Error):
            hm.check_map(m, 1)   # source 2 is past the overlay

    def test_tree_hash_skips_what_the_runtime_skips(self):
        with tempfile.TemporaryDirectory() as tmp:
            os.makedirs(os.path.join(tmp, "Fighter", "Sub"))
            with open(os.path.join(tmp, "Fighter", "Sub", "Model.NUMDLB"), "wb") as f:
                f.write(b"x" * 10)
            base = hm.tree_hash(tmp)
            want = hm.fnv64(hm.fnv64(hm.FNV0, b"fighter/sub/model.numdlb"), struct.pack("<Q", 10))
            self.assertEqual(base, want)
            for skipped in ("info.toml", "preview.webp", "README.md", ".hidden", "plugin.dll", "lib.so"):
                with open(os.path.join(tmp, skipped), "wb") as f:
                    f.write(b"y")
            self.assertEqual(hm.tree_hash(tmp), base)
            with open(os.path.join(tmp, "config.json"), "wb") as f:
                f.write(b"{}")
            self.assertNotEqual(hm.tree_hash(tmp), base)

    def test_pack_tree(self):
        with tempfile.TemporaryDirectory() as root:
            modules = os.path.join(root, "dump", "modules")
            work = os.path.join(root, "dump", "mods")
            os.makedirs(modules)
            os.makedirs(work)
            os.makedirs(os.path.join(root, "mods", "A Mod", "fighter"))
            with open(os.path.join(root, "mods", "A Mod", "fighter", "f.bin"), "wb") as f:
                f.write(b"abc")
            src = os.path.join(root, "base.nca")
            with open(src, "wb") as f:
                f.write(b"\0" * 64)
            vmap, mmap_, ov = (os.path.join(modules, "romfs.layers"), os.path.join(modules, hm.MODDED_MAP),
                               os.path.join(work, "overlay.bin"))
            with open(vmap, "wb") as f:
                f.write(layer_map([(0, 0, 0)], 64))
            with open(mmap_, "wb") as f:
                f.write(layer_map([(0, 1, 0), (8, 0, 8)], 64))
            with open(ov, "wb") as f:
                f.write(b"modded!!")
            with open(os.path.join(modules, "romfs_vanilla.cfg"), "w") as f:
                f.write(f"layered\n{vmap}\n{src}\n")
            with open(os.path.join(modules, "romfs.cfg"), "w") as f:
                f.write(f"layered\n{mmap_}\n{src}\n{ov}\n")
            with open(os.path.join(work, "applied.json"), "w") as f:
                f.write('{"mods": ["A Mod"], "files": 1}')
            plain, mods = hm.pack_tree(root)
            got = hm.parse_plain(plain)
            self.assertEqual(mods, [("A Mod", hm.mod_hash32(os.path.join(root, "mods", "A Mod")))])
            self.assertEqual(got["source_sizes"], [64])
            self.assertEqual(got["overlay"], b"modded!!")
            with open(os.path.join(work, "applied.json"), "w") as f:
                f.write('{"mods": ["A Mod"], "arcropolis": true}')
            with self.assertRaises(hm.Error):
                hm.pack_tree(root)


class Depot(unittest.TestCase):
    """The pack service through the online lobby's own TCP handler (OnlineLobby.stream)."""

    def run_lobby(self, body):
        import ssbu_online_lobby as online
        with tempfile.TemporaryDirectory() as tmp:
            lobby = online.OnlineLobby(online.parse_args(["--state", tmp, "--nex-log", "0"]))
            lobby.say = lambda *a: None
            lobby.depot.log = lambda *a: None
            loop = asyncio.new_event_loop()
            server = loop.run_until_complete(asyncio.start_server(lobby.stream, "127.0.0.1", 0))
            port = server.sockets[0].getsockname()[1]
            t = threading.Thread(target=loop.run_forever, daemon=True)
            t.start()
            try:
                body(f"127.0.0.1:{port}", lobby)
            finally:
                async def down():
                    server.close()
                    for task in asyncio.all_tasks():
                        if task is not asyncio.current_task():
                            task.cancel()
                    await asyncio.sleep(0.05)
                asyncio.run_coroutine_threadsafe(down(), loop).result(5)
                loop.call_soon_threadsafe(loop.stop)
                t.join(5)
                loop.close()

    def test_put_get_and_down_with_the_host(self):
        def body(addr, lobby):
            pid = hm.pack_id(CODE)
            self.assertIsNone(hm.get(addr, pid))
            plain = os.urandom(3 << 20)
            sealed = hm.seal(CODE, plain)
            host = hm.put(addr, pid, sealed)
            self.assertEqual(hm.unseal(CODE, hm.get(addr, pid)), plain)
            with self.assertRaises(hm.Error):   # one host per code
                hm.put(addr, pid, sealed)
            host.close()
            for _ in range(100):
                if not lobby.depot.packs:
                    break
                threading.Event().wait(0.02)
            self.assertIsNone(hm.get(addr, pid))
        self.run_lobby(body)

    def test_limits_and_not_a_pack(self):
        def body(addr, lobby):
            lobby.depot.max_pack = 1000
            with self.assertRaises(hm.Error):
                hm.put(addr, "big", hm.seal(CODE, b"x" * 2000))
            with self.assertRaises(hm.Error):
                hm.put(addr, "junk", b"not sealed at all")
            self.assertEqual(lobby.depot.packs, {})
        self.run_lobby(body)


ROOT = os.path.abspath(os.path.join(HERE, ".."))


def lobby_thread():
    """An online lobby on a free port of 127.0.0.1, in a thread: (its host:port, the lobby, stop)."""
    import ssbu_online_lobby as online
    tmp = tempfile.TemporaryDirectory()
    lobby = online.OnlineLobby(online.parse_args(["--state", tmp.name, "--nex-log", "0"]))
    lobby.say = lambda *a: None
    lobby.depot.log = lambda *a: None
    loop = asyncio.new_event_loop()
    server = loop.run_until_complete(asyncio.start_server(lobby.stream, "127.0.0.1", 0))
    port = server.sockets[0].getsockname()[1]
    t = threading.Thread(target=loop.run_forever, daemon=True)
    t.start()

    def stop():
        async def down():
            server.close()
            for task in asyncio.all_tasks():
                if task is not asyncio.current_task():
                    task.cancel()
            await asyncio.sleep(0.05)
        asyncio.run_coroutine_threadsafe(down(), loop).result(5)
        loop.call_soon_threadsafe(loop.stop)
        t.join(5)
        loop.close()
        tmp.cleanup()
    return f"127.0.0.1:{port}", lobby, stop


@unittest.skipUnless(sys.platform.startswith("linux") and shutil.which("clang"), "needs clang on Linux")
class RuntimeRomfs(unittest.TestCase):
    """The runtime's side (tools/dev/hostmodstest.c: hle_fs.c and hle_hostmods.c, no game): a pack up on a
    real lobby, fetched, opened and read through as the game's RomFS."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.exe = os.path.join(cls.tmp.name, "hostmodstest")
        r = subprocess.run(["clang", "-O1", "-w", "-I", "runtime/macos/include", "-I", "runtime/linux/include", "-I", "runtime",
                            "tools/dev/hostmodstest.c", "runtime/linux/win32.c", "-lpthread", "-o", cls.exe],
                           cwd=ROOT, capture_output=True, text=True)
        if r.returncode:
            raise AssertionError("hostmodstest.c did not build:\n" + r.stdout + r.stderr)
        cls.addr, cls.lobby, cls.stop = lobby_thread()

    @classmethod
    def tearDownClass(cls):
        cls.stop()
        cls.tmp.cleanup()

    def game(self, d):
        """A made-up game's data in d: two sources, the layer map over them, romfs_vanilla.cfg (romfs.cfg
        this player's own mods: nothing host mods read). The image starts as the runtime wants a RomFS
        (fstest.c's: a root folder, hello.txt)."""
        data = os.path.join(d, "dump", "modules")
        os.makedirs(data)
        image = bytearray(os.urandom(4096))
        head = struct.pack("<10Q", 0x50, 0, 0, 0x50, 24, 0, 0, 0x68, 44, 0x94)
        image[:0x99] = (head + struct.pack("<6I", 0, 0xFFFFFFFF, 0xFFFFFFFF, 0, 0, 0)
                        + struct.pack("<IIQQII", 0, 0xFFFFFFFF, 0, 5, 0, 9) + b"hello.txt\0\0\0" + b"hello")
        a, b = os.path.join(data, "a.bin"), os.path.join(data, "b.bin")
        with open(a, "wb") as f:
            f.write(image[:2048])
        with open(b, "wb") as f:
            f.write(image[2048:])
        vmap = os.path.join(data, "romfs.layers")
        with open(vmap, "wb") as f:
            f.write(layer_map([(0, 0, 0), (2048, 1, 0)], 4096))
        with open(os.path.join(data, "romfs_vanilla.cfg"), "w") as f:
            f.write(f"layered\n{vmap}\n{a}\n{b}\n")
        with open(os.path.join(data, "romfs.cfg"), "w") as f:
            f.write("layered\n/nowhere/romfs_mods.layers\n/nowhere/a\n/nowhere/overlay.bin\n")
        return data, bytes(image), vmap, [a, b]

    def pack(self, image, vmap, sources):
        """The host's mods over that game: 0x200..0x400 changed, 512 bytes added past the end."""
        overlay = os.urandom(0x400)
        modded = layer_map([(0, 0, 0), (0x200, 2, 0), (0x400, 0, 0x400), (2048, 1, 0), (4096, 2, 0x200)], 4096 + 512)
        import hashlib
        with open(vmap, "rb") as f:
            sha = hashlib.sha256(f.read()).digest()
        plain = hm.build_plain([("Stage Pack", 0x0BADF00D), ("Costume, Slot 3", 0x12345678)], sha,
                               [os.path.getsize(s) for s in sources], modded, overlay)
        want = image[:0x200] + overlay[:0x200] + image[0x400:] + overlay[0x200:]
        return plain, want

    def run_game(self, data, code):
        out = os.path.join(os.path.dirname(data), "image.bin")
        env = dict(os.environ, SSBU_HOST_MODS=code, SSBU_NET_RELAY=self.addr)
        r = subprocess.run([self.exe, data, out], capture_output=True, text=True, env=env, timeout=60)
        got = None
        if r.returncode == 0:
            with open(out, "rb") as f:
                got = f.read()
        return r, got

    def test_reads_the_hosts_mods(self):
        with tempfile.TemporaryDirectory() as d:
            data, image, vmap, sources = self.game(d)
            plain, want = self.pack(image, vmap, sources)
            code = hm.new_code()
            host = hm.put(self.addr, hm.pack_id(code), hm.seal(code, plain, chunk=1000))
            try:
                r, got = self.run_game(data, code.lower().replace("-", " "))
                self.assertEqual(r.returncode, 0, r.stderr)
                self.assertEqual(got, want)
                self.assertEqual(r.stdout.split("\n")[:2], ["0badf00d Stage Pack", "12345678 Costume, Slot 3"])
                self.assertIn("in memory only", r.stderr)
                # (nothing of the pack left on disk: image.bin is this test's own)
                self.assertEqual(sorted(os.listdir(d)), ["dump"])
                self.assertEqual(sorted(os.listdir(os.path.dirname(data))), ["image.bin", "modules"])
                self.assertEqual(sorted(os.listdir(data)), ["a.bin", "b.bin", "romfs.cfg", "romfs.layers", "romfs_vanilla.cfg"])
            finally:
                host.close()

    def test_refusals(self):
        with tempfile.TemporaryDirectory() as d:
            data, image, vmap, sources = self.game(d)
            plain, _ = self.pack(image, vmap, sources)
            code, other = hm.new_code(), hm.new_code()
            r, _ = self.run_game(data, code)
            self.assertEqual(r.returncode, 3)
            self.assertIn("no host has a pack up", r.stderr)
            # a pack sealed with another code, up under this code's id
            host = hm.put(self.addr, hm.pack_id(code), hm.seal(other, plain))
            try:
                r, _ = self.run_game(data, code)
                self.assertEqual(r.returncode, 3)
                self.assertIn("doesn't open with that invite code", r.stderr)
            finally:
                host.close()
            host = hm.put(self.addr, hm.pack_id(other), hm.seal(other, plain))
            try:
                with open(sources[1], "ab") as f:   # another version of the game's data
                    f.write(b"!")
                r, _ = self.run_game(data, other)
                self.assertEqual(r.returncode, 3)
                self.assertIn("isn't the host's", r.stderr)
                with open(vmap, "ab") as f:
                    f.write(b"!")
                r, _ = self.run_game(data, other)
                self.assertEqual(r.returncode, 3)
                self.assertIn("the host's game isn't this one", r.stderr)
            finally:
                host.close()


def c_compiler():
    for cc in ("gcc", "clang", "cc"):
        if shutil.which(cc):
            return cc
    return None


@unittest.skipUnless(c_compiler(), "no C compiler")
class CryptoInC(unittest.TestCase):
    """runtime/hostmods_crypto.h opens what tools/hostmods.py seals (tests/hostmods_crypto_test.c)."""

    def test_c_opens_python_seal(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe = os.path.join(tmp, "hmtest")
            subprocess.run([c_compiler(), "-O2", "-Wall", "-Wextra", "-Werror", "-o", exe,
                            os.path.join(HERE, "hostmods_crypto_test.c")], check=True)
            for n, chunk in ((0, 64), (1, 64), (63, 64), (64, 64), (65, 64), (100_000, 4096), (3 << 20, 1 << 20)):
                plain = os.urandom(n)
                sealed = os.path.join(tmp, "p.sealed")
                with open(sealed, "wb") as f:
                    f.write(hm.seal(CODE, plain, chunk=chunk))
                out = subprocess.run([exe, "abcde-fghij klmnopqrst", sealed], capture_output=True, check=True).stdout
                self.assertEqual(out, plain, (n, chunk))
                bad = subprocess.run([exe, "ABCDE-FGHIJ-KLMNO-PQRSU", sealed], capture_output=True)
                self.assertNotEqual(bad.returncode, 0)
            ids = subprocess.run([exe, "--id", CODE], capture_output=True, check=True).stdout.decode().strip()
            self.assertEqual(ids, hm.pack_id(CODE))
            # SHA-256 of lengths about the block edges
            for n in (0, 1, 55, 56, 63, 64, 65, 1000):
                data = os.urandom(n)
                path = os.path.join(tmp, "d")
                with open(path, "wb") as f:
                    f.write(data)
                got = subprocess.run([exe, "--sha256", path], capture_output=True, check=True).stdout.decode().strip()
                import hashlib
                self.assertEqual(got, hashlib.sha256(data).hexdigest(), n)


if __name__ == "__main__":
    unittest.main()
