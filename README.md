# RosterLoader 1.0 — your roster loaded at boot in NBA 2K14 (PC)

RosterLoader loads your roster when the game starts (about half a second, no boxes), hides the "2K Sports server is temporarily unavailable" box on the title page, and lets you set your own Play Now default teams. It is a [SLAM](https://github.com/Dokitooo/SLAM) mod.

**Download:** [RosterLoader_v1.0.zip](https://github.com/Dokitooo/RosterLoader/releases/download/v1.0/RosterLoader_v1.0.zip) (latest release) ·
[RosterLoader_v1.0_plain.zip](https://github.com/Dokitooo/RosterLoader/releases/download/v1.0/RosterLoader_v1.0_plain.zip) (plain `.asi`, no SLAM) ·
[all releases](https://github.com/Dokitooo/RosterLoader/releases)

## Requirements

- NBA 2K14 PC
- [SLAM](https://github.com/Dokitooo/SLAM/releases/download/v1.0.0/SLAM_v1.0.0.zip) 1.0.0 or newer

## Install

1. Install SLAM first (see its README).
2. Extract the zip into the game folder (the one with `nba2k14.exe`). It contains only:
   ```
   SLAM\RosterLoader\RosterLoader.asi
   SLAM\RosterLoader\RosterLoader.ini
   ```
3. Start the game. The mod list at the top left shows `RosterLoader 1.0`.

Set `AutoLoad=` in `RosterLoader.ini` to your `.ROS` file name from the Saves folder. The game window may be in the background while it boots (alt-tab); Enter reaches the game directly.

Not using SLAM? `RosterLoader_v1.0_plain.zip` is the same mod as a plain `.asi` for any ASI loader. Put `plain\RosterLoader.asi` where your loader reads `.asi` files and the `plain\RosterLoader\` folder (the ini) next to it.

The log is `SLAM\RosterLoader\RosterLoader.log`. To remove the mod, delete `SLAM\RosterLoader`.

## Settings (`RosterLoader.ini`)

| Setting | What it does |
| --- | --- |
| `AutoLoad` | Roster file in your Saves folder, loaded at boot. Empty = off. |
| `Delay` | Milliseconds after the menus start (0 = right away). |
| `HidePopup` | `1` = no "Loading Roster" box (its Enter is pressed for you). `0` = the game's box. |
| `Fast` | `1` = frames are not drawn while loading (fastest). `0` = black frames. |
| `Warp` | The game clock runs this many times faster while loading (`1` = off). |
| `HideServerMessage` | `1` = the title page's server box is not shown (the servers are gone). |
| `DefaultHome / DefaultAway` | Play Now default teams by the team's Graphics ID in the roster, also the number in its logo file (`logo014.iff` = 14). 76ers 0, Heat 10, Knicks 14, Spurs 25. |

## Repository layout

| Path | Contents |
| --- | --- |
| `src/` | The mod (`rosterloader.c`) |
| `slam/` | SLAM SDK header and glue (`slam_lite.c` = plain `.asi` build) |
| `build/` | Build script (clang-cl on Linux) |
| `package/` | The README and default `.ini` |

## Build

`build/build.sh` builds `bin/SLAM/RosterLoader.asi` (SLAM mod) and `bin/plain/RosterLoader.asi` (plain `.asi`, any ASI loader).
On Linux with clang-cl + lld-link (LLVM 18). `slam/slam_glue.c` needs Windows headers: set `MGW` to the
MinGW-w64 `mingw-w64-headers` folder and `GEN` to a folder with a generated `_mingw.h` (see SLAM's `source/build`).

## Verify your download

Every release lists the SHA-256 of the zip in `SHA256SUMS.txt` (also attached to the release).
Compare it with the hash of the file you downloaded:

```powershell
Get-FileHash .\RosterLoader_v1.0.zip -Algorithm SHA256
```

```bash
sha256sum -c SHA256SUMS.txt
```

## About antivirus warnings

The ASI file in this mod hooks into `nba2k14.exe` at runtime, which some antivirus heuristics
flag on unknown files. To make the release easy to check:

- The full source is in this repository; the release binary is built from it.
- The binary is not packed, compressed or obfuscated, and the zip has no password.
- SHA-256 checksums are published with each release.

If your antivirus still flags a file, you can submit it as a false positive to the vendor, or
build it yourself from the source.
