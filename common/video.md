# Shared desktop video presentation

Phase, Vortex, Cubis2, HoldEm, Lost, Sims Bowling, and Mini Golf use the same
`ipod::video::Presenter`. It owns RGB24 uploads, texture caching, filtering,
reduction, aspect fitting, black borders, and screenshot readback. Game adapters
provide frames, window events, and saved options; none has a separate scaling
implementation. Window resizing changes presentation, not game coordinates,
click-wheel input, chart timing, or internal rendering resolution.

## Player options

All seven desktop hosts accept **F6** to cycle Sharp → Nearest → Smooth → NVIDIA
NIS and **F7**
to toggle integer scaling. Choices persist through each host's settings store.
The six existing settings windows offer the same choices. Phase shows the active
filter and Fit/Integer mode in its title. Repeated or modified shortcuts are ignored.

- **Sharp** (default): enlarge to whole pixel blocks, then smoothly fit those
  blocks to the window. Edges stay clearer at fractional window sizes.
- **Nearest**: enlarge with hard pixel blocks. Fractional sizes can make adjacent
  blocks different widths.
- **Smooth**: bilinear enlargement, with softer edges.
- **NVIDIA NIS**: edge-aware GPU enlargement with adaptive sharpening on Windows
  using SDL's Direct3D 11 backend. Tested on the RTX 4070. It uses the finished
  game frame, so it does not need motion vectors or change gameplay timing.
  This is NVIDIA Image Scaling, not DLSS, AI reconstruction, or frame generation.
- **Integer scaling**: prefer a whole-number enlargement, leaving more black
  border when necessary. The presenter falls back to fractional fitting when
  the window is smaller than the logical frame; it does not force a larger window.

All filters average reductions in successive halves, then fit the result. This
preserves detail when a game's internal renderer supplies a larger frame than
the display area. Sharp's intermediate enlargement is capped at eight times the
source size. Intermediate textures are reused and recreated when either dimension
changes. Resizing never snaps the window to a different size.

NIS runs only when enlarging. It preserves native pixels at 1× and uses the normal
filtered reduction when shrinking. Enlargements above 2× use multiple bounded
passes, applying moderate sharpening on the final pass. Shader bytecode is
compiled offline and embedded, so selecting it does not compile shaders in-game.
It adds no NVIDIA runtime installer or model download. Unsupported backends or
initialization failures use Sharp and report the fallback in the log; Phase also
shows it in the title. Sharp remains the default, since NIS may produce halos or
alter thin text and pixel art. Compare the modes with F6 at a large window size.

## Integration

`src/ipod/platform/video_options.h` defines the shared enum, labels, and defaults
without an SDL dependency. Existing game settings retain their serialized keys.
The new mode appends an enum value and the `nvidia-nis` key; old selections retain
their meanings. The NVIDIA shader's MIT license and provenance are in
`common/third_party/nis`.
`src/ipod/platform/sdl3/presenter.{h,cpp}` implements the presenter; CMake exports
its source through `IPOD_CORE_SDL3_SOURCES`. Phase's Windows build links the same
source and SDL3 library, and copies the SDL DLL and license beside its executable.

Attach a dedicated `SDL_Renderer` with the game's logical dimensions, set options,
then call `present(rgb, width, height)` on the main thread. The presenter owns its
textures but borrows the renderer: call `reset()` before destroying that renderer.
`capture()` redraws the last uploaded frame before readback, because SDL invalidates
the backbuffer after presentation. The caller owns the returned `SDL_Surface`.
Readback includes the full drawable and black borders.

`presentation.h` handles SDL logical presentation and physical content dimensions.
Offscreen targets disable logical presentation, avoiding double letterboxing.
The independent `video.h` rectangle calculation is used as a test reference.

Phase converts its original software RGB565 framebuffer to RGB24 and applies
emulated brightness before calling the shared presenter. It retains a **hidden
OpenGL compatibility context only for original SDK state and queries**. That
context never displays or scales the game. SDL owns the visible window and input;
the adapter restores the SDK context after presentation and before SDK GL calls.
Audio remains on XAudio2 and Media Foundation.

Phase stores versioned host video options under the `host/video` record in its
existing atomic save snapshot. Native profile/score formats are unchanged, and
default settings create no extra record until the player changes an option.

API references:
[SDL logical presentation](https://wiki.libsdl.org/SDL3/SDL_SetRenderLogicalPresentation),
[SDL content rectangle](https://wiki.libsdl.org/SDL3/SDL_GetRenderLogicalPresentationRect),
[SDL screenshot timing](https://wiki.libsdl.org/SDL3/SDL_RenderReadPixels).

## Verification

Run the standalone shared tests without proprietary game assets:

```powershell
cmake -S common/tests -B build-video -DSDL3_DIR=C:/SDK/SDL3/cmake
cmake --build build-video --config Release
ctest --test-dir build-video -C Release --output-on-failure
```

This compiles all six desktop adapters and their settings implementations. The
shared renderer test uses authored white frames, sharp edges, and checkerboards
to check all filters, black bars, successive resizes, integer scaling, reduction,
height-only source changes, target isolation, and shortcut handling.
On Windows a second test uses D3D11 and requires actual NIS dispatch. It checks
border/content pixels, repeated frames, 4.5× enlargement, native-size bypass,
and output differing from bilinear interpolation; the software test checks the
fallback. The tests compile with exceptions and RTTI disabled like the other games.

Phase's authored host test exercises its actual SDL window and renderer, preserved
SDK GL state, SDL keyboard delivery, filter shortcuts, options across restarts,
resized screenshots, and dim/restore behavior. Native startup verification
compares registers, memory, SDK calls, and pixels with ARM execution. Menu,
persistence, and
audible Hard gameplay replays check the integration with the original game.

These checks do not constitute full gameplay verification of the other six
titles or validation on every OS/GPU. Their adapters compile; presentation runs
through the tested shared implementation.
