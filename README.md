# PD2 DPS Meter

A lightweight DPS (damage per second) meter for Project Diablo 2 (Diablo II 1.13c), e.g. for
testing builds on target dummies in PlugY offline games.

* Real-time DPS: last 1 s, 5 s, 20 s and the session average
* Session time, total damage, hits (+ hits/s), last/max hit, kills and time-to-kill
* Counts only damage dealt to monsters (no own life loss, no double counting)
* Three display modes: **overlay** on the game, **window** (for OBS), **hidden**
* Every hit is logged to CSV; `tools/analyze_dps.py` turns the logs into an HTML report
* Optional Python front end (`python/pd2dps_meter.py`): injector + separate meter window

---

## Quick start

1. Put `PD2Injector.exe`, `PD2DpsHook.dll` and `logo.png` (all three are in `bin\Release` after building) in one folder.
2. Start the game, then `PD2Injector.exe` (or start the injector first - it waits up to 2 minutes for `Game.exe`).
3. The meter appears on the game. **Right-click** it for all options.

| Action | How |
|---|---|
| Move | drag the logo (overlay) or anywhere (window mode) |
| Reset statistics | RESET button, `Ctrl+Alt+R`, or menu |
| Switch overlay / window / hidden | `Ctrl+Alt+M` or menu |
| Colours | menu -> *Colours* -> background (window mode), title/label colour, value colour, text shadow |
| Settings | menu -> *Edit settings* (`PD2DpsMeter.ini` next to the DLL, created on first start) |
| Logs | menu -> *Open log folder* (`logs\` next to the DLL) |

Everything you change in the meter (mode, position, colours, always on top, hide while
inactive) is saved to `PD2DpsMeter.ini` immediately, so the meter looks the same on the next start.

## Display modes and OBS

* **overlay** - transparent, glued to the game picture. Follows the game window when it is
  moved (windowed mode), hides when the game is minimised or another program is in front
  (`HideWhenGameInactive`). Works in fullscreen and windowed mode.
* **window** - a normal window titled *PD2 DPS Meter* with a taskbar entry and a solid
  background colour (`[Look] Background`, default green). For OBS:
  1. *Sources -> + -> Window Capture -> [Game.exe]: PD2 DPS Meter*
  2. Right-click the source -> *Filters -> + Color Key* -> custom colour = the Background colour.
  (Capture method "Windows 10 (1903 and up)" also captures it while other windows cover it.)
* **hidden** - no window; logging, hotkeys and the shared memory (Python meter) keep working.

The old overlay could not be captured in windowed games because it was a hidden tool window
at fixed screen coordinates; window mode solves this.

## Logo

The default is `logo.png` next to the DLL (source: `PD2DpsHook/logo.png`, copied by the build).
To use your own logo, replace that file or point `[Look] Logo=` to another BMP, PNG, JPG or GIF
(relative to the DLL folder or an absolute path). It is scaled to `LogoSize` x `LogoSize`
pixels (default 50). Leave it empty for no logo. In
overlay mode the colour `1,0,1` is fully transparent; PNG transparency works best in window
mode (semi-transparent edges blend with the background colour).

## What is counted - `[Filter]`

The DLL hooks the life update in `D2Common.dll` and sees every life change of every unit.

| Setting | Default | Meaning |
|---|---|---|
| `MonstersOnly` | 1 | only units of type monster (your own life loss is ignored) |
| `ServerUnitsOnly` | 1 | only the unit whose field `0xD0` is 11. In single player every unit exists twice (server + client copy); this keeps each hit counted once. If nothing is counted at all, set 0. |
| `MinLossRaw` | 0 | ignore losses up to this many 1/256 life points. 0 also counts small poison/burn ticks (the old version used 256 = ignored everything up to 1 life) |
| `ClassIds` | empty | only these monster ids (MonStats hcIdx), e.g. `1285-1292` = the target dummies |

Damage from minions, mercenaries or other players also lowers monster life and is counted.

## Logs and report

* `logs\hits_<date>_<time>.csv` - one line per hit: time, session, seconds since session start,
  damage, life before/after, kill flag, unit type, monster id, unit id
* `logs\sessions.csv` - one summary line per session (written on reset and at game exit)
* `logs\PD2DpsMeter.log` - start-up / diagnostics

```
python tools\analyze_dps.py                       # all logs in bin\Release\logs
python tools\analyze_dps.py <meter folder>\logs --gap 5 --monsters dummy_monsters.json
```
writes `dps_report.html` (overview table, best values, DPS-over-time and hit-distribution
charts per session, kills/TTK, all raw data) and `dps_report_per_second.csv`.

## Python front end

```
python python\pd2dps_meter.py --inject            # inject the DLL and open the meter window
python python\pd2dps_meter.py --bg #00ff00 --topmost
```
The hook itself has to run inside the game, so the DLL is still required. The Python window
is a separate program (*[python.exe]: PD2 DPS Meter (Python)* in OBS); set `Mode=hidden` in the
ini if you only want this window. It reads the live values from the shared memory block
`Local\PD2DpsMeter_<pid>` (layout: `PD2DpsHook/SharedState.h`).

Right-click the Python window to pick the background, title/label and value colours or to
toggle *Always on top*. These settings and the window position are saved in
`python\pd2dps_meter.json` and restored on the next start; command line options such as
`--bg`/`--fg`/`--title-color` override them and are saved too.

### Python requirements

Python **3.13** for Windows from python.org (it includes `tkinter`). Only the standard library
is used - `requirements.txt` lists no packages, so `pip install -r requirements.txt` has
nothing to install.

## Building

Visual Studio 2026 (v145 toolset) with *Desktop development with C++*. Open `PD2DpsHook.slnx`,
select **Release | x86** and build - output goes to `bin\Release\`. Only 32 bit is supported
(the game is 32 bit, the hook uses x86 inline assembly). The C runtime is linked statically,
so no Visual C++ redistributable is needed.

Command line: `MSBuild PD2DpsHook.slnx -p:Configuration=Release -p:Platform=x86`

### Testing without the game

`tests\fake_game` contains a fake `D2COMMON.dll` with the exact hooked instruction sequence
and a fake `Game.exe` that simulates hits (monster, player and client-copy hits, kills):
```
tests\fake_game\build.bat
cd tests\fake_game && fake_game.exe 20                # loads the DLL itself
fake_game.exe 20 --no-inject                           # then: PD2Injector.exe --process fake_game.exe
```
It prints the expected totals; compare them with the meter / `logs\sessions.csv`.

## Source overview

| File | Purpose |
|---|---|
| `PD2DpsHook/Hook.cpp` | pattern scan, 5 byte jump patch, naked stub, filters, event ring buffer |
| `PD2DpsHook/Stats.cpp` | DPS windows, sessions, kills/TTK, CSV logs, shared memory |
| `PD2DpsHook/Overlay.cpp` | meter window (overlay/window/hidden), menu, hotkeys, logo, DPI |
| `PD2DpsHook/Config.cpp` | `PD2DpsMeter.ini` defaults, parsing, saving positions |
| `PD2Injector/PD2Injector.cpp` | injector (DLL next to the exe, waits for the game, no double injection) |
| `python/pd2dps_meter.py` | Python injector + meter window |
| `tools/analyze_dps.py` | log analyser / HTML report |

## Disclaimer

Intended for personal, offline gameplay analysis (e.g. PlugY single player). Do not inject
it into online games. Use at your own risk.

## Social Media

Find us @ https://discord.gg/zTwRPYhc
