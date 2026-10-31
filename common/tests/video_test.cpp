#include "ipod/platform/sdl3/presentation.h"
#include "ipod/platform/video.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
void check(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "FAIL %s: %s\n", message, SDL_GetError());
        std::exit(1);
    }
}
}  // namespace
int main() {
    check(ipod::video::fit(320, 240, 0, 0).width == 0, "minimized output");
    check(ipod::video::fit(0, 240, 960, 540).width == 0, "invalid source");
    check(SDL_Init(SDL_INIT_VIDEO), "video init");
    SDL_Window* window =
        SDL_CreateWindow("Presentation test", 960, 540, SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE);
    check(window != nullptr, "window");
    SDL_Renderer* renderer = SDL_CreateRenderer(window, "software");
    check(renderer != nullptr, "renderer");
    auto* texture =
        SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STATIC, 320, 240);
    check(texture != nullptr, "source texture");
    std::vector<unsigned char> white(320 * 240 * 3, 255);
    check(SDL_UpdateTexture(texture, nullptr, white.data(), 320 * 3), "upload");
    for (const auto& size : std::array<std::array<int, 2>, 5>{
             {{960, 540}, {500, 900}, {319, 241}, {160, 120}, {320, 240}}}) {
        check(SDL_SetWindowSize(window, size[0], size[1]), "resize");
        check(SDL_SyncWindow(window), "resize synchronization");
        check(ipod::video::configure_presentation(renderer, 320, 240, false), "letterbox");
        int width = 0, height = 0;
        check(SDL_GetRenderOutputSize(renderer, &width, &height), "physical output");
        const auto fitted = ipod::video::fit(320, 240, width, height);
        int content_width = 0, content_height = 0;
        check(ipod::video::presentation_size(renderer, &content_width, &content_height),
              "content size");
        check(std::abs(content_width - fitted.width) <= 1 &&
                  std::abs(content_height - fitted.height) <= 1,
              "physical rather than logical size");
        check(SDL_RenderClear(renderer) && SDL_RenderTexture(renderer, texture, nullptr, nullptr),
              "draw");
        // Read the complete drawable, including letterbox areas, before swap.
        check(SDL_SetRenderLogicalPresentation(renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED),
              "read full output");
        auto* pixels = SDL_RenderReadPixels(renderer, nullptr);
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
                const bool inside = x >= fitted.x && x < fitted.x + fitted.width && y >= fitted.y &&
                                    y < fitted.y + fitted.height;
                check(inside ? (r == 255 && g == 255 && b == 255) : (r == 0 && g == 0 && b == 0),
                      "content or black bar");
            }
        }
        SDL_DestroySurface(pixels);
    }
    check(SDL_SetWindowSize(window, 960, 540) && SDL_SyncWindow(window), "integer resize");
    check(ipod::video::configure_presentation(renderer, 320, 240, true), "integer scaling");
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
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    std::puts("PASS: resize, aspect ratio, black bars, physical pixels, integer scaling, target "
              "isolation");
}
