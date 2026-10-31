# Phase startup evidence

Recorded 2026-10-31 for build 3168002, SHA-256
`35834fede51f170a0b0bda8e380e1c12af0f86ca1a7952e5abfbb24ab4082258`.
This is static analysis plus native/emulated comparison, not an observation of a
running iPod or playable Windows game.

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
context field at `+0x38`. In the reason-zero path it creates the configuration
loader and sets the answer byte to 1; at its end it copies the answer back into
the context. The current diagnostic uses a zeroed context, no input events, and
separate context/answer blocks at `0x19000000` and `0x19000100`.

Initialization invokes the constructor walker at `0x18002844`. Its literals at
`0x1800287C` and `0x18002880` give table bounds relative to `0x18002854` and
`0x1800285C`. Each entry is an offset relative to its own slot, with the low two
bits removed after addition. Recovering these 42 entries fixes missing indirect
targets without importing a Ghidra project or skipping original constructors.

## Host services reached

| Service | Observed contract | Diagnostic implementation |
| --- | --- | --- |
| `MemoryAlloc[0]` | The arena initializer calls from `0x180115C8`, passes `0x7FF80` bytes, and retains the returned address | 16 bounded, aligned backing allocations; game suballocation code unchanged |
| `miscTBD[12]` | `0x180055D0` passes an output word; `0x180055E4` compares elapsed readings with 1,000,000 | Deterministic microsecond clock, advanced per frame and one unit per read; not an audio clock |
| `OpenGLES[52]` | `0x180610A0` calls from `0x180610F8` with count 1 and output pointer `0x180F6134` | Unsupported: exact stop recorded |

The graphics initialization loops over 64 objects using ordinal 52, then 64 more
using ordinal 51. Their final renderer mappings remain to be verified. Silently
returning zero here would let startup proceed with invalid graphics objects.

Ordinal identities differ across titles: Vortex's `miscTBD[12]` is a wall-clock
structure, while Phase's caller supplies only one word. Copying Vortex's binding
would corrupt Phase's stack. The Phase-specific output-word clock is supported
by its consumers; the diagnostic return-register policy is a host convention.

## Acceptance boundary

The current native run completes initialization after 2,150 translated function
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
