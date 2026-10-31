#include "ipod/platform/sdl3/presentation.h"
#include "ipod/platform/sdl3/presenter.h"
#include "ipod/platform/video.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {
void check(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "FAIL %s: %s\n", message, SDL_GetError());
        std::exit(1);
    }
}
}  // namespace
int main(int argc, char** argv) {
    check(ipod::video::fit(320, 240, 0, 0).width == 0, "minimized output");
    check(ipod::video::fit(0, 240, 960, 540).width == 0, "invalid source");
    check(SDL_Init(SDL_INIT_VIDEO), "video init");
    SDL_Window* window =
        SDL_CreateWindow("Presentation test", 960, 540, SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE);
    check(window != nullptr, "window");
    const char* backend = argc > 1 ? argv[1] : "software";
    SDL_Renderer* renderer = SDL_CreateRenderer(window, backend);
    check(renderer != nullptr, "renderer");
    ipod::video::Presenter presenter;
    presenter.attach(renderer, 320, 240);
    std::vector<unsigned char> white(320 * 240 * 3, 255);
    for (auto mode : {ipod::video::Scaling::Sharp, ipod::video::Scaling::Nearest,
                      ipod::video::Scaling::Smooth, ipod::video::Scaling::NvidiaNis}) {
        presenter.set_options({mode, false});
        for (const auto& size : std::array<std::array<int, 2>, 5>{
                 {{960, 540}, {500, 900}, {319, 241}, {160, 120}, {320, 240}}}) {
            check(SDL_SetWindowSize(window, size[0], size[1]), "resize");
            check(SDL_SyncWindow(window), "resize synchronization");
            check(presenter.present(white.data(), 320, 240), "present");
            int width = 0, height = 0;
            check(SDL_GetRenderOutputSize(renderer, &width, &height), "physical output");
            const auto fitted = ipod::video::fit(320, 240, width, height);
            int content_width = 0, content_height = 0;
            check(ipod::video::presentation_size(renderer, &content_width, &content_height),
                  "content size");
            check(std::abs(content_width - fitted.width) <= 1 &&
                      std::abs(content_height - fitted.height) <= 1,
                  "physical rather than logical size");
            auto* pixels = presenter.capture();
            check(pixels != nullptr && pixels->w == width && pixels->h == height,
                  "readback dimensions");
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    // SDL uses fractional rectangles; allow their one-pixel rounding
                    // boundary while checking every other content/bar pixel.
                    if (std::abs(x - fitted.x) <= 1 || std::abs(x - fitted.x - fitted.width) <= 1 ||
                        std::abs(y - fitted.y) <= 1 || std::abs(y - fitted.y - fitted.height) <= 1)
                        continue;
                    Uint8 r = 0, g = 0, b = 0, a = 0;
                    check(SDL_ReadSurfacePixel(pixels, x, y, &r, &g, &b, &a), "read pixel");
                    const bool inside = x >= fitted.x && x < fitted.x + fitted.width &&
                                        y >= fitted.y && y < fitted.y + fitted.height;
                    check(inside ? (r == 255 && g == 255 && b == 255)
                                 : (r == 0 && g == 0 && b == 0),
                          "content or black bar");
                }
            }
            SDL_DestroySurface(pixels);
        }
    }
    check(SDL_SetWindowSize(window, 960, 540) && SDL_SyncWindow(window), "integer resize");
    presenter.set_options({ipod::video::Scaling::Sharp, true});
    check(presenter.present(white.data(), 320, 240), "integer scaling");
    int width = 0, height = 0;
    check(ipod::video::presentation_size(renderer, &width, &height) && width == 640 &&
              height == 480,
          "integer content size");
    auto* target =
        SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, 64, 48);
    check(target && SDL_SetRenderTarget(renderer, target), "intermediate target");
    check(ipod::video::presentation_size(renderer, &width, &height) && width == 64 && height == 48,
          "intermediate has no double letterboxing");
    check(SDL_SetRenderTarget(renderer, nullptr), "restore window target");
    check(ipod::video::presentation_size(renderer, &width, &height) && width == 640 &&
              height == 480,
          "window presentation retained");
    SDL_DestroyTexture(target);
    check(SDL_SetWindowSize(window, 160, 120) && SDL_SyncWindow(window), "small integer resize");
    check(presenter.present(white.data(), 320, 240), "small integer presentation");
    check(ipod::video::presentation_size(renderer, &width, &height) && width == 160 &&
              height == 120,
          "integer mode fits below native size");
    // A high-resolution checkerboard must average down rather than alias.
    check(SDL_SetWindowSize(window, 320, 240) && SDL_SyncWindow(window), "reduction resize");
    std::vector<unsigned char> checker(2560 * 1920 * 3);
    for (int y = 0; y < 1920; ++y)
        for (int x = 0; x < 2560; ++x)
            for (int c = 0; c < 3; ++c)
                checker[(y * 2560 + x) * 3 + c] = (x + y) % 2 ? 255 : 0;
    for (auto mode : {ipod::video::Scaling::Sharp, ipod::video::Scaling::Nearest,
                      ipod::video::Scaling::Smooth}) {
        presenter.set_options({mode, false});
        check(presenter.present(checker.data(), 2560, 1920), "multistep reduction");
        auto* pixels = presenter.capture();
        Uint8 r, g, b, a;
        check(pixels && SDL_ReadSurfacePixel(pixels, 160, 120, &r, &g, &b, &a), "reduced sample");
        check(r >= 126 && r <= 129 && r == g && g == b, "reduction averages detail");
        SDL_DestroySurface(pixels);
    }
    // Reuse across source changes, including height-only changes, exercises all
    // texture caches without recreating the presenter.
    check(SDL_SetWindowSize(window, 960, 540) && SDL_SyncWindow(window), "filter resize");
    std::array<int, 3> blended{};
    for (unsigned mode = 0; mode < 3; ++mode) {
        presenter.set_options({static_cast<ipod::video::Scaling>(mode), false});
        for (int h : {240, 120, 240}) {
            std::vector<unsigned char> edge(320 * h * 3);
            for (int y = 0; y < h; ++y)
                for (int x = 160; x < 320; ++x)
                    for (int c = 0; c < 3; ++c)
                        edge[(y * 320 + x) * 3 + c] = 255;
            check(presenter.present(edge.data(), 320, h), "source dimensions change");
        }
        auto* pixels = presenter.capture();
        check(pixels != nullptr, "filter capture");
        for (int x = 450; x < 510; ++x) {
            Uint8 r, g, b, a;
            check(SDL_ReadSurfacePixel(pixels, x, 270, &r, &g, &b, &a), "edge sample");
            blended[mode] += r > 0 && r < 255;
        }
        SDL_DestroySurface(pixels);
    }
    check(blended[1] == 0 && blended[2] > blended[0],
          "Nearest hard edge; Smooth softer than Sharp");
    SDL_Event event{};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.scancode = SDL_SCANCODE_F6;
    check(presenter.handle_shortcut(event) &&
              presenter.options().scaling == ipod::video::Scaling::NvidiaNis,
          "cycle scaling");
    check(presenter.present(white.data(), 320, 240), "NIS or explicit Sharp fallback");
    const bool gpu = std::string(backend) == "direct3d11";
    check(presenter.nis_active() == gpu, "NIS actually dispatched on D3D11");
    if (gpu) {
        std::vector<unsigned char> diagonal(320 * 240 * 3);
        for (int y = 0; y < 240; ++y)
            for (int x = 0; x < 320; ++x)
                for (int c = 0; c < 3; ++c)
                    diagonal[(y * 320 + x) * 3 + c] = x > y ? 210 : 40;
        const auto captured = [&]() {
            check(presenter.present(diagonal.data(), 320, 240), "diagonal presentation");
            auto* image = presenter.capture();
            check(image != nullptr, "diagonal capture");
            auto* rgb = SDL_ConvertSurface(image, SDL_PIXELFORMAT_RGB24);
            check(rgb != nullptr, "diagonal conversion");
            std::vector<unsigned char> result;
            for (int y = 0; y < rgb->h; ++y) {
                auto* row = static_cast<unsigned char*>(rgb->pixels) + y * rgb->pitch;
                result.insert(result.end(), row, row + rgb->w * 3);
            }
            SDL_DestroySurface(rgb);
            SDL_DestroySurface(image);
            return result;
        };
        // A non-round 4.5x enlargement requires three bounded NIS dispatches.
        check(SDL_SetWindowSize(window, 1440, 1080) && SDL_SyncWindow(window), "large NIS resize");
        auto enhanced = captured();
        check(presenter.nis_active(), "cascaded NIS active");
        check(enhanced == captured(), "NIS deterministic across repeated frames");
        presenter.set_options({ipod::video::Scaling::Smooth, false});
        check(enhanced != captured(), "NIS differs from ordinary bilinear scaling");
        presenter.set_options({ipod::video::Scaling::NvidiaNis, false});
        check(SDL_SetWindowSize(window, 320, 240) && SDL_SyncWindow(window), "NIS native resize");
        check(captured() == diagonal && !presenter.nis_active(),
              "native pixels bypass enhancement");
    }
    check(presenter.handle_shortcut(event) &&
              presenter.options().scaling == ipod::video::Scaling::Sharp,
          "cycle back to Sharp");
    event.key.repeat = true;
    check(!presenter.handle_shortcut(event), "ignore repeats");
    event.key.repeat = false;
    event.key.mod = SDL_KMOD_CTRL;
    check(!presenter.handle_shortcut(event), "ignore modified shortcuts");
    event.key.mod = 0;
    event.key.scancode = SDL_SCANCODE_F7;
    check(presenter.handle_shortcut(event) && presenter.options().pixel_perfect,
          "toggle integer scaling");
    check(!presenter.present(nullptr, 0, 0), "reject invalid frame");
    presenter.reset();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    std::puts("PASS: resize, aspect ratio, black bars, physical pixels, integer scaling, target "
              "isolation");
}
