# bbcse-rollback

Rollback netcode for **BlazBlue: Continuum Shift Extend** on Steam.

## What it does

You get into matches the same way as always. The Network menu, lobbies and player matches are all still the game's own. Once the fight starts, the mod swaps the game's delay-based netcode for [GGPO](https://github.com/pond3r/ggpo), running over the same Steam connection the game was already using.

To make rollback work, the mod saves the whole fight every frame (characters, projectiles, effects, camera, RNG, stage, UI) and rewinds it whenever your opponent's input shows up late. Both games also compare checksums as the match goes on, so if they ever drift apart you'll see it in the log right away.

Spectating and replays still go through the game's own system, but they work with rollback now. Spectators get the fight from whoever's hosting the lobby, and that works whether the host is playing or just watching. Replays record the inputs that were actually used in the match, so they play back the fight you really had.

It also fixes a few things that were broken anyway:

- The Network menu wouldn't open. Now it does.
- The game kept losing your online profile (D-Card) and asking you to make a new one, because Steam Cloud isn't enabled for it anymore. The mod keeps `DCARD.UMS` in a `bbcse-cloud` folder next to the game instead. If your old one is still on Steam Cloud, grab it from Steam's cloud page and drop it in that folder.
- On plain Direct3D 9 the game could run at 30 fps, or bounce between 60 and 58. The mod keeps it at a steady 60 itself. If your graphics driver forces VSync on, it lets VSync do the pacing instead of fighting it.

There are a few extras too. They're all off until you turn them on in the ini:

- **Render size.** The game draws everything at 1280x768 and stretches it to fit your screen, which looks pretty rough at 1600x900 or 1920x1080. Give it your screen size and it draws at that size instead. It's the same trick the BBCF Improvement Mod uses for its viewport setting.
- **Borderless window.** Put the game in window mode and the title bar goes away and the window fills the screen, so alt-tabbing is instant.
- **Key remap.** Lets one key act as another, for keys the game won't let you bind, like `'`.

## Install

1. In Steam, right-click BBCSE > Manage > Browse local files.
2. Drop `dinput8.dll` and `bbcse-net.ini` next to `BBCSE.exe`.
3. Play online like normal. **Both players need the mod**, and anyone spectating should have it too.

To uninstall, just delete those two files. If your opponent doesn't have the mod, the match falls back to the game's normal netcode after a few seconds.

## Settings

Everything lives in `bbcse-net.ini`. Here's what it looks like out of the box:

```ini
[net]
; rollback in online matches
mode=steam
; input delay in frames
delay=2
; 0 = the mod keeps the game at a steady 60, 1 = use VSync and the game's own limiter
vsync=0

[display]
; these only work with vsync=0
; the size the game draws at, e.g. 1920 and 1080. 0 = the game's own 1280x768
renderwidth=0
renderheight=0
; 1 = no title bar, window fills the screen (set the game to window mode at your screen size)
borderless=0

[keys]
```

Keep comments on their own line. Windows doesn't strip comments after a value, so `mode=steam ; comment` gets read as `steam ; comment` and rollback won't turn on.

To remap a key, add a line under `[keys]` in the form `key you press=key the game sees`. For example, `apostrophe=P` makes `'` act as P. Bind P in the game's key config and `'` will do it.

Key names: letters, digits, `apostrophe semicolon comma period slash backslash grave minus equals lbracket rbracket space tab capslock enter backspace lshift rshift lctrl rctrl lalt ralt up down left right insert delete home end pageup pagedown num0`–`num9`. You can also use a DirectInput key code like `0x28`.

## Reporting bugs

Hit **F1** during an online fight for a small panel with ping, rollback depth, frame advantage and frame times. The mod also writes everything to `bbcse-probe.log` in the game folder. That's the file to send with a bug report. Grab it before you restart the game, because restarting replaces it.

## Building it yourself

You'll need Visual Studio 2019 or 2022 with the C++ desktop workload. Then run:

```
build.bat
```

The DLL ends up in `build\`. BBCSE is 32-bit, so the DLL has to be too; the script handles that. `build.bat dev` also adds some developer hotkeys (F2–F11: sync test, input recorder, save/load state, freeze, frame step) that normal builds leave out.

## Credits

- [GGPO](https://github.com/pond3r/ggpo), MIT license (`ggpo/LICENSE.txt`). The copy here has a few small changes, marked with `bbcse:`.
- [Dear ImGui](https://github.com/ocornut/imgui), MIT license (`imgui/LICENSE.txt`).
- The Network menu fix uses the same idea as Geo's patcher, just done in memory instead of patching the exe.

Not affiliated with Arc System Works. No game files are included, so you'll need your own copy of BBCSE on Steam.
