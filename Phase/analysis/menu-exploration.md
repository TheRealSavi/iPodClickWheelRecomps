# Phase menu exploration

This Windows sweep uses the original game, isolated copies of a test player's
save, keyboard input replays, screenshots, and complete SDK traces. Original
assets, screenshots, native saves, and traces remain private under `build/`.
The image and firmware hashes are pinned in [startup](startup.md).
An action is covered only when its destination or effect was inspected; issuing
a key sequence alone does not prove the intended menu item was reached.

## Missing commands found and implemented

| Action | SDK service | Evidence and behavior |
| --- | --- | --- |
| Open Brightness | `miscTBD[2]` | Native `0x18044EF0` dispatches `get_backlight` through `0x180051F4`. Firmware `0x0829F2AC` reads the backlight manager's current percentage. |
| Adjust Brightness | `miscTBD[1]` | Native `0x18005630` caps the argument at 100. Firmware `0x082A2538` delegates to `0x081971C0`, which clamps signed input to 0–100 and returns zero. |
| Confirm Delete User | `Users[3]` | Native `0x180365FC` gets the object by persistent ID, calls this service, and checks success before changing the active player. Firmware `0x082A2578` delegates to `0x081143E8`; its assembly converts a successful removal index to boolean 1, failure to 0. |
| Confirm changed name | `Users[9]` | Native `0x1800726C` rejects another player's existing name, then passes the user object and name. Firmware `0x082A26B8` ignores null objects, otherwise calls `0x082587F0`, replacing the name and marking users dirty. |

Pre-fix traces stopped at `miscTBD[2]` (LR `0x18045024`), `Users[3]`
(LR `0x18036614`), and `Users[9]` (LR `0x1800729C`). Brightness's setter was
identified while following the getter's companion path. The same native routes
now complete and shut down normally.

The brightness host adapter dims the presented RGB pixels. It deliberately uses
a readable minimum and a linear 20–100% presentation scale; this is a desktop
approximation, not a reconstruction of the physical LCD's luminance curve. The
SDK value remains 0–100. It is session-local and never controls the user's monitor.

Profile deletion required separating persistent IDs from enumeration order.
The snapshot migration and atomic storage behavior are described in
[persistence](persistence.md). Unknown SDK services remain explicit failures.
In particular, `Users[6,7]` were not guessed to be rename: firmware evidence shows
those entries copy 10-byte fields from the user object; rename is ordinal 9.

## Coverage

| Area | Exercised paths |
| --- | --- |
| Main menu | Quick Spin, Marathon, Volume, Options, High Scores, Help, Exit Game and its confirmation |
| Library browsing | Original Phase soundtrack list; Artists, Albums, Songs, Genres; artist and genre drill-down to difficulty selection |
| Options | Music/SFX volume adjustment; brightness adjustment; shuffle toggle; Choose Journey list; About/credits |
| Users | Create a second user; select user; rename and reload; duplicate-name handling; delete sole player; delete one of two players; create after deletion with a new ID |
| High Scores | Top Songs/detail, Clear High Scores confirmation, cleared counters |
| Help | All nine pages, Select and touch-wheel navigation, return to main menu |
| Quick Spin | Original soundtrack and analyzed custom-track gameplay; pause/resume; pause Options; Restart; confirmed Abandon; Save & Exit; short-song results and new high score |
| Continue game | Launch from Save & Exit, inspect Continue Saved Game prompt, accept Resume and reach gameplay |
| Marathon | Difficulty selection, song selection, gameplay, pause menu; a one-song library correctly reports that at least five playable songs are required |

The private sweep includes intentionally mis-targeted routes while calibrating
wheel movement. These are not counted as coverage of their intended destination.
Disabled items and remembered selection can change wheel steps. Help ignores the
Previous/Next button presses tested here; Select and wheel movement do turn pages.
The original code decides these controls; the host does not remap them by screen.

This is not exhaustive gameplay verification. Remaining gaps include every
journey/difficulty/song combination, a full multi-song Marathon through its final
ranking/unlocks, a maximum-length run, physical-iPod save import, and precise
timing/audio-position fidelity after restoring a saved game. Reaching resumed
gameplay establishes that path's SDK coverage, not sample-accurate continuation.

## Repeatable checks

```powershell
uv run --python 3.12 python Phase/tools/build.py
uv run --python 3.12 python -m unittest discover -s Phase/tests -p test_*.py
uv run --python 3.12 python Phase/tests/verify_actions.py `
  --image C:/private/Phase_3168002.plain.bin --assets C:/private/Phase-assets
```

The action verifier creates its own profile, runs each mutation in another
process, inspects persistent IDs/names, and reloads the remaining player's native
save. It requires the relevant SDK calls and normal shutdown/teardown, and leaves
screenshots and per-case SDK inventories under `build/menu-actions` for review.
Authored tests cover signed brightness clamping/readback, profile handle stability,
deletion without renumbering, non-reuse of IDs, version-1 migration, and preservation
of another player's exact score bytes. Existing startup, menu, playback, and
persistence verifiers remain applicable.
