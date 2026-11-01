# Windows game launcher

Double-click **Launch Games.cmd** in the repository root. It uses UV to run
Python 3.12 and opens the launcher without leaving a terminal window behind.
UV must be installed; no additional Python packages are required.

Select a game and click **Play**, or double-click its name. Options save
automatically when edited, when switching games, and on exit. Closing the
launcher leaves a running game open. Each game can be launched once at a time
from that launcher window. Exit failures appear in its status line; **Open last
log** opens the selected game's most recent launch log.

## Set up a game

The launcher lists Phase, Mini Golf, Cubis 2, Texas Hold'em, Lost, The Sims
Bowling, and Vortex. It launches existing builds; it does not build or download
games. **Game guide** opens the selected title's build and setup documentation.

- **Phase:** supply its decrypted game image and asset folder. The music library
  field accepts the local iPod root containing iTunesDB and analyzed Phase data.
  A blank save folder uses your normal local profiles and scores. Audio is on
  by default. The launcher uses `Phase/tools/run.py`, preserving its image hash
  check and save behavior, and the executable at `Phase/build/phase-startup.exe`.
  **Render scale** accepts 1–4: blank/1 uses original drawing, while 4 enables
  experimental 1280×960 drawing. Resize the window to see the extra resolution.
- **Other games:** the launcher detects desktop executables in common local
  build folders. Use **Browse** to select an executable from another location.
  Leave game paths blank to use the game's installed files and first-run
  installer, or supply a game files folder directly. An optional game image
  also requires its game files folder. Do not choose a headless executable.

**Launch options** offers Phase's audio/logging/temporary-save switches, or the
other games' frame rate and internal render scale overrides. Blank values keep
the game's settings. Video filters remain available in-game with F6.

## Additional parameters

**Advanced** provides optional frame limits, Phase time limits and screenshots,
and input scripts. All limits are blank by default: ordinary play continues
until you close the game.

The additional-arguments box accepts any other parameter supported by that game,
one argument per line. Arguments are passed directly to the process, without a
shell. Do not add surrounding quotes, even when paths contain spaces.

For example, Lost accepts:

```text
--time=12:00
--cheats=unlock-chapters
```

For a flag with a separate value, place each on its own line:

```text
--input-script
C:/My Game Files/input.txt
```

Additional arguments are appended after the form's options. Avoid specifying the
same option in both places. Relative paths resolve from the repository root.

## Local files and checks

Settings and per-launch logs live under
`%LOCALAPPDATA%/iPodClickWheelRecomps/Launcher`. Settings use atomic replacement;
an unreadable or unsupported settings file is reported and left intact.
Personal game paths are not stored in the repository. `launcher.log` records
startup errors from the detached UI.

Run only the focused launcher checks:

```powershell
uv run --python 3.12 python -m unittest discover `
  -s common/tests -p test_launcher.py -v
```

They cover commands for all seven titles, paths containing spaces, optional
parameters, absent play limits, validation, settings persistence, Tk forms,
and a real authored child process that exits with an error. They do not require
original game files or launch the full game test suites.
