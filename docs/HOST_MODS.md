# Host-only mods (prototype)

The room's host has the mods. The other players don't keep a copy: their game gets the mods sealed
through the lobby when it starts, opens them in memory, and reads the game's data through them.
Nothing of the pack is written to their disk by the runtime. The online mod check still matches,
because a player's mod set is the host's (names and hashes).

**This is a deterrent, not protection.** A player's game has to read the mods, so the invite code,
the key and the plain mods are all in that game's memory while it runs. Anyone who can read a
process's memory (a debugger, `/proc/<pid>/mem`, a memory dump) can take them. What it stops is
the easy case: copying a folder out of `mods/`.

## Use

Host (the tree you play from, with the mods applied by `tools/mods.py apply` as usual):

    python tools/hostmods.py serve --lobby LOBBY_HOST[:PORT]

This prints an invite code (or pass your own with `--code`). The pack is up on the lobby while
this runs. Ctrl+C takes it down. Give the code to the players directly, not in a room name.

Players start the game with:

    SSBU_HOST_MODS=<invite code>   SSBU_NET_RELAY=LOBBY_HOST[:PORT]

Their own `mods/` and applied mods don't count while `SSBU_HOST_MODS` is set: the game reads its
unmodded data (`romfs_vanilla.cfg`, else `romfs.cfg`) through the host's layer map.

## How it fits

| Piece | What it does |
|---|---|
| `tools/hostmods.py` | Builds the pack from the host's applied tree (the modded layer map, `overlay.bin`, the mods' names and hashes as the runtime takes them). Seals it with the code and keeps it up on the lobby. |
| `tools/hostmods_depot.py` | The lobby's side. It holds sealed packs in memory, only while their host stays connected, and hands one out by its id. It is hooked into `ssbu_online_lobby.py`'s TCP port by the `SSBUPAK1` preface. |
| `runtime/hle_hostmods.c` | Takes `SSBU_HOST_MODS`. Fetches the pack from the lobby into memory that isn't put in core dumps (Linux `MADV_DONTDUMP`). Opens it in place and checks it against this game's data. |
| `runtime/hostmods_crypto.h` | SHA-256 and ChaCha20-Poly1305 (RFC 8439), no library. |
| `runtime/hle_fs.c` | A layer map source can be in memory. With host mods, the RomFS is this game's own sources plus the overlay in memory, read through the host's map. |
| `runtime/net_relay.c` | With host mods, the online mod set is the pack's names and hashes, not this game's `applied.json`. |
| `runtime/hle_ro.c` | A modded NRO from the host's pack that isn't recompiled is not saved to `nro/` (the "harvest"). |
| `runtime/host.c` | With host mods, the plugin libraries are the pack's, not this tree's `plugin_dlls.txt`. Each one's image (`<module>.reloc.bin`) is mapped from the pack too. |
| `runtime/hle_memdll.c` | Loads a plugin DLL from memory (Windows x64): sections, relocations, imports, protections, unwind tables, entry point. The plugin never exists as a file on the player's disk. |
| `runtime/hle_skyline.c` | With host mods, the plugin list is the host's `plugins.txt` from the pack. |

### Plugins (pack version 2)

A moveset's `.nro` can't be streamed and run as-is: this port runs plugins as native code translated ahead of time.
So the pack carries what the host's game already built:

- the translated library, `build/plugins/<module>.dll`, from `tools/skyline.py cache_mod_plugins`;
- its image, `dump/modules/<module>.reloc.bin`;
- the host's `plugins.txt`.

`hostmods.py serve` takes every plugin that `dump/skyline/plugin_dlls.txt` lists and `plugins.txt` names.
`--no-plugins` leaves them out, and players then use their own.

Each plugin is tagged with the system it was built for:

| Tag | System |
|---|---|
| 1 | Windows x64 |
| 2 | Linux x64 |
| 3 | macOS arm64 |

A player loads only the plugins tagged with their own system. If the host's plugins are all for other systems, the player's game stops and says so; it doesn't run the match without them.

Requirements:

- **Same HypeSmash build.** Host and players need the same build: a plugin library is linked against the runtime's `module.h` interface.
- **Standard plugins on the player's side.** Plugins the game links in, such as Smashline, come from the player's own build. If the host's list names one the player's build lacks, it is left out.

The lobby never sees the code. It only sees the pack id, `sha256("HypeSmash host mods id\0" + code)[:10]`,
and can't open a pack. The formats are in `tools/hostmods.py`'s docstring.

## Checked

`tests/test_hostmods.py` checks these:

- Sealing and opening round trips, in chunks. A wrong code fails, and so does a changed byte, a changed head, swapped chunks, or a cut pack.
- The pack format, and building a pack from a made-up applied tree. ARCropolis-mode trees are refused.
- The Python port of the runtime's mod-tree hash, including the files it skips.
- The depot through the real `OnlineLobby.stream` handler:
  - PUT, GET, one host per code, size limits, and a non-pack being refused.
  - The pack goes away when the host disconnects.
- The C crypto opens what Python seals, with gcc `-Wall -Wextra -Werror`. Its SHA-256 and pack id match Python's.
- `tools/dev/hostmodstest.c` runs the real `hle_fs.c` and `hle_hostmods.c` with no game, against a real lobby:
  - It fetches, opens and reads the whole RomFS image through the host's map. The result is byte-for-byte what is expected, the mods list matches, and no files are left behind.
  - These cases each fail with their own message: no pack up, a pack sealed with another code, a source of another size, and another vanilla layer map.

## Not checked

- Plugins, end to end:
  - `tools/dev/memdlltest.c` passes on Windows (tests/test_hostmods.py PluginFromMemory). It builds a plugin-shaped DLL the way `tools/skyline.py` builds one and loads it from memory twice, the second copy relocated. It covers the C runtime, a bound function, and an SEH exception raised and caught inside the DLL.
  - The changed runtime files compile with the game's flags.
- No real moveset has been loaded from a pack.

- A real game. The runtime with the real game hasn't been run with `SSBU_HOST_MODS`, so a real match on host mods is unverified.
- Windows builds. `hle_hostmods.c` has its Windows paths (`VirtualAlloc`, `_stat64`, Winsock) but was only compiled on Linux, where the modified runtime files compile cleanly with the Linux build's flags.
- Matching hashes. The Python mod hash matching the C one on a real mod folder isn't checked. Non-ASCII file names may differ on Windows: the runtime hashes them as `FindFirstFileA` sees them.

## Limits

- **Plugins: Windows players only, for now.** Only Windows builds translate a mod's plugin into its own library, so a host can only send Windows plugins:
  - A Linux or macOS player of a pack with plugins gets a clear stop.
  - Linux would need a per-plugin `.so` build and a memfd + `dlopen` loader. macOS would need a `.dylib` build.
  - A plugin library with thread-local data is refused, since only the system's loader can give it a TLS slot. Plugins built by `tools/skyline.py` have none.
- **The plugin is native code in the player's memory.** That makes it harder to reuse than the `.nro`, but it can still be dumped, like the rest of the pack.
- ARCropolis-mode trees (`"arcropolis": true`) are refused.
- **Same game data.** The player's unmodded data has to match the host's: the vanilla layer map's sha256 and each source's size. Otherwise the game stops with a clear message.
- **Code patches.** The ARCropolis code-patches setting still has to match between players (it is its own mod-set entry).
- **Leaks still possible:**
  - The OS can page the pack out to the pagefile or swap.
  - GPU shader/pipeline caches can hold shaders built from modded data.
  - Mid-match mod refresh (`hle_mod_refresh.c`) still watches the player's own `mods/`.
  - The code is in the process environment, which child processes inherit and other local users may read.
- **Memory.** The whole overlay sits in RAM on every player, and in the lobby's RAM while it's up. The defaults are 1 GiB per pack, 2 GiB in all and 32 packs (`hostmods_depot.py`).
- **Anyone with the code can fetch the pack.** The code is the only secret: a player can share it, and the lobby serves every request with the right id.
