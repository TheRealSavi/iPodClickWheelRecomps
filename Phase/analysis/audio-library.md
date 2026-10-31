# Audio and local library bring-up

This extends the earlier silent, empty-library preview. The executable and
firmware hashes remain those in [startup.md](startup.md). No original binary,
music, chart, translated code, or screenshot is committed.

## Host boundaries

`library.cpp` reads an uncompressed iTunesDB into immutable host records. It
checks chunk extents, bounds, identities, strings, and relative media paths.
Dataset 3 takes precedence over its older dataset-2 counterpart. The browser
filters music, retains playlist occurrence order, and exposes album/artist/genre
groups. SDK handles are host indices, not pointers into the original firmware.
Collation uses host UTF-8 lexical order; full firmware collation is not claimed.

`audio.cpp` owns decoded PCM and XAudio2 voices. Music uses Media Foundation's
synchronous Source Reader, requested as 16-bit PCM. One music clip is retained,
bounded to 384 MiB; each effect copies the guest PCM into owned host memory.
Destroying a voice precedes releasing its sample memory. Audio failure is
explicit, rather than pretending a stream is playing. Diagnostic runs without
`--audio` retain the previous silent behavior for reproducible ARM comparisons.

Audible runs pace host callbacks and use monotonic wall time for miscTBD 12.
The native music clock separately consumes XAudio2's sample position in
milliseconds. Pause stops sample consumption; Resume retains the queued PCM.
No judgment logic, chart selection, or game animation is replaced by host code.

Relevant Windows contracts:

- [Source Reader](https://learn.microsoft.com/en-us/windows/win32/medfound/source-reader)
- [Voice state and sample counts](https://learn.microsoft.com/en-us/windows/win32/api/xaudio2/ns-xaudio2-xaudio2_voice_state)
- [Buffer ownership and looping](https://learn.microsoft.com/en-us/windows/win32/api/xaudio2/nf-xaudio2-ixaudio2sourcevoice-submitsourcebuffer)

## Native SDK evidence

| Service | Implemented meaning | Evidence |
| --- | --- | --- |
| Metadata 61/89/162/164 | Playlist count, handle, 64-bit PID, title | Native playlist selector `180416C4`; firmware import table |
| Metadata 48/63/78 | Select playlist, song count, indexed Track handle | Native wrappers `18006AA0`, `180069DC`, `18006990` |
| Metadata 97/119/120 | 64-bit Track PID, duration seconds/milliseconds | Firmware `082A1148`, `082A0EEC`, `082A0F10` |
| Metadata 100/101/107 | Media path, title, file type | Native `18006DF0`, `18006BE0`, `1804150C` |
| Metadata 58/59/60 | Album/artist/genre group counts | LibraryBrowser vtable `0895B978`; native group builders `080322A0`, `080322F4`, `08032590` |
| Metadata 81/80/82/44 | Group handles and names | Firmware name readers `08030EF0`, `08031018`, `0803141C` |
| Metadata 95/160 | Release Track/playlist descriptor | Firmware `0829F824` / `0829F808`; immutable host handles need no release |
| Audio 14/16/18/19 | Register, clear, play queue index, stop | Native `18006C40`, `18006D48`, `18006D68`, `18006DF0` |
| Audio 28/31 | Current queue index / playback position in ms | Native clock `180287C4`, wrappers `18006E58` and `18006CBC` |
| Audio 20/33 | Pause toggle / playback status | Native `18006D2C`, `18006D78`, `18006CD4`; live pause/resume |
| Audio 1/41/42 | Set repeat / repeat-mode queries | Native `18006EA8`, `18006D04`; corrected during [persistence work](persistence.md) |
| Audio 3/40 | Set/read shuffle policy | Native `18006EF4`, `18006E98`; restoration during shutdown |
| Audio 47/48/49 | Volume, volume scale, set volume | Native `180055A8`, `18005648`; firmware volume getter `08166358` |
| SoundEffect 7–16/2/5/35 | PCM and voice parameters, play, stop, status | Native setters at `18010CEC`–`18010D34` and play `1800D67C`; matching shared title contracts |

AsyncFileIO kind-1 reads first check the session save map, then the mounted
`iPod_Control/GameData_RW/1D000` directory. The original native parser reads the
Phase Library index and decrypts the PHZ itself. Files remain untouched.
The bridge adds deletion of unused software-renderer buffer names and GL scissor
state, both reached when the tutorial ends and gameplay begins.

## Verification and limits

The private five-song library was produced using iOpenPod's version-5 analyzer,
with its previously captured Demucs part evidence. Audio excerpts are cut to
the corresponding offsets and lengths. Full Poppies produces 1,252 transients
and 400 beat records; those are analysis events, not a count of playable notes.

The native game selects Poppies, accepts its chart, reaches Hard gameplay, and
renders animated notes and targets with real audio. A visible Windows keyboard
test opens the original pause screen with W and resumes with S through the
native countdown. Audio logs confirm paused/resumed output. The authored
playback replay makes those checks repeatable against the private library.

Authored host tests cover playlist membership (including duplicate occurrences),
media filtering, full-width IDs, bounded Unicode strings, duration units,
path containment, malformed input, and read-only behavior. Device-backed audio
checks exercise clock advancement, pause/resume, stop, repeat, and end of stream.
The earlier instruction, three-frame ARM, and menu regressions remain separate.

This does not establish physical-iPod latency equivalence or full-game fidelity.
Profiles remain session-only. Queue-wide shuffle/repeat semantics, streaming,
full-song scoring/results, Marathon, all library variants and menu routes need
additional work. Known single-track playback and pause behavior are the tested
scope; unsupported services still fail visibly.
