# Phase: Windows bring-up

Phase now displays its original logo animation and **Enter Your Name** screen
in a native Windows window. Its own translated ARM code loads the archive and
draws the interface. This is a graphical bring-up milestone, **not a playable
port yet**: menu/game input, profile saving, and audio playback are not connected.

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
  --assets C:/private/Phase-assets --visible --frames 1800 --timeout 60 `
  --screenshot Phase/build/name-entry.ppm
```

Escape or the window's Close button exits. The name-entry screen currently waits
for input that the host does not yet deliver. Without `--visible`, rendering uses
a hidden window, useful for verification. Screenshots are uncompressed PPM files.
The graphical default is 300 frames; `--frames` accepts 1–10,000. This bounded
preview advances a diagnostic clock and is not paced to real-time audio.

Both generation and the supported runner require SHA-256:

```text
35834fede51f170a0b0bda8e380e1c12af0f86ca1a7952e5abfbb24ab4082258
```

Use `run.py`, rather than launching the internal executable directly: the runner
checks the complete image hash and kills an overlong process after 15 seconds.
The native diagnostic also bounds function entries and guest memory. A tight
loop within one function is covered by the runner's external timeout.

Without `--assets`, the original diagnostic remains available: initialization
returns, then the first frame stops at
`unimplemented OpenGLES[52], lr=180610f8`. **Exit code 2 is an incomplete port,
not a successful first frame.** Windows shells may display it as failure.
`Phase/build/startup.json` records the executable's exit status and SDK trace.

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
- English, an empty profile list, a full battery indicator, and silent audio
  bookkeeping are preview conventions. Unknown services still stop explicitly.
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

Receipts and private memory dumps are under `Phase/build/`; translations are
under `Phase/gen/`. Both are ignored. The ARM comparison establishes the first
three frames; the later name-entry screen has native-run and visual evidence,
not a full ARM differential comparison. Neither establishes gameplay or timing
accuracy against a physical iPod.

## Next increment

1. Map keyboard/click-wheel events into Phase's original event list, complete
   name entry, and reach the main menu. Add isolated host profile persistence;
   never save into the original asset directory.
2. Implement real audio decoding and playback with a measured playback clock.
   Replace the silent stream/voice bookkeeping and validate pause/repeat behavior.
3. Follow with one bundled song, synchronized audio, input judgment, and results;
   only then use user-song charts for gameplay-quality comparisons.

The short-lived host arena does not reclaim backing blocks. The diagnostic
clock is not connected to real time or audio. The host does not yet implement
GPU buffer uploads, all texture services, teardown, or gameplay input. These
limits are intentional for this milestone, not suitable for a playable session.

See [startup evidence](analysis/startup.md) for the native entry contract and the
verified graphics/file contracts. Licensing follows [the repository split](../LICENSING.md).
