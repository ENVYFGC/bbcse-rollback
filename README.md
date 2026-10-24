# bbcse-rollback

Rollback netcode for **BlazBlue: Continuum Shift Extend** (Steam).

## Features

- [GGPO](https://github.com/pond3r/ggpo) rollback in the game's own online matches, over Steam.
- Full fight state saved every frame. Checksums compared during the match, desyncs logged.
- Spectating and replays work with rollback. Spectators get the fight from the lobby host.
- Network menu fixed.
- Room list shows the room owner's Steam name.
- D-Card kept in `bbcse-cloud` next to the game. The old one is copied from Steam's local cloud folder on first launch. Otherwise: download it from Steam's cloud page, put it in that folder.
- Steady 60 fps on Direct3D 9. Driver-forced VSync is respected.
- Optional (ini): render size, borderless window, key remap.

## Install

1. Steam > right-click BBCSE > Manage > Browse local files.
2. Put `dinput8.dll`, `bbcse-net.ini` and `bbcse-versions` next to `BBCSE.exe`. Updating: delete the old `bbcse-versions` first.
3. Both players need the mod. Spectators too.

Linux / Steam Deck: `winmm.dll` instead of `dinput8.dll`. Launch options: `WINEDLLOVERRIDES="winmm=n,b" %command%`

Uninstall: delete `dinput8.dll` (or `winmm.dll`) and `bbcse-net.ini`. Left behind: `bbcse-cloud` (D-Card), `bbcse-palettes`, `bbcse-versions`, `bbcse-im.ini` (version and palette options), log and crash files.

Opponent without the mod: the game's own netcode after 15 seconds.

## IM BBCS:EX (F2)

Training:

- F3 frame advantage: startup, hit/block advantage, invul. Follow-ups (214A~214D) get their own startup.
- F4 hitboxes.
- Palette editor.
- Position reset (F5 or Select): center, me in the corner, opponent in the corner, swap sides.

Online:

- F1 rollback panel: ping, rollback, frame advantage, frame times.
- Ping and rollback bar, bottom center. On by default.
- Palette options.

Version select:

- CSE, CS2, CS1. Characters, system rules and HUD from that version.
- Files in `bbcse-versions`.
- Files are checked at start. A version with missing or changed files is turned off: copy `bbcse-versions` from the zip again. Only the shipped `cs1` and `cs2` folders are used.
- Online: the lobby host's pick applies to the room.
- CS2/CS1: version shown top center during the fight.
- CS1 is version 1.02, CS2 is version 1.03 (2.0). Relius and Platinum work against them.

Save:

- Unlock all gallery items: marks every gallery item as bought (Unlimited characters, colors, art, movies, voices). Backup in `Save\bbsave.before-unlock.dat`.

## Palettes

Folder: `bbcse-palettes\<name>\`, made at first launch. Same names as the BBCF Improvement Mod:

Arakune, Bang, Carl, Hakumen, Hazama, Jin, Lambda, Litchi, Makoto, Mu, Noel, Platinum, Rachel, Ragna, Relius, Tager, Taokaka, Tsubaki, Valkenhayn

Old folders (`rg`, `jn`...) are moved on first launch.

- File name = palette name in `palettes.ini`.
- Formats: `.cfpl`, `.hpl`, `.act`, `.png`. Editor PNG exports go to `bbcse-palettes\exports`; move them into a character folder to use them.
- `.cfpl` from BBCF: effect files that differ from CF stay on the game's colors.
- `.hpl` / `.act`: `name` = character, `name_effect01` to `name_effect07` = effects.
- Editor saves `.cfpl`.
- Editor: edit, highlight, gradient, undo/redo, save, PNG export, effect preview.
- Use for this color: assigns the palette to a color slot, character select included. Saved in `bbcse-palettes\palettes.ini`:

```ini
[Ragna]
1="Fox"
5="Fox,Panda,Default"
```

List = random pick per match, character select shows the first. `Default` = game color. `Random` = any. `Random_Exclude_Default` = any custom.

Online (F2 > Online):

- Custom palettes are shown to the opponent.
- Show the opponent's custom palette.
- Let opponents save my palette.
- Save their palette: goes in the character folder.
- Block palette / Block player: `bbcse-palettes\blocked.ini`, unblock in the menu.

## Settings

`bbcse-net.ini`:

```ini
; BBCSE rollback settings. Delete this file to play with the game's own netcode.
[net]
; steam = rollback in online matches
mode=steam
; input delay in frames
delay=2
; 0 = the mod keeps 60 fps, 1 = VSync and the game's limiter
vsync=0

[display]
; draw size, e.g. 1920 and 1080. 0 = 1280x768
renderwidth=0
renderheight=0
; 1 = borderless, with the game in window mode at screen size
borderless=0

[keys]
; key pressed=key the game sees, e.g. apostrophe=P
```

Comments on their own line only. `mode=steam ; comment` breaks it.

Key remap under `[keys]`: `pressed=seen`. Example: `apostrophe=P`, then bind P in the game.

Key names: letters, digits, `apostrophe semicolon comma period slash backslash grave minus equals lbracket rbracket space tab capslock enter backspace lshift rshift lctrl rctrl lalt ralt up down left right insert delete home end pageup pagedown num0` to `num9`, or a DirectInput code like `0x28`.

## Bug reports

Send `bbcse-probe.log` from the game folder. After a desync or crash, also `bbcse-desync.bin` and `bbcse-crash.txt` if present.

## Build

Visual Studio 2019/2022, C++ desktop workload.

```
build.bat
```

- Output: `build\`.
- `build_winmm.bat`: Linux build.
- 32-bit only.
- `build.bat dev`: developer hotkeys (sync test, input recorder, save/load state, freeze, frame step).

## Credits

- [GGPO](https://github.com/pond3r/ggpo) (GroundStorm Studios), through the BBCF-Improvement-Mod-GGPO fork, MIT (`ggpo/LICENSE-GroundStorm.txt`, `ggpo/LICENSE.txt`). Changes marked `bbcse:`.
- [Dear ImGui](https://github.com/ocornut/imgui), MIT (`imgui/LICENSE.txt`). Win32 backend mouse scaling from the BBCF Improvement Mod, marked `bbcse:`.
- BBCF Improvement Mod, GPL-3: frame advantage, hitboxes, palette editor. Palette features from [HaiKamDesu's fork](https://github.com/HaiKamDesu/BBCF-Improvement-Mod).
- DroidSans (Google), Apache 2.0 (`LICENSE-DroidSans.txt`).
- Network menu fix: same idea as Geo's patcher, done in memory.

Not affiliated with Arc System Works.
