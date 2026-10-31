# Shared desktop video presentation

All seven desktop hosts allow arbitrary window sizes. The game frame stays
centered, preserves its aspect ratio, and fits completely inside the drawable.
Unused space is black. Resizing changes presentation, not game coordinates,
click-wheel input, chart timing, or the renderer's configured internal resolution.

The six SDL games keep Nearest, Smooth, and Sharp scaling. Pixel-perfect mode
fits a whole-number enlargement inside the window rather than snapping the window
itself to a different size. The existing size shortcuts and fullscreen controls
still work. Phase uses smooth GPU texture scaling and supports the ordinary
Windows resize/maximize controls. Its default client size remains 320×240 so
native-resolution diagnostic screenshots remain comparable.

## Reuse

- `src/ipod/platform/video.h`: dependency-free aspect-fit rectangle calculation.
  Handles wide, tall, small, odd-sized, and zero-sized/minimized outputs.
- `src/ipod/platform/sdl3/presentation.h`: shared SDL logical presentation policy
  and physical content size query. Call `configure_presentation` on the window
  target; use `presentation_size` to size intermediate scaling textures. SDL's
  current-render-output query returns logical dimensions when enabled, so it
  cannot be used as the physical magnification size.
- `src/ipod/platform/opengl_presenter.{h,cpp}`: reusable RGB24 presenter for a
  desktop OpenGL 1.1 compatibility context. The host supplies source and drawable
  dimensions and performs the swap. Call `release()` before destroying the
  context. Compile this source and link the platform's OpenGL library; Phase's
  build shows that integration without adding an SDL dependency.

The OpenGL presenter reuses one texture and a padded upload buffer. Copied edge
texels prevent filtering against unused storage; power-of-two storage also works
with Windows' legacy OpenGL fallback. It clears the entire drawable each frame,
then draws the fitted image, preserving the game's GL state. At native size the
existing Phase menu text mask remains identical. Phase's screenshots now include
the entire drawable, including bars, at its current size.

The live test also reached Phase's previously unmapped idle dim command,
`miscTBD[3]`, at native LR `0x1807ED44`. Firmware `0x0829EF5C` delegates to
`0x08197184`: a nonzero argument temporarily caps display brightness at 20;
zero restores the chosen brightness. The adapter now handles this without
altering that setting or Windows monitor brightness. Pixel tests cover dimming
and restoration; an 8,000-frame native replay reaches both idle and wake calls.
Firmware/image provenance is in Phase's analysis documents.

SDL intermediate targets retain their own presentation state. The shared helper
uses the actual content rectangle for window targets and physical target size
when logical presentation is disabled, preventing duplicate letterboxing and
avoiding unnecessarily large Sharp/reduction textures in wide windows.

API references:
[SDL logical presentation](https://wiki.libsdl.org/SDL3/SDL_SetRenderLogicalPresentation),
[SDL content rectangle](https://wiki.libsdl.org/SDL3/SDL_GetRenderLogicalPresentationRect),
and the [SDL 3.4.16 renderer implementation](https://github.com/libsdl-org/SDL/blob/release-3.4.16/src/render/SDL_render.c).

## Verification

The standalone shared test uses authored white frames and SDL's software backend:

```powershell
cmake -S common/tests -B build-video -DSDL3_DIR=C:/SDK/SDL3/cmake
cmake --build build-video --config Release
ctest --test-dir build-video -C Release --output-on-failure
```

This also compiles all six SDL platform adapters without proprietary assets.
Tests inspect content and bar pixels after successive wide, tall, odd-sized,
smaller-than-native, and native resizes; they check integer scaling and switching
between window and intermediate targets.

Phase's `test_host.py` checks real Win32 client resizing and GPU readback with
authored colored quadrants, including orientation, smooth interpolation, every
bar pixel, screenshot dimensions, and restoration of game GL state. Its native
menu verifier additionally checks the original game's image at 320×240.
The live Phase window was maximized and restored on Windows 11. Other titles'
adapters compile and the shared SDL backend is exercised; this does not claim a
complete gameplay replay of all six other games or validation on every OS/GPU.
