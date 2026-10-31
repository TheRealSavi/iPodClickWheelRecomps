# Phase: Windows bring-up

Phase now accepts keyboard click-wheel input, completes its original name-entry
screen, and reaches the **main menu** in a native Windows window. Its translated
ARM code handles the menus and drawing. This is **not a playable rhythm-game
port yet**: audio playback and a host music library are not connected. Profiles
and save records last only for the current process; they are not saved to disk.

The implementation is independent of iOpenPod and the firmware research lab.
Generation needs only the owner's plaintext Phase executable. Original game
files, generated translations, memory snapshots, and executables are not shipped
in this repository.

## Build and run on Windows

Requirements: UV, Python 3.12 or newer, and Visual Studio 2022 C++ x64 Build Tools.
The build script locates MSVC with `vswhere`; `--vcvars PATH` can override it.
The host uses Windows' built-in OpenGL compatibility renderer and system
libraries (`opengl32`, `gdi32`, `user32`). No additional runtime dependency is
required. This preserves the game's own software rasterizer while providing
the graphics state it queries and a window for its framebuffer.

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
  --assets C:/private/Phase-assets --visible --frames 18000 `
  --timeout 600 --quiet-sdk `
  --screenshot Phase/build/preview.ppm
```

Escape or the window's Close button exits. Without `--visible`, rendering uses
a hidden window, useful for verification. Screenshots are uncompressed PPM files.
The graphical default is 300 frames; `--frames` accepts 1–36,000. This bounded
preview advances a diagnostic clock and is not paced to real-time audio.
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

## Next increment

1. Add isolated host profile and save persistence;
   never save into the original asset directory.
2. Implement real audio decoding and playback with a measured playback clock.
   Replace the silent stream/voice bookkeeping and validate pause/repeat behavior.
3. Follow with one bundled song, synchronized audio, input judgment, and results;
   only then use user-song charts for gameplay-quality comparisons.

The short-lived host arena does not reclaim backing blocks. The diagnostic
clock is not connected to real time or audio. The host does not yet implement
GPU buffer uploads, all texture services, teardown, or validated gameplay timing.
These limits are intentional for this milestone, not suitable for gameplay.

See [startup evidence](analysis/startup.md) for the native entry contract and the
verified graphics/file contracts. Licensing follows [the repository split](../LICENSING.md).
