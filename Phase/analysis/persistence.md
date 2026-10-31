# Phase profiles and score persistence

## Native contract

The native executable and firmware hashes are pinned in [startup](startup.md).
No original code, save data, game images, or generated translations are distributed.

`Users[4]` creates a named profile; `[0]` counts profiles; `[1]` looks up by index;
`[2]` looks up by persistent ID; `[5]` gets the name; `[8]` gets the ID. The host
retains profile order and stable positive IDs across launches. Up to five profiles
are supported. Native score records use that ID in their filenames.

The native writer at `0x1803DB7C` constructs a 12-byte header containing a zero,
total length, and additive checksum of payload words. It calls `0x18004B44`,
which opens a save slot (`AsyncFileIO[12]`), writes the complete record (`[16]`),
and closes it (`[14]`). `0x1800E254` writes the selected-user record `default`.
`0x18024678` tries `save_<id>.dtb`, then `save_<id>_back.dtb`; `0x1802434C` creates
a default backup when neither loads. `0x18024F0C` serializes profile/game state.
The game supplies the `.dtb` suffix itself; the host must not add or strip it.

The frame callback's reason **5** requests application shutdown; response **6**
acknowledges completion. `0x18005BB4` calls the application destructor, including
`0x1805C828`, which serializes the current profile via `0x180249A4`. Only then may
the host call header-slot-1 teardown `0x1807EAB0`. Calling that header entry alone
frees arenas without flushing the application state. Persistent sessions follow
both phases on normal close and stop if shutdown does not acknowledge within
300 callbacks. Early diagnostic screenshots/memory dumps remain pre-shutdown.

Shutdown also writes kind-2 `en/stats` through `0x18006FC0`. Kind-1 profile
records and kind-2 statistics are distinct namespaces in the host snapshot;
neither writes to the mounted library. Kind-2 names may contain validated relative
segments, but remain keys inside the snapshot rather than host filesystem paths.
The host also releases tracked SDK arena allocations and forwards the observed
literal `DebugUtil[4]` messages to the log (firmware `0x082A23F0` branches to
`0x082954F4`, a `Semihost_DebugVPrintf` wrapper).

The exit path exposed a prior audio-contract error: `Audio[41,42]` are repeat
mode queries, mapped to modes 1 and 2 by native `0x18006D04`; playback status
comes from `[33]`. `[40]` reads shuffle and `[3]` restores it, with native
`0x18006EF4` mapping enabled/disabled to 1/2. These are now separate from playback
and pause state. Phase queues one track, so shuffle retains policy only.

Firmware `0x0829EF30` delegates replacement of the complete buffered record to
`0x08126F9C`: allocate/reallocate, copy bytes, set size and dirty flag. The flush
routine `0x08127014` writes bytes to the built path. `0x08127100` releases
a slot reference, retaining dirty data until flushed. This supports persisting
opaque whole records; it does not require reimplementing Phase's score schema.
The host commits synchronously before returning success, a deliberate durability
difference from deferred firmware flushing.

## Host storage

`persistence::Store` owns a versioned snapshot of profile names and exact native
record kinds/names/bytes. The snapshot has a CRC32 to detect host-file damage.
Limits are five profiles, 31 bytes per profile name, 32 records, 255 bytes per
record name, 100 KiB per record, and 4 MiB per snapshot. Native save slots remain
limited to four. Record names are serialized keys, never filesystem paths.

Each mutation validates a candidate state, creates a fresh staging file, writes
and flushes it, then replaces the primary with `ReplaceFileW`, retaining the old
primary as `state.bak`. Initial/recovery commits use `MoveFileExW`; recovery never
promotes a damaged primary over the valid backup. Memory changes only after a
successful commit. Failure stops the preview explicitly instead of reporting a
successful SDK write. Stale staging files are not loaded.

A process holds an exclusive `CreateFileW` lock for the entire session. Save
directories overlapping assets or the mounted library are refused before any
directory creation. Fixed save-file paths reject reparse points and directories;
staging uses `CREATE_NEW` so it cannot truncate an existing target. These measures
protect ordinary local use; they are not a sandbox against another program
deliberately racing filesystem changes. This is not a claim of physical-disk
power-loss guarantees.

Windows API behavior follows Microsoft's documentation for
[CreateFileW](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew),
[FlushFileBuffers](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers),
and [ReplaceFileW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-replacefilew).

Visible `tools/run.py` sessions default to
`%LOCALAPPDATA%/iPodClickWheelRecomps/Phase`. `--save-dir` selects another directory;
`--ephemeral` disables persistence. Hidden/native diagnostic sessions remain
temporary by default so regression replays never consume interactive profiles.
Every submitted write is committed immediately. Normal closing additionally runs
the native shutdown save. Forced termination can lose unsubmitted game changes;
an in-progress song is not a verified resumable checkpoint.

## Verification

- Authored fixtures run in separate processes and verify names, stable IDs,
  exact binary records, partial-staging exclusion, primary/backup recovery,
  refusal to reset when both copies are bad, write-failure behavior, bounds,
  protected roots, and exclusive writer locking.
- Runner tests distinguish visible defaults, explicit directories, and temporary
  diagnostic sessions.
- `tests/verify_persistence.py` runs the original translated game twice with the
  known assets. The first process creates profile A and writes `default` (24
  bytes), `save_1_back.dtb` (436 bytes), and `save_1.dtb` (641 bytes). The second
  loads the existing profile, opens the 641-byte record, and renders the original
  main menu without input or another `Users[4]` call. Rendered menu text matches
  the previously verified mask. Both processes complete native shutdown and
  teardown; the first also writes the 5,808-byte kind-2 `en/stats` record.
- A private 30-second Poppies excerpt was prepared with iOpenPod's analyzer and
  real database/chart writers. A scripted Medium run completed the song, then
  normal shutdown grew `save_1.dtb` from 641 to 749 bytes. Two later processes
  loaded that record. The native High Scores screen showed one song played and
  seven notes caught; Top Songs displayed **3,210** for Poppies (excerpt).
  Private receipts/screenshots are in `build/persistence-earned`. This verifies
  a game-earned score through serialization, shutdown, reload, and display;
  it does not establish long-song timing accuracy or every difficulty.

Snapshot version 2 stores each profile's explicit ID and a monotonically increasing
next ID. Version 1 loads with its original index-plus-one IDs and upgrades only
on a subsequent mutation. Unsupported versions still fail without rolling back
to a backup. Deleting one profile removes only its `save_<id>.dtb` and
`save_<id>_back.dtb` records. Remaining players retain their exact record bytes
and IDs. Guest handles occupy reusable slots separate from persistent IDs.
Renaming preserves both identity and score records. The native game continues to
own selection, default-user changes, and its global statistics serialization.

`tests/verify_actions.py` exercises rename, reload, deletion of the last player,
creation after deletion, deletion with another player present, and reloading that
remaining player's original ID. See [menu exploration](menu-exploration.md) for
the firmware contracts. Migration from physical iPod saves, all Marathon save
paths, and long-term compatibility with other Phase builds remain unverified.
