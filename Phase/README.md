# Phase: Windows bring-up

Phase is **not playable yet**. This target runs the original build-3168002
initialization and the beginning of its first frame in native Windows C++.
It stops deliberately at the first unsupported graphics service. There is no
window, renderer binding, audio playback, or input delivery yet.

The implementation is independent of iOpenPod and the firmware research lab.
Generation needs only the owner's plaintext Phase executable. Original game
files, generated translations, memory snapshots, and executables are not shipped
in this repository.

## Build and run on Windows

Requirements: UV, Python 3.12 or newer, and Visual Studio 2022 C++ x64 Build Tools.
The build script locates MSVC with `vswhere`; `--vcvars PATH` can override it.
This initial diagnostic uses only the standard library and the existing shared
runtime headers. No SDL dependency is needed until the graphical host is added.

From the repository root, substitute the path to your own decrypted executable:

```powershell
uv run --python 3.12 python Phase/tools/generate.py --image C:/private/Phase_3168002.plain.bin
uv run --python 3.12 python Phase/tools/build.py
uv run --python 3.12 python Phase/tools/run.py --image C:/private/Phase_3168002.plain.bin
```

Both generation and the supported runner require SHA-256:

```text
35834fede51f170a0b0bda8e380e1c12af0f86ca1a7952e5abfbb24ab4082258
```

Use `run.py`, rather than launching the internal executable directly: the runner
checks the complete image hash and kills an overlong process after 15 seconds.
The native diagnostic also bounds function entries and guest memory. A tight
loop within one function is covered by the runner's external timeout.

Expected current result: initialization returns, then the first frame stops at
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
- The host supplies arena backing allocations and a deterministic diagnostic
  clock. Every other SDK request stops with its framework, ordinal, caller, and
  arguments. These adapters are not validated as complete firmware replacements.
- The original ARM initializer and first-frame prefix, run under Unicorn with
  identical adapters, match native Windows exactly: 18 SDK requests, R0–R14,
  CPSR, and all 80 MiB + 128 KiB of mapped memory.

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
```

The build generates the authored instruction fixtures. The differential check
executes 1,792 cases across 28 instruction encodings and compares registers,
flags, and touched memory. BLX and jump-table structure have separate unit tests.
The startup check requires the exact known graphics stop; an arbitrary crash or
timeout fails the test. Its memory comparison permits no ignored differences.

Receipts and private memory dumps are under `Phase/build/`; translations are
under `Phase/gen/`. Both are ignored. These checks establish the startup prefix,
not a title screen, complete game compatibility, or rhythm/audio accuracy.

## Next increment

1. Identify Phase's graphics object-allocation and drawing ordinals from their
   callers and the matching firmware interface. Connect them to the shared
   renderer; another game's ordinal numbers are not interchangeable.
2. Add local asset lookup and asynchronous file completions as the loading path
   reaches them. Reach a repeatable original title-screen image before adding
   the interactive Windows window and menu input.
3. Follow with one bundled song, synchronized audio, input judgment, and results;
   only then use user-song charts for gameplay-quality comparisons.

The short-lived host arena does not reclaim backing blocks. The diagnostic
clock is not connected to real time or audio. The frame pump has a three-frame
limit and currently stops before its first frame returns. Those limits are
intentional for this bring-up milestone, not suitable for a playable session.

See [startup evidence](analysis/startup.md) for the native entry contract and the
specific boundary to implement next. Licensing follows [the repository split](../LICENSING.md).
