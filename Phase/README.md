# Phase: Windows bring-up

Phase now reaches **audible gameplay on Windows 11**, using keyboard click-wheel
input and a local iTunesDB/Phase Library. Its translated ARM code handles the
menus, chart decoding, difficulty selection, notes, and drawing. A private
iOpenPod-generated Poppies chart has reached Hard gameplay; pause/resume works
through the original pause menu. Timing and full-game compatibility still need
playtesting. Profiles and native save records persist between interactive launches.

The implementation is independent of iOpenPod and the firmware research lab.
Generation needs only the owner's plaintext Phase executable. Original game
files, generated translations, memory snapshots, and executables are not shipped
in this repository.

## Build and run on Windows

Requirements: UV, Python 3.12 or newer, Visual Studio 2022 C++ x64 Build Tools,
and the [SDL3 Visual C++ development SDK](https://github.com/libsdl-org/SDL/releases).
The tested SDK is 3.4.16. Extract it to `Phase/build/deps/SDL3-3.4.16`, set
`SDL3_ROOT`, or pass `--sdl-root C:/SDK/SDL3` to the build command. The build copies
`SDL3.dll` and its license beside the executable; keep the DLL there when running.
The build script locates MSVC with `vswhere`; `--vcvars PATH` can override it.
The host uses the same SDL video presenter as the other desktop games. A hidden
Windows OpenGL context implements original SDK state queries only. Media Foundation
decodes music; XAudio2 mixes it with the game's PCM sound effects. Windows N
editions need the Media Feature Pack.

From the repository root, substitute the path to your own decrypted executable:

```powershell
uv run --python 3.12 python Phase/tools/generate.py --image C:/private/Phase_3168002.plain.bin
uv run --python 3.12 python Phase/tools/build.py
uv run --python 3.12 python Phase/tools/run.py --image C:/private/Phase_3168002.plain.bin
```

To show the original startup interface, provide your own Phase asset directory
(containing `main.hdr`, `main_0.ark`, `global_soundbank.dat`, and the music files):

```powershell
uv run --python 3.12 python Phase/tools/run.py `
  --image C:/private/Phase_3168002.plain.bin `
  --assets C:/private/Phase-assets --audio --visible --frames 18000 `
  --library C:/private/test-ipod `
  --timeout 600 --quiet-sdk `
  --screenshot Phase/build/preview.ppm
```

Escape or the window's Close button exits. Without `--visible`, rendering uses
a hidden window, useful for verification. Screenshots are uncompressed PPM files.
Drag the window edges or maximize it to enlarge the game. The shared presenter
preserves the native 320×240 frame's aspect ratio and fills unused space with
black bars. **F6** cycles Sharp (default), Nearest, and Smooth; **F7** toggles
integer scaling. The title shows the current choices, which save with your local
profiles between launches. Screenshots include the complete resized client area.
See [shared video presentation](../common/video.md).
The graphical default is 300 frames; `--frames` accepts 1–36,000. This bounded
preview uses real elapsed time and a 60 Hz callback limit with `--audio`.
Without that flag it retains silent, deterministic diagnostic behavior.
`--quiet-sdk` suppresses per-service tracing during longer previews; verification
commands retain it by default.

Both generation and the supported runner require SHA-256:

```text
35834fede51f170a0b0bda8e380e1c12af0f86ca1a7952e5abfbb24ab4082258
```

Use `run.py`, rather than launching the internal executable directly: the runner
checks the complete image hash and kills an overlong process after 15 seconds
by default. `--timeout` accepts up to 600 seconds for interactive previews.
The native diagnostic also bounds function entries and guest memory. A tight
loop within one function is covered by the runner's external timeout.

Without `--assets`, the original diagnostic remains available: initialization
returns, then the first frame stops at
`unimplemented OpenGLES[52], lr=180610f8`. **Exit code 2 is an incomplete port,
not a successful first frame.** Windows shells may display it as failure.
`Phase/build/startup.json` records the executable's exit status and SDK trace.

## Profiles and scores

Visible launches through `tools/run.py` automatically save to
`%LOCALAPPDATA%/iPodClickWheelRecomps/Phase`. The local preview launcher uses this
same directory. Use `--save-dir PATH` for a separate portable save set, or
`--ephemeral` for a temporary session. Hidden runs and direct diagnostic-executable
launches remain temporary unless given `--save-dir`.

Profile creation and each native save write are committed immediately. Normal
closing first asks Phase to save through its original shutdown routine. A crash
or forced termination can lose changes the game has not submitted yet. Closing
during a song is not a verified resumable gameplay checkpoint. The host preserves
the game's serialized
bytes and stable profile IDs, including its own primary/backup score records.
No game assets, music, iTunesDB, or Phase charts are modified.

`state.bin` is a checksummed host snapshot; `state.bak` keeps the previous valid
commit. An unreadable primary is recovered from the backup, with a log message.
If neither is valid, startup stops without creating a replacement profile.
One running process may use a save directory at a time. Copy the entire directory
while Phase is closed to back up or move profiles. Rename and delete work through
Manage Users. IDs remain stable after deletion; a new player never inherits a
deleted ID. Older host snapshots load automatically and upgrade on the next write.

`tests/verify_persistence.py --image PATH --assets PATH` creates a profile in an
isolated directory and verifies that a second process loads its native save and
renders the main menu without input. Authored cross-process tests cover record
bytes, stable IDs, recovery, interrupted staging, write failures, and locking.
See [persistence evidence](analysis/persistence.md).

The menu sweep also added Brightness: the game's percentage control dims only
the preview window. It resets to full brightness on launch and does not change
the Windows monitor setting. See [menu exploration](analysis/menu-exploration.md)
for exercised paths, missing-command evidence, and remaining coverage limits.

## Keyboard controls

| Key | Click-wheel button |
| --- | --- |
| W | Menu / Back |
| A | Previous |
| S | Center / Select |
| X | Play/Pause |
| D | Next |
| Enter | Center / Select (alternate) |
| Escape | Close the Windows preview |

The touch ring uses these physical US-QWERTY key positions, clockwise from top:

```text
             O 0°
      I 315°       P 45°
  K 270°               ; 90°
      , 225°       / 135°
             . 180°
```

Hold a touch key to place the virtual finger. Hold adjacent keys together for
the midpoint: O+P is 22.5°. Overlap keys as you move around the ring to maintain
contact. Touch keys never press wheel buttons; `;` only touches the right side.
Release all touch keys to lift the finger. Losing window focus releases every
held control, and Windows key repeat does not generate extra button presses.

While contact continues, the host sends intermediate positions along the
shortest arc to the new target, at most 5.625° per game frame. Reversing direction
retargets that movement; releasing stops it immediately. The ring still has
16 ordinary resting positions, but movement is not limited to 16 jumps. Phase
consumes an 8-bit angle; this adapter does not assume that byte corresponds
directly to the physical sensor's electrode count. Opposing/symmetric chords
with no unique angle lift the virtual finger.

For name entry, rotate to a letter and press S. Rotate to the checkmark and
press S to confirm the name. At the main menu, W opens the game's exit
confirmation. Keyboard mappings preserve the original game's behavior on each
screen; rhythm timing is not yet validated.

## Verified milestones

- The ARMv5TE translator additions build with MSVC, including CLZ, BLX-register,
  saturating arithmetic, signed halfword multiplication, dual transfers, and
  absolute-pointer jump tables. Negative-offset import thunks are recognized.
- Generation recovers the image's 42 table-relative constructor entries. Stored
  and PC-relative pointer discovery supplements direct calls. A Ghidra function
  survey is optional (`--functions`), not required for the build.
- Native startup executes the original initializer and original game allocator;
  there are no replaced game routines and no prepared conversion snapshot.
- The graphical host supplies the RGB565 framebuffer, selected graphics-state
  operations, read-only asset access, and queued file completions. Phase retains
  its archive parsing, callbacks, resource decoding, layout, and drawing logic.
- A 300-frame run exits successfully at name entry. A visible Windows run was
  inspected and exited successfully using Escape after 2,290 frames.
- A repeatable keyboard sequence enters a name and reaches the main menu after
  400 frames. A second replay verifies W opens the original Exit Game dialog.
- English, an initially empty profile list, a full battery indicator, and silent
  audio bookkeeping are preview conventions. Unknown services stop explicitly.
  These adapters are not complete firmware replacements.
- The original ARM initializer and first-frame prefix, run under Unicorn with
  identical adapters, match native Windows exactly: 18 SDK requests, R0–R14,
  CPSR, and all 80 MiB + 128 KiB of mapped memory.
- With `--assets`, that comparison now covers three complete frames and 250 SDK
  calls, including header reading and archive opening. All memory and registers
  match, and the Windows screenshot matches every pixel of the ARM framebuffer.

The generator currently emits 4,844 bodies without a survey. This includes
conservatively discovered entries and overlapping code; it is not a count of
proven distinct source functions or evidence of complete runtime coverage.

## Tests

The unit tests and instruction fixtures are authored and contain no game assets.
Unicorn 2.1.4 is a test-only dependency supplied temporarily by UV.

```powershell
uv run --python 3.12 python -m unittest discover -s Phase/tests -p test_*.py
uv run --python 3.12 --with unicorn==2.1.4 python Phase/tests/verify_instructions.py
uv run --python 3.12 --with unicorn==2.1.4 python `
  Phase/tests/verify_startup.py `
  --image C:/private/Phase_3168002.plain.bin
uv run --python 3.12 --with unicorn==2.1.4 python `
  Phase/tests/verify_startup.py `
  --image C:/private/Phase_3168002.plain.bin --assets C:/private/Phase-assets
uv run --python 3.12 python Phase/tests/verify_menu.py `
  --image C:/private/Phase_3168002.plain.bin --assets C:/private/Phase-assets
```

The build generates the authored instruction fixtures. The differential check
executes 1,792 cases across 28 instruction encodings and compares registers,
flags, and touched memory. BLX and jump-table structure have separate unit tests.
The startup check requires the exact known stop, or three returned frames in
graphics mode; an arbitrary crash or timeout fails it. Its memory comparison
permits no ignored differences. The compiled host-contract fixture additionally
checks reads, seeking, EOF, missing files, completion ordering, unsafe paths,
write rejection, normalized color queries, and presentation-state preservation.
Build first so this fixture runs rather than being skipped.
It also covers wheel positions, chord midpoints, interpolation and reversal,
wraparound, releases, button identities, profile lookup, and session-save
readback. The menu replay checks rendered text masks, not just a successful
process exit; it pins the original asset hashes and ships no game pixels.

Receipts and private memory dumps are under `Phase/build/`; translations are
under `Phase/gen/`. Both are ignored. The ARM comparison establishes the first
three frames; the later name-entry/main-menu screens have native-run and visual
evidence, not a full ARM differential comparison. Neither establishes gameplay
or timing accuracy against a physical iPod.

## Prepare a local library with iOpenPod

`--library` mounts a local test directory read-only. It reads the actual
`iPod_Control/iTunes/iTunesDB`, music paths from its Track records, and
`iPod_Control/GameData_RW/1D000/Phase Data/Phase Library`. The original game
reads each indexed chart by its full 64-bit Track persistent ID.

The optional preparation tool runs in an iOpenPod checkout's UV environment.
It uses iOpenPod's current analyzer and iPodDB's iTunesDB, Phase index, and PHZ
writers, then reads the resulting database back for validation. The game itself
has no iOpenPod or Python runtime dependency. FFmpeg is needed for preparation.

Create a private JSON manifest such as:

```json
[
  {"source": "C:/private/Music/song.flac"},
  {"source": "C:/private/Music/another.flac", "offset_s": 60, "length_ms": 90000}
]
```

```powershell
uv run --project C:/path/to/iOpenPod python Phase/tools/prepare_library.py `
  --manifest C:/private/songs.json --output Phase/build/test-ipod
```

Output must be a **new local directory**, never a mounted iPod. Source music is
read only. The tool creates 44.1 kHz stereo WAV copies, real analysis charts, and
an ordinary playlist named Phase. Excerpts are labeled in the song browser.
Its optional `--cached-parts` accepts source-hash-bound research NPZ evidence;
it regenerates the chart using the current algorithm, without downloading a
separation model. The preparation receipt records audio/chart hashes and counts.

The tested private corpus contains full-length Poppies and 90-second excerpts
of 505, Clocks, The Red, and ocean eyes. No music is distributed here.
Use **Quick Spin → Songs → Poppies → Hard** after entering a name.
W opens the native pause menu; S selects Resume. X remains the iPod Play/Pause
button; Phase's gameplay pause screen uses Menu.

`tests/verify_playback.py --image PATH --assets PATH --library PATH` replays
that private corpus through Hard gameplay, pause/resume, measured playback,
rendered targets, and unchanged mounted files. `phase-startup.exe --test-audio
PATH` separately checks decoder/output progress, pause/resume, repeat, and EOF.

## Remaining limits

The iTunesDB reader supports the ordinary
uncompressed music/playlist subset, not iTunesCDB, SQLite libraries, or full
firmware collation. Music decodes into bounded memory before playback (384 MiB
maximum per track); long-track streaming is future work. Windows audio latency
has not been calibrated against a physical iPod. Full-song completion, Marathon,
all menu routes, GPU uploads, and long-session memory behavior still
need coverage. Unknown SDK services continue to stop explicitly.

See [startup evidence](analysis/startup.md) for the native entry contract and the
verified graphics/file contracts. Licensing follows [the repository split](../LICENSING.md).
See [audio and library evidence](analysis/audio-library.md) for this increment.
