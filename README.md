# bbcse-rollback

Rollback netcode for **BlazBlue: Continuum Shift Extend** on Steam.

## What it does

You still play online the normal way: Network menu, lobbies, player matches, all of it is the game's own. Once a fight starts, the mod takes over the input exchange and runs it through [GGPO](https://github.com/pond3r/ggpo) over the same Steam connection the game already uses. Spectating and replays are left to the game.

To make rollback possible, the mod saves and restores the battle state every frame (characters, objects, effects, camera, RNG, stage and UI bits). Both sides also compare a state hash during the match, so if the games ever split it shows up in the log right away.

Other things it fixes along the way:

- the Network menu not opening anymore.
- your online profile (D-Card) getting lost and the game asking you to make a new one at every online login. Steam Cloud isn't enabled for the game anymore, so the mod keeps `DCARD.UMS` in a `bbcse-cloud` folder next to the game instead. If you have your old one backed up on Steam Cloud, download it from Steam's cloud page and drop it in that folder
- the uneven frame rate on plain Direct3D 9 (30 fps or bouncing between 60 and 58). The mod paces the game at a steady 60 itself, and if your driver forces VSync it lets VSync handle it instead of fighting it

## Install

1. Steam > right click BBCSE > Manage > Browse local files
2. Drop `dinput8.dll` and `bbcse-net.ini` next to `BBCSE.exe`
3. Play online like you normally would. **Both players need the mod.**

To uninstall, just delete those two files. If your opponent doesn't have the mod, the match falls back to the game's regular netcode after a few seconds.

## Settings

`bbcse-net.ini`:

```ini
[net]
mode=steam   ; rollback in online matches
delay=2      ; input delay in frames
;vsync=1     ; uncomment to keep VSync and the game's own frame limiter
```

Press **F1** during an online fight for a small panel with ping, rollback depth, frame advantage and frame times. Everything also gets logged to `bbcse-probe.log` in the game folder, which is the file to send along with a bug report.

## Building it yourself

You need Visual Studio 2019 or 2022 with the C++ desktop workload. Then run:

```
build.bat
```

The DLL shows up in `build\`. It has to be a 32-bit build since BBCSE is 32-bit, and the script takes care of that. `build.bat dev` adds some developer hotkeys (F2–F11: sync test, input recorder, save/load state, freeze, frame step), which are off in normal builds.

## Credits

- [GGPO](https://github.com/pond3r/ggpo), MIT license (`ggpo/LICENSE.txt`). The copy here has a few small changes marked with `bbcse:`
- [Dear ImGui](https://github.com/ocornut/imgui), MIT license (`imgui/LICENSE.txt`)
- the Network menu fix uses the same idea as Geo's patcher, just done in memory instead of patching the exe

Not affiliated with Arc System Works. No game files are included, so you need your own copy of BBCSE on Steam.
