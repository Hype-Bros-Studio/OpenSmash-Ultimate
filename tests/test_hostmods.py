"""Host-only mods (docs/HOST_MODS.md): the sealed and open packs (tools/hostmods.py), the lobby's pack service
(tools/hostmods_depot.py) through the online lobby's TCP port (tools/ssbu_online_lobby.py), and the
runtime's C crypto (runtime/hostmods_crypto.h) against Python's when a C compiler is there."""
import asyncio
import hashlib
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

    def test_plugins_round_trip(self):
        m = layer_map([(0, 0, 0)], 0x10)
        plugins = [("moveset_x", 1, b"MZ library", b"image bytes"), ("moveset_x", 2, b"\x7fELF", b"")]
        p = hm.build_plain([("A", 1)], b"\0" * 32, [1], m, b"ov", b"smashline\nmoveset_x\n", plugins)
        got = hm.parse_plain(p)
        self.assertEqual(got["plugins"], plugins)
        self.assertEqual(got["plugin_list"], b"smashline\nmoveset_x\n")
        self.assertEqual(got["overlay"], b"ov")
        with self.assertRaises(hm.Error):
            hm.parse_plain(p + b"!")

    def test_version_1_and_2_still_read(self):
        m = layer_map([(0, 0, 0)], 0x10)
        v2 = hm.build_plain([("A", 1)], b"\0" * 32, [1], m, b"ov", b"list\n", [("p", 1, b"lib", b"img")])
        self.assertEqual(struct.unpack_from("<I", v2, 8)[0], 2)   # (a sealed pack's: no mods' folders)
        got = hm.parse_plain(v2)
        self.assertEqual((got["plugins"], got["folders"]), ([("p", 1, b"lib", b"img")], None))
        v2 = hm.build_plain([("A", 1)], b"\0" * 32, [1], m, b"ov")
        v1 = v2[:8] + struct.pack("<I", 1) + v2[12:-8]   # (no plugin list or plugins)
        got = hm.parse_plain(v1)
        self.assertEqual(got["plugins"], [])
        self.assertEqual(got["overlay"], b"ov")
        self.assertIsNone(got["folders"])

    def test_open_pack_round_trip(self):
        m = layer_map([(0, 0, 0)], 0x10)
        folders = [("Some Mod", [("config.json", b"{}"), ("fighter/mario/model/body/c00/model.numdlb", b"\0" * 100),
                                 ("plugin.nro", b"NRO0"), (".gitkeep", b""), ("Ünï/ok.bin", b"x")]),
                   ("Empty", [])]
        p = hm.build_plain([("Some Mod", 1), ("Empty", 2)], b"\0" * 32, [1], m, b"ov", b"list", [("p", 2, b"l", b"i")], folders)
        self.assertEqual(p[:8], hm.PLAIN_MAGIC)
        self.assertEqual(struct.unpack_from("<I", p, 8)[0], 3)
        got = hm.parse_plain(p)
        self.assertEqual(got["folders"], folders)
        self.assertEqual(got["plugins"], [("p", 2, b"l", b"i")])
        self.assertEqual(got["overlay"], b"ov")
        with self.assertRaises(hm.Error):
            hm.parse_plain(p + b"!")
        with self.assertRaises(hm.Error):
            hm.parse_plain(p[:-1])

    BAD_PATHS = ["..", "../x", "a/../b", "a/..", ".", "./a", "/abs", "/etc/passwd", "C:/x", "C:x", "c:\\x", "a\\b",
                 "..\\x", "", "a//b", "a/", "con", "NUL.txt", "a/Com1.bin", "lpt9", "x.", "x ", "a\x01b", "a:stream"]
    BAD_NAMES = ["..", ".", ".packs", "a/b", "a\\b", "C:", "", "AUX", "mod.", "../mods"]

    def test_names_that_leave_the_folder_are_refused(self):
        for good in ("config.json", "fighter/mario/c00/model.numdlb", ".gitkeep", "a b/c d.txt", "Ünï/ok", "console.bin", "com10"):
            self.assertTrue(hm.safe_path(good), good)
        m = layer_map([(0, 0, 0)], 0x10)
        for bad in self.BAD_PATHS:
            self.assertFalse(hm.safe_path(bad), bad)
            p = hm.build_plain([("M", 1)], b"\0" * 32, [1], m, b"ov", folders=[("M", [("ok.bin", b"1"), (bad, b"evil")])])
            with self.assertRaises(hm.Error, msg=bad):
                hm.parse_plain(p)
        for bad in self.BAD_NAMES:
            self.assertFalse(hm.safe_name(bad), bad)
            p = hm.build_plain([("M", 1)], b"\0" * 32, [1], m, b"ov", folders=[(bad, [("ok.bin", b"1")])])
            with self.assertRaises(hm.Error, msg=bad):
                hm.parse_plain(p)

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
            plain, mods, plugins = hm.pack_tree(root)
            got = hm.parse_plain(plain)
            self.assertEqual(mods, [("A Mod", hm.mod_hash32(os.path.join(root, "mods", "A Mod")))])
            self.assertEqual(got["source_sizes"], [64])
            self.assertEqual(got["overlay"], b"modded!!")
            self.assertEqual((plugins, got["plugins"]), ([], []))
            # the turned-on mods' translated plugins (tools/skyline.py's list), those plugins.txt lists
            skyline, built = os.path.join(root, "dump", "skyline"), os.path.join(root, "build", "plugins")
            os.makedirs(skyline)
            os.makedirs(built)
            for name in ("one", "gone"):
                with open(os.path.join(built, name + ".dll"), "wb") as f:
                    f.write(b"MZ " + name.encode())
                with open(os.path.join(modules, name + ".reloc.bin"), "wb") as f:
                    f.write(b"image " + name.encode())
            with open(os.path.join(skyline, "plugins.txt"), "w", newline="\n") as f:
                f.write("smashline\none  # a moveset\n")
            with open(os.path.join(skyline, "plugin_dlls.txt"), "w") as f:
                f.write("build/plugins/one.dll\nbuild/plugins/gone.dll\n")
            plain, _, plugins = hm.pack_tree(root)
            got = hm.parse_plain(plain)
            self.assertEqual(plugins, ["one"])
            self.assertEqual(got["plugins"], [("one", 1, b"MZ one", b"image one")])
            self.assertEqual(got["plugin_list"], b"smashline\none  # a moveset\n")
            self.assertEqual(hm.parse_plain(hm.pack_tree(root, with_plugins=False)[0])["plugins"], [])
            self.assertIsNone(got["folders"])   # (sealed packs: no mods' folders)
            # an open pack: the turned-on mods' folders, every file
            mod = os.path.join(root, "mods", "A Mod")
            os.makedirs(os.path.join(mod, "plugins"))
            os.makedirs(os.path.join(root, "mods", "Not On"))
            for rel, data in (("config.json", b'{"a": 1}'), ("plugins/libmoveset.nro", b"NRO0 bytes")):
                with open(os.path.join(mod, rel), "wb") as f:
                    f.write(data)
            with open(os.path.join(root, "mods", "Not On", "x.bin"), "wb") as f:
                f.write(b"off")
            plain, _, _ = hm.pack_tree(root, open_pack=True)
            got = hm.parse_plain(plain)
            self.assertEqual(got["folders"], [("A Mod", [("config.json", b'{"a": 1}'), ("fighter/f.bin", b"abc"),
                                                         ("plugins/libmoveset.nro", b"NRO0 bytes")])])
            self.assertEqual(got["plugins"], [("one", 1, b"MZ one", b"image one")])
            os.remove(os.path.join(modules, "one.reloc.bin"))
            with self.assertRaises(hm.Error):
                hm.pack_tree(root)
            # an ARCropolis tree (version 4): no overlay, ARCropolis' listed folders whole, each mod's plugin.nro
            # by its build id (dump/skyline/plugins.json) as its Windows DLL
            with open(os.path.join(work, "applied.json"), "w") as f:
                f.write('{"mods": ["A Mod"], "arcropolis": true}')
            os.remove(os.path.join(mod, "plugins", "libmoveset.nro"))
            os.rmdir(os.path.join(mod, "plugins"))
            nro = bytes(0x10) + b"NRO0" + bytes(0x2c) + bytes.fromhex("ab12") + bytes(30)
            with open(os.path.join(mod, "plugin.nro"), "wb") as f:
                f.write(nro)
            with open(os.path.join(modules, "one.reloc.bin"), "wb") as f:
                f.write(b"image one")
            with open(os.path.join(skyline, "mods.txt"), "w") as f:
                f.write("A Mod\n")
            with open(os.path.join(skyline, "plugins.json"), "w") as f:
                f.write('{"AB12' + "0" * 60 + '": {"module": "one"}}')
            plain, mods, plugins = hm.pack_tree(root)
            got = hm.parse_plain(plain)
            self.assertEqual(got["version"], hm.ARC_VERSION)
            self.assertEqual((got["overlay"], got["map"]), (b"", b""))
            self.assertEqual(got["source_sizes"], [64])
            self.assertEqual(plugins, ["one"])
            self.assertEqual(got["folders"], [("A Mod", [("config.json", b'{"a": 1}'), ("fighter/f.bin", b"abc"),
                                                         ("plugin.nro", nro)])])
            self.assertEqual(sorted(p for p, *_ in got["mods_index"]), ["config.json", "fighter/f.bin", "plugin.nro"])


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
            self.assertEqual(hm.pack_sum(addr, pid), (len(sealed), hashlib.sha256(sealed).digest()))
            self.assertIsNone(hm.pack_sum(addr, "no such pack"))
            with self.assertRaises(hm.Error):   # one host per code
                hm.put(addr, pid, sealed)
            host.close()
            for _ in range(100):
                if not lobby.depot.packs:
                    break
                threading.Event().wait(0.02)
            self.assertIsNone(hm.get(addr, pid))
        self.run_lobby(body)

    def test_open_pack(self):
        def body(addr, lobby):
            pid = hm.pack_id(CODE)
            m = layer_map([(0, 0, 0)], 0x10)
            plain = hm.build_plain([("A", 1)], b"\0" * 32, [1], m, b"ov" * 1000, folders=[("A", [("f.bin", os.urandom(5000))])])
            host = hm.put(addr, pid, plain)
            try:
                self.assertEqual(hm.get(addr, pid), plain)
                self.assertEqual(hm.parse_plain(hm.get(addr, pid))["folders"][0][0], "A")
            finally:
                host.close()
            lobby.depot.max_pack = 1000   # (the same limits as a sealed one)
            with self.assertRaises(hm.Error):
                hm.put(addr, "big-open", plain)
            for _ in range(100):
                if not lobby.depot.packs:
                    break
                threading.Event().wait(0.02)
            self.assertEqual(lobby.depot.packs, {})
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

    def pack(self, image, vmap, sources, plugins=(), folders=None):
        """The host's mods over that game: 0x200..0x400 changed, 512 bytes added past the end."""
        overlay = os.urandom(0x400)
        modded = layer_map([(0, 0, 0), (0x200, 2, 0), (0x400, 0, 0x400), (2048, 1, 0), (4096, 2, 0x200)], 4096 + 512)
        import hashlib
        with open(vmap, "rb") as f:
            sha = hashlib.sha256(f.read()).digest()
        plain = hm.build_plain([("Stage Pack", 0x0BADF00D), ("Costume, Slot 3", 0x12345678)], sha,
                               [os.path.getsize(s) for s in sources], modded, overlay,
                               b"smashline\nmoveset_x\n" if plugins else b"", plugins, folders)
        want = image[:0x200] + overlay[:0x200] + image[0x400:] + overlay[0x200:]
        return plain, want

    def run_game(self, data, code, **more):
        out = os.path.join(os.path.dirname(data), "image.bin")
        env = dict(os.environ, SSBU_HOST_MODS=code, SSBU_NET_RELAY=self.addr)
        env.pop("SSBU_HOST_MODS_DOWNLOAD", None)
        env.pop("HOSTMODSTEST_SETTING", None)
        env["SSBU_HOST_MODS_CACHE"] = "0"   # (test_pack_kept_between_starts turns it on)
        env.update(more)
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

    FOLDERS = [("Stage Pack", [("config.json", b'{"stage": 1}'), ("stage/battlefield/normal/model/m.numdlb", b"\1" * 3000),
                               ("plugins/libstage.nro", b"NRO0 plugin")]),
               ("Costume, Slot 3", [("fighter/mario/c03/model.numdlb", os.urandom(70000)), ("empty.bin", b"")])]

    def tree_files(self, folder):
        out = {}
        for top, _, names in os.walk(folder):
            for n in names:
                with open(os.path.join(top, n), "rb") as f:
                    out[os.path.relpath(os.path.join(top, n), folder).replace(os.sep, "/")] = f.read()
        return out

    def test_open_pack_saved_when_asked(self):
        with tempfile.TemporaryDirectory() as d:
            data, image, vmap, sources = self.game(d)
            plain, want = self.pack(image, vmap, sources, folders=self.FOLDERS)
            code = hm.new_code()
            host = hm.put(self.addr, hm.pack_id(code), plain)   # (open: not sealed)
            try:
                mods = os.path.join(d, "mods")
                # not asked: played from memory, nothing saved
                r, got = self.run_game(data, code)
                self.assertEqual(r.returncode, 0, r.stderr)
                self.assertEqual(got, want)
                self.assertIn("an open pack", r.stderr)
                self.assertIn("Download host's mods", r.stderr)
                self.assertFalse(os.path.exists(mods))
                # SSBU_HOST_MODS_DOWNLOAD=0 wins over the setting
                setting = os.path.join(d, "setting.txt")
                with open(setting, "w") as f:
                    f.write("# download\non\n")
                r, _ = self.run_game(data, code, SSBU_HOST_MODS_DOWNLOAD="0", HOSTMODSTEST_SETTING=setting)
                self.assertEqual(r.returncode, 0, r.stderr)
                self.assertFalse(os.path.exists(mods))
                # the setting (online_host_mods_download.txt's stand-in) "on": saved
                r, got = self.run_game(data, code, HOSTMODSTEST_SETTING=setting)
                self.assertEqual(r.returncode, 0, r.stderr)
                self.assertEqual(got, want)
                self.assertEqual(sorted(os.listdir(mods)), ["Costume, Slot 3", "Stage Pack"])
                for name, files in self.FOLDERS:
                    self.assertEqual(self.tree_files(os.path.join(mods, name)), dict(files), name)
                self.assertIn("2 saved into mods/", r.stderr)
                # again (SSBU_HOST_MODS_DOWNLOAD=1): the same folders are left as they are
                r, _ = self.run_game(data, code, SSBU_HOST_MODS_DOWNLOAD="1")
                self.assertEqual(r.returncode, 0, r.stderr)
                self.assertIn("0 saved into mods/, 2 there already", r.stderr)
                self.assertEqual(sorted(os.listdir(mods)), ["Costume, Slot 3", "Stage Pack"])
                # the player's own Stage Pack (another file): the host's goes to "Stage Pack (host)"
                with open(os.path.join(mods, "Stage Pack", "config.json"), "wb") as f:
                    f.write(b'{"stage": 2}')
                r, _ = self.run_game(data, code, SSBU_HOST_MODS_DOWNLOAD="1")
                self.assertEqual(r.returncode, 0, r.stderr)
                self.assertEqual(sorted(os.listdir(mods)), ["Costume, Slot 3", "Stage Pack", "Stage Pack (host)"])
                self.assertEqual(self.tree_files(os.path.join(mods, "Stage Pack (host)")), dict(self.FOLDERS[0][1]))
                self.assertEqual(self.tree_files(os.path.join(mods, "Stage Pack"))["config.json"], b'{"stage": 2}')
                r, _ = self.run_game(data, code, SSBU_HOST_MODS_DOWNLOAD="1")   # ("(host)" is the host's: left)
                self.assertIn("0 saved into mods/, 2 there already", r.stderr)
                self.assertEqual(sorted(os.listdir(mods)), ["Costume, Slot 3", "Stage Pack", "Stage Pack (host)"])
            finally:
                host.close()

    def test_open_pack_paths_that_leave_mods_refused(self):
        with tempfile.TemporaryDirectory() as d:
            data, image, vmap, sources = self.game(d)
            outside = os.path.join(d, "escaped.txt")
            folders = [("../escaped", [("x.txt", b"evil")]), (".packs", [("x.txt", b"evil")]), ("C:", [("x.txt", b"evil")]),
                       ("Up", [("ok.txt", b"fine"), ("../../escaped.txt", b"evil")]),
                       ("Abs", [(outside, b"evil")]), ("Back", [("..\\..\\escaped.txt", b"evil")]),
                       ("Drive", [("C:escaped.txt", b"evil")]), ("Dev", [("nul", b"evil")]),
                       ("Good", [("ok.txt", b"fine")])]
            plain, want = self.pack(image, vmap, sources, folders=folders)
            code = hm.new_code()
            host = hm.put(self.addr, hm.pack_id(code), plain)
            try:
                r, got = self.run_game(data, code, SSBU_HOST_MODS_DOWNLOAD="1")
                self.assertEqual(r.returncode, 0, r.stderr)
                self.assertEqual(got, want)
                self.assertEqual(os.listdir(os.path.join(d, "mods")), ["Good"])
                self.assertEqual(self.tree_files(os.path.join(d, "mods", "Good")), {"ok.txt": b"fine"})
                self.assertEqual(sorted(os.listdir(d)), ["dump", "mods"])
                self.assertFalse(os.path.exists(outside))
                self.assertEqual(r.stderr.count("[hostmods] not saved"), 8, r.stderr)
                self.assertIn("1 saved into mods/", r.stderr)
            finally:
                host.close()

    def arc_pack(self, data, vmap, sources, folders):
        """An ARCropolis pack (version 4) over that game: no map or overlay, the folders as the SD card's ultimate/mods."""
        with open(vmap, "rb") as f:
            sha = hashlib.sha256(f.read()).digest()
        return hm.build_plain([("Fighter Mod", 0x0BADF00D)], sha, [os.path.getsize(s) for s in sources], b"", b"",
                              b"arcropolis\n", [("other_os", 99, b"lib", b"image")], folders, version=hm.ARC_VERSION)

    def test_arcropolis_pack_is_the_sd_cards_mods(self):
        with tempfile.TemporaryDirectory() as d:
            data, image, vmap, sources = self.game(d)
            folders = [("Fighter Mod", sorted([("config.json", b'{"x": 1}'), ("fighter/mario/model/body/c08/model.numdlb", b"\2" * 5000),
                                               ("fighter/mario/model/body/c08/x.marker", b""), ("plugin.nro", b"NRO0 x")])),
                       ("WindowSmash characters", [("ui/message/msg_name.msbt", b"names")])]
            plain = self.arc_pack(data, vmap, sources, folders)
            code = hm.new_code()
            host = hm.put(self.addr, hm.pack_id(code), hm.seal(code, plain))
            try:
                r, got = self.run_game(data, code, HOSTMODSTEST_SD=";Fighter Mod;fighter mod/CONFIG.json;Fighter Mod/fighter/mario/model/body/c08;"
                                                                  "Fighter Mod/plugin.nro;Fighter Mod/fighter/mar;Nope/x;Fighter Mod/config.json/")
                self.assertEqual(r.returncode, 0, r.stderr)
                self.assertEqual(got, image)   # (this game's own data: ARCropolis lays the mods over it)
                self.assertIn("arcropolis 1", r.stdout)
                self.assertIn("built-in copies", r.stderr)
                sd = r.stdout[r.stdout.index("sd "):].split("arcropolis")[0]
                self.assertEqual(sd.split("\n"), [
                    "sd  2 0 ", "  dir Fighter Mod 0", "  dir WindowSmash characters 0",
                    "sd Fighter Mod 2 0 ", "  file config.json 8", "  dir fighter 0", "  file plugin.nro 6",
                    'sd fighter mod/CONFIG.json 1 8 {"x": 1}',
                    "sd Fighter Mod/fighter/mario/model/body/c08 2 0 ", "  file model.numdlb 5000", "  file x.marker 0",
                    "sd Fighter Mod/plugin.nro 1 6 NRO0 x", "sd Fighter Mod/fighter/mar 0 0 ", "sd Nope/x 0 0 ",
                    'sd Fighter Mod/config.json/ 1 8 {"x": 1}', ""])
                self.assertEqual(sorted(os.listdir(d)), ["dump"])
            finally:
                host.close()

    def test_pack_kept_between_starts(self):
        with tempfile.TemporaryDirectory() as d:
            data, image, vmap, sources = self.game(d)
            plain = self.arc_pack(data, vmap, sources, [("A", [("a.txt", b"one")])])
            code = hm.new_code()
            sealed = hm.seal(code, plain)
            kept = os.path.join(d, "hostmods_cache")
            host = hm.put(self.addr, hm.pack_id(code), sealed)
            try:
                r, _ = self.run_game(data, code, SSBU_HOST_MODS_CACHE="1", HOSTMODSTEST_SD="A/a.txt")
                self.assertEqual(r.returncode, 0, r.stderr)
                self.assertIn("sd A/a.txt 1 3 one", r.stdout)
                pid = hm.pack_id(code)
                self.assertEqual(sorted(os.listdir(kept)), [pid + ".bin", pid + ".sum"])
                with open(os.path.join(kept, pid + ".bin"), "rb") as f:
                    self.assertEqual(f.read(), sealed)   # (sealed as it came: the code opens it)
                r, _ = self.run_game(data, code, SSBU_HOST_MODS_CACHE="1", HOSTMODSTEST_SD="A/a.txt")
                self.assertEqual(r.returncode, 0, r.stderr)
                self.assertIn("not fetched again", r.stderr)
                self.assertIn("sd A/a.txt 1 3 one", r.stdout)
                # a broken kept pack (the same sum): fetched again, kept again
                with open(os.path.join(kept, pid + ".bin"), "r+b") as f:
                    f.seek(len(sealed) - 5)
                    f.write(b"XXXXX")
                r, _ = self.run_game(data, code, SSBU_HOST_MODS_CACHE="1", HOSTMODSTEST_SD="A/a.txt")
                self.assertEqual(r.returncode, 0, r.stderr)
                self.assertIn("doesn't open: fetching it again", r.stderr)
                self.assertIn("sd A/a.txt 1 3 one", r.stdout)
            finally:
                host.close()
            # the host's pack changed (another salt and content): the kept one isn't used
            plain2 = self.arc_pack(data, vmap, sources, [("A", [("a.txt", b"two")])])
            host = hm.put(self.addr, hm.pack_id(code), hm.seal(code, plain2))
            try:
                r, _ = self.run_game(data, code, SSBU_HOST_MODS_CACHE="1", HOSTMODSTEST_SD="A/a.txt")
                self.assertEqual(r.returncode, 0, r.stderr)
                self.assertNotIn("not fetched again", r.stderr)
                self.assertIn("sd A/a.txt 1 3 two", r.stdout)
            finally:
                host.close()

    def test_sealed_pack_never_saved(self):
        with tempfile.TemporaryDirectory() as d:
            data, image, vmap, sources = self.game(d)
            plain, want = self.pack(image, vmap, sources)
            code = hm.new_code()
            # a version 1 pack (no plugins) still reads
            v1 = plain[:8] + struct.pack("<I", 1) + plain[12:-8]
            host = hm.put(self.addr, hm.pack_id(code), hm.seal(code, v1))
            try:
                r, got = self.run_game(data, code, SSBU_HOST_MODS_DOWNLOAD="1")
                self.assertEqual(r.returncode, 0, r.stderr)
                self.assertEqual(got, want)
                self.assertIn("in memory only", r.stderr)
                self.assertIn("the host didn't allow downloads", r.stderr)
                self.assertEqual(sorted(os.listdir(d)), ["dump"])
            finally:
                host.close()

    def test_plugins_for_this_system(self):
        with tempfile.TemporaryDirectory() as d:
            data, image, vmap, sources = self.game(d)
            plugins = [("moveset_x", 1, b"windows-lib", b"win-image"), ("moveset_x", 2, b"linux-lib", b"linux-image")]
            plain, _ = self.pack(image, vmap, sources, plugins)
            code = hm.new_code()
            host = hm.put(self.addr, hm.pack_id(code), hm.seal(code, plain))
            try:
                r, _ = self.run_game(data, code)
                self.assertEqual(r.returncode, 0, r.stderr)
                self.assertIn("plugin moveset_x linux-lib linux-image\n", r.stdout)
                self.assertNotIn("windows-lib", r.stdout)
                self.assertIn("list smashline\nmoveset_x\n", r.stdout)
            finally:
                host.close()
            # the host's plugins built for Windows only: a Linux player's game stops, saying why
            plain, _ = self.pack(image, vmap, sources, plugins[:1])
            host = hm.put(self.addr, hm.pack_id(code), hm.seal(code, plain))
            try:
                r, _ = self.run_game(data, code)
                self.assertEqual(r.returncode, 3)
                self.assertIn("other systems", r.stderr)
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


def _windows_compiler():
    if sys.platform != "win32":
        return None
    try:
        import cc
    except Exception:   # (no toolchain found at import)
        return None
    return cc if os.path.isfile(cc.CLANG) or shutil.which(cc.CLANG) else None


@unittest.skipUnless(_windows_compiler(), "needs Windows and the game's compiler (tools/cc.py)")
class PluginFromMemory(unittest.TestCase):
    """runtime/hle_memdll.c: a plugin-shaped DLL (tools/dev/memdll_plugin.c), built as tools/skyline.py
    builds a mod's plugin, loaded from memory twice - the second copy relocated - and run."""

    def test_loads_and_runs(self):
        cc = _windows_compiler()
        with tempfile.TemporaryDirectory() as d:
            obj, dll, exe = (os.path.join(d, n) for n in ("plugin.obj", "plugin.dll", "memdlltest.exe"))
            r = subprocess.run(cc.compile_command(os.path.join(ROOT, "tools", "dev", "memdll_plugin.c"), obj, "/O1", cc.GAME_CFLAGS),
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            cc.link_library([obj], dll)
            r = subprocess.run([cc.CLANG, "/nologo", "/O1", "-w", *(["/winsysroot", cc.WINSYSROOT] if cc.WINSYSROOT else []),
                                "/I", os.path.join(ROOT, "runtime"), os.path.join(ROOT, "tools", "dev", "memdlltest.c"),
                                "/Fe" + exe, "/Fo" + os.path.join(d, "memdlltest.obj")], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            r = subprocess.run([exe, dll], capture_output=True, text=True, timeout=60)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            lines = r.stdout.splitlines()
            self.assertEqual(lines, ["copy 0 at its address: 1 two three 42 7", "copy 1 at another address: 1 two three 42 7"])


if __name__ == "__main__":
    unittest.main()
