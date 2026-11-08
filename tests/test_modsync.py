"""tools/modsync.py: one PC shares its mods over HTTP, another matches them, and undo puts it back.
Two made-up project folders on this machine, the share on a free localhost port."""
import json
import os
import shutil
import sys
import tempfile
import threading
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import mods  # noqa: E402
import modsync  # noqa: E402


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data if isinstance(data, bytes) else data.encode())


def snapshot(tree):
    """Every file under mods/ (sync's own folder left out) -> bytes, and mods.json."""
    out = {}
    for path, full in modsync.folder_files(tree.mods):
        out[path] = Path(full).read_bytes()
    return out, tree.load()


class Sync(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.a = mods.Tree(os.path.join(self.tmp, "a"))
        self.b = mods.Tree(os.path.join(self.tmp, "b"))
        # A: three mods, one off
        write(os.path.join(self.a.mods, "Skin", "fighter/mario/model/body/c00/model.numdlb"), "skin v2")
        write(os.path.join(self.a.mods, "Skin", "ui/replace/chara/chara_0/chara_0_mario_00.bntx"), "portrait")
        write(os.path.join(self.a.mods, "Moveset", "fighter/dabi/plugin.nro"), b"\x00NRO" * 1000)
        write(os.path.join(self.a.mods, "Moveset", "config.json"), "{}")
        write(os.path.join(self.a.mods, "Stage", "stage/battlefield/normal/model/x.numdlb"), "stage")
        self.a.save({"order": ["Moveset", "Skin", "Stage"],
                     "mods": {"Moveset": {"enabled": True}, "Skin": {"enabled": True, "gamebanana": 123},
                              "Stage": {"enabled": False}}})
        # B: an older Skin with an extra file, and a mod of its own that is on
        write(os.path.join(self.b.mods, "Skin", "fighter/mario/model/body/c00/model.numdlb"), "skin v1")
        write(os.path.join(self.b.mods, "Skin", "fighter/mario/model/body/c00/old.nutexb"), "gone")
        write(os.path.join(self.b.mods, "Mine", "fighter/link/model/body/c00/model.numdlb"), "mine")
        self.b.save({"order": ["Mine", "Skin"], "mods": {"Mine": {"enabled": True}, "Skin": {"enabled": False}}})

        self.server = None
        ready = threading.Event()

        def started(server, share):
            self.server = server
            ready.set()
        self.thread = threading.Thread(target=modsync.serve, kwargs=dict(
            tree=self.a, port=0, key="abcd2345", bind="127.0.0.1", ready=started), daemon=True)
        self.thread.start()
        self.assertTrue(ready.wait(10))
        self.address = f"127.0.0.1:{self.server.server_address[1]}"

    def tearDown(self):
        if self.server:
            self.server.shutdown()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_plan_names_what_changes(self):
        remote = modsync.Remote(self.address, "ABCD2345").manifest()
        self.assertEqual(remote["order"], ["Moveset", "Skin"])   # Stage is off: not shared
        p = modsync.plan(self.b, remote)
        self.assertEqual(p["add"], ["Moveset"])
        self.assertEqual(p["update"]["Skin"], {"get": ["fighter/mario/model/body/c00/model.numdlb",
                                                       "ui/replace/chara/chara_0/chara_0_mario_00.bntx"],
                                               "drop": ["fighter/mario/model/body/c00/old.nutexb"]})
        self.assertEqual(p["turn_off"], ["Mine"])

    def test_pull_matches_and_undo_restores(self):
        before = snapshot(self.b)
        modsync.pull(self.b, self.address, "abcd2345", apply=False)
        files, state = snapshot(self.b)
        theirs, _ = snapshot(self.a)
        for path in ("Skin/fighter/mario/model/body/c00/model.numdlb", "Moveset/fighter/dabi/plugin.nro",
                     "Moveset/config.json", "Skin/ui/replace/chara/chara_0/chara_0_mario_00.bntx"):
            self.assertEqual(files[path], theirs[path], path)
        self.assertNotIn("Skin/fighter/mario/model/body/c00/old.nutexb", files)
        self.assertNotIn("Stage/stage/battlefield/normal/model/x.numdlb", files)
        self.assertEqual(files["Mine/fighter/link/model/body/c00/model.numdlb"], b"mine")
        self.assertEqual(state["order"], ["Moveset", "Skin", "Mine"])
        self.assertTrue(state["mods"]["Skin"]["enabled"])
        self.assertEqual(state["mods"]["Skin"]["gamebanana"], 123)
        self.assertFalse(state["mods"]["Mine"]["enabled"])
        self.assertFalse(os.path.exists(os.path.join(modsync.sync_root(self.b), "incoming")))

        again = modsync.plan(self.b, modsync.Remote(self.address, "abcd2345").manifest())
        self.assertEqual((again["add"], again["update"], again["state_changes"]), ([], {}, False))

        modsync.undo(self.b, apply=False)
        self.assertEqual(snapshot(self.b), before)
        with self.assertRaises(mods.Error):
            modsync.undo(self.b, apply=False)

    def test_wrong_key_and_unlisted_files_are_refused(self):
        with self.assertRaisesRegex(mods.Error, "refused the key"):
            modsync.Remote(self.address, "WRONGKEY").manifest()
        remote = modsync.Remote(self.address, "abcd2345")
        remote.manifest()
        for mod, path in (("Stage", "stage/battlefield/normal/model/x.numdlb"),   # off, so not shared
                          ("Skin", "../Moveset/config.json"),
                          ("..", "mods.json")):
            with self.assertRaisesRegex(mods.Error, "404"):
                remote.open("/file", mod=mod, path=path)

    def test_a_damaged_download_changes_nothing(self):
        before = snapshot(self.b)
        real = modsync.Remote.fetch

        def flaky(self_, mod, path, size, sha, dest):
            if mod == "Skin":
                return real(self_, mod, path, size, "0" * 64, dest)
            return real(self_, mod, path, size, sha, dest)
        modsync.Remote.fetch = flaky
        try:
            with self.assertRaisesRegex(mods.Error, "damaged"):
                modsync.pull(self.b, self.address, "abcd2345", apply=False)
        finally:
            modsync.Remote.fetch = real
        self.assertEqual(snapshot(self.b), before)


class Names(unittest.TestCase):
    def test_names_and_paths_from_the_other_pc(self):
        for good in ("Skin", "(Moveset) Dabi", "a.b"):
            self.assertTrue(modsync.safe_name(good), good)
        for bad in ("", ".", "..", ".packs", "a/b", "a\\b", "C:", " x"):
            self.assertFalse(modsync.safe_name(bad), bad)
        for good in ("fighter/mario/x.nutexb", "stream;/sound/bgm/x.nus3audio", "config.json"):
            self.assertTrue(modsync.safe_path(good), good)
        for bad in ("", "/etc/passwd", "../x", "a/../b", "a//b", "C:/x", "a\\b", ".git/config", "a/.x"):
            self.assertFalse(modsync.safe_path(bad), bad)


if __name__ == "__main__":
    unittest.main()
