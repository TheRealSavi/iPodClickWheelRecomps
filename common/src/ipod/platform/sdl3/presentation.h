#pragma once
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>

namespace ipod::video {
// Apply to the window target. Offscreen scale/reduction targets retain their
// own disabled logical presentation, so the image is letterboxed only once.
inline bool configure_presentation(SDL_Renderer* renderer, int width, int height,
                                   bool integer_scale) {
    return SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255) &&
           SDL_SetRenderLogicalPresentation(renderer, width, height,
                                            integer_scale ? SDL_LOGICAL_PRESENTATION_INTEGER_SCALE
                                                          : SDL_LOGICAL_PRESENTATION_LETTERBOX);
}

// Physical content pixels, excluding bars. GetCurrentRenderOutputSize instead
// returns the logical dimensions when logical presentation is enabled.
inline bool presentation_size(SDL_Renderer* renderer, int* width, int* height) {
    SDL_RendererLogicalPresentation mode = SDL_LOGICAL_PRESENTATION_DISABLED;
    if (!SDL_GetRenderLogicalPresentation(renderer, nullptr, nullptr, &mode))
        return false;
    if (mode == SDL_LOGICAL_PRESENTATION_DISABLED)
        return SDL_GetCurrentRenderOutputSize(renderer, width, height) && *width > 0 && *height > 0;
    SDL_FRect rect{};
    if (!SDL_GetRenderLogicalPresentationRect(renderer, &rect) || rect.w <= 0 || rect.h <= 0)
        return false;
    *width = std::max(1, static_cast<int>(std::ceil(rect.w)));
    *height = std::max(1, static_cast<int>(std::ceil(rect.h)));
    return true;
}
}  // namespace ipod::video
