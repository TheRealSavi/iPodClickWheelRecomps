# Phase startup evidence

Recorded 2026-10-31 for build 3168002, SHA-256
`35834fede51f170a0b0bda8e380e1c12af0f86ca1a7952e5abfbb24ab4082258`.
This combines static analysis, native/emulated comparison, and observation of
the Windows logo/name-entry screens. It is not physical-iPod timing evidence or
a playable Windows game.

## Entry contract

The five header slots at image offset `0x14` are:

| Slot | Role | Address |
| --- | --- | --- |
| 0 | Initialization | `0x1807EB10` |
| 1 | Teardown, not a second initializer | `0x1807EAB0` |
| 2–3 | Unused | Zero |
| 4 | Frame callback | `0x1807EB70` |

The frame callback receives a context in R0 and an answer block in R1. It reads
the context reason byte at `+0`, event-list pointer at `+0x30`, and another
RGB565 framebuffer pointer at `+0x38`. In the reason-zero path it creates the configuration
loader and sets the answer byte to 1; at its end it copies the answer back into
the context. The current diagnostic uses a zeroed context, no input events, and
separate context/answer blocks at `0x19000000` and `0x19000100`.

Initialization invokes the constructor walker at `0x18002844`. Its literals at
`0x1800287C` and `0x18002880` give table bounds relative to `0x18002854` and
`0x1800285C`. Each entry is an offset relative to its own slot, with the low two
bits removed after addition. Recovering these 42 entries fixes missing indirect
targets without importing a Ghidra project or skipping original constructors.

## Original no-assets probe

| Service | Observed contract | Diagnostic implementation |
| --- | --- | --- |
| `MemoryAlloc[0]` | The arena initializer calls from `0x180115C8`, passes `0x7FF80` bytes, and retains the returned address | 16 bounded, aligned backing allocations; game suballocation code unchanged |
| `miscTBD[12]` | `0x180055D0` passes an output word; `0x180055E4` compares elapsed readings with 1,000,000 | Deterministic microsecond clock, advanced per frame and one unit per read; not an audio clock |
| `OpenGLES[52]` | `0x180610A0` calls from `0x180610F8` with count 1 and output pointer `0x180F6134` | Unsupported: exact stop recorded |

The graphics initialization loops over 64 texture names using ordinal 52, then
64 buffer names using ordinal 51. The no-assets probe deliberately stops here;
the graphical adapter described below now supplies names for both collections.

Ordinal identities differ across titles: Vortex's `miscTBD[12]` is a wall-clock
structure, while Phase's caller supplies only one word. Copying Vortex's binding
would corrupt Phase's stack. The Phase-specific output-word clock is supported
by its consumers; the diagnostic return-register policy is a host convention.

## Initial translation acceptance boundary

The original no-assets probe completes initialization after 2,150 translated function
entries and stops at the first graphics request after 2,156 entries. The 18 SDK
requests are 16 arena allocations, one clock read, and one graphics request.

`tests/verify_startup.py` executes the original ARM bytes through the same point
with only the two documented host services intercepted. It checks the complete
SDK trace, R0–R14 and CPSR, and every byte in RAM, image/BSS, heap, and IRAM.
The host and oracle use the same outer-call return sentinel; no stack bytes are
excluded. Local hashes and results live in `build/startup-verification.json`.

This comparison validates translation along the observed path. It does not
validate the adapters against physical firmware or cover later initialization,
rendering, audio, input, teardown, or all indirect calls.

## Graphics and asset milestone

The graphical run opens `main.hdr`, `main_0.ark`, and `global_soundbank.dat` from
the owner's supplied directory. A 300-frame run reaches **Enter Your Name**
through the original startup sequence. A visible Windows run was inspected and
closed using Escape after 2,290 returned frames. No game functions are replaced,
no preloaded runtime snapshot is used, and no menu is recreated in host code.

### Software framebuffer and graphics state

`0x18012E4C` retains the framebuffer pointer passed at context `+0x38`.
Supplying zero caused an unmapped halfword write; supplying a 320 × 240 RGB565
buffer at `0x11010000` let the game's original software drawing code render the
logo. The buffer occupies reserved host RAM, separate from the return sentinel,
stack, image, and game allocator. The title path uses that same drawing route.

The host uses a Win32/WGL compatibility context rather than introducing another
graphics dependency. This is an initial Windows-specific adapter, not a new
shared renderer abstraction. System OpenGL holds the queried graphics state;
the translated game rasterizes its own assets into the RGB565 buffer. On swap,
the host converts that buffer to RGB8 and displays it, saving and restoring
graphics state so presentation cannot alter subsequent game rendering.

| Phase ordinal | Mapping used on the observed path | Evidence or limit |
| --- | --- | --- |
| `OpenGLES[51,52]` | Generate buffer/texture names | Initialization at `0x180610A0`; buffer storage/upload is not implemented |
| `OpenGLES[5,9,18,28]` | Blend function, clear color, fixed-point current color, depth function | State wrapper at `0x18012E68` and immediate callers |
| `OpenGLES[32,36,71]` | Disable, enable, query capability | Queries include scissor state `0x0C11` |
| `OpenGLES[56]` | Integer current-color query | `0x1800779C` forwards it; `0x1801B868` shifts returned normalized signed integers by 15 |
| `OpenGLES[63,127]` | Get/set texture-environment mode | Target `0x2300`, property `0x2200`; ordinal 63 writes through R2 |
| `OpenGLES[140,160]` | Display/current-surface tokens | Presentation sequence at `0x1800D5DC`; host has one of each |
| `OpenGLES[163,165]` | Finish/swap | Swap presents the native software framebuffer |

Ordinal 56 is **not** a fixed-point color query: returning `0x10000` for white
makes the rasterizer's subsequent `>> 15` nearly black. Signed normalized
integer colors restore readable text and the original background artwork.
The host-contract test checks the conversion at the precision Phase consumes
and checks that presentation preserves color, enable state, and texture mode.
Only texture unit zero is supported. Unverified GPU uploads remain explicit
stops; this is not a complete OpenGL ES implementation.

### Read-only file service

Phase's asynchronous request layout differs from Vortex's:

| Offset | Field used by this adapter |
| --- | --- |
| `+0x00` | Next completed request |
| `+0x04` | Event state, managed by native code |
| `+0x08`, `+0x0C` | Native callback and cookie; preserved |
| `+0x10` | Operation byte: 1 open, 2 close, 3 read, 5 seek |
| `+0x14` | File object; its `+8` receives result count/size |
| `+0x18`, `+0x1C` | Signed seek offset and origin |
| `+0x20`, `+0x24` | Buffer and requested byte length |
| `+0x28` | Completion status: zero success, `0xFFFFFFFF` failure |
| `+0x2C`, `+0x30`, `+0x34` | Result byte count, position, handle |

Constructors at `0x1806BC14`–`0x1806BD40` establish these fields. Framework
ordinal 0 opens a read-only game resource; ordinals 1/2 submit close/read/seek
requests. The host queues completion and links requests into context `+0x2C`
at the next frame boundary. Native `0x1802F47C`/`0x1802F460` dispatch the original
callbacks. The host never calls a callback early or parses the archive itself.

Only resource kind zero and read-only open flag one are supported. Paths must
remain under the canonical asset root; parent traversal, absolute paths, and
alternate stream syntax are refused. Files are bounded to 64 MiB and loaded
into host memory for this preview. Writes and save-store services remain
unsupported. Tests cover successful and missing opens, reads, EOF, end-relative
seeks, queued ordering, rejection of writes, and path containment.

### Explicit preview conventions

Settings supplies English, Users supplies an empty profile list, and InputEvents
supplies no buttons or wheel contact. This naturally leads to name entry. The
battery indicator is full and not charging. Escape closes the host window;
it is not delivered as a game button.

Firmware `0x0829F3A4` backs `miscTBD[8]`, which builds a path into a bounded
caller buffer. The host resolves it against the asset root. Audio ordinals
14/29 maintain the observed stream registration count; 18 marks the selected
stream active; 41/42 answer playing/not-paused; 1 and 16/19 handle the startup
repeat/reset sequence silently. SoundEffect creates handles and retains the
supplied descriptor fields but never plays samples. These are deliberately
limited preview contracts, not verified firmware audio semantics. No decoding,
audio synchronization, profile persistence, or rhythm judgment is implemented.

### Extended acceptance boundary

`tests/verify_startup.py --assets PATH` independently executes the original ARM
initializer and **three complete frames**, with equivalent diagnostic host
contracts. It compares 250 SDK calls, R0–R14, CPSR, and every mapped byte,
including the software framebuffer. The screenshot read back from Windows
OpenGL matches all 320 × 240 pixels of that ARM framebuffer after RGB565
expansion. The header read and archive open also occur inside this comparison.

The later 300-frame name-entry run is a native/visual smoke test. It is outside
the current ARM comparison, and neither result proves physical-device fidelity.
Receipts and screenshots stay local under ignored `build/`; original assets and
generated translations are not included in source control.
