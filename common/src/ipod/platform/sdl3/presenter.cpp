#include "presenter.h"

#include "presentation.h"

#include <algorithm>
#include <climits>

namespace ipod::video {
void Presenter::attach(SDL_Renderer* renderer, int width, int height) {
    reset();
    renderer_ = renderer;
    logical_width_ = width;
    logical_height_ = height;
}
void Presenter::reset() {
    if (frame_)
        SDL_DestroyTexture(frame_);
    if (prescale_)
        SDL_DestroyTexture(prescale_);
    for (auto*& texture : reduction_) {
        if (texture)
            SDL_DestroyTexture(texture);
        texture = nullptr;
    }
    frame_ = prescale_ = nullptr;
    renderer_ = nullptr;
    width_ = height_ = 0;
}
bool Presenter::handle_shortcut(const SDL_Event& event) {
    if (event.type != SDL_EVENT_KEY_DOWN || event.key.repeat ||
        (event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_SHIFT | SDL_KMOD_GUI)))
        return false;
    if (event.key.scancode == SDL_SCANCODE_F6)
        options_.scaling =
            static_cast<Scaling>((static_cast<unsigned>(options_.scaling) + 1) % SCALING_COUNT);
    else if (event.key.scancode == SDL_SCANCODE_F7)
        options_.pixel_perfect = !options_.pixel_perfect;
    else
        return false;
    return true;
}
bool Presenter::target(SDL_Texture*& texture, int width, int height) {
    if (texture && texture->w == width && texture->h == height)
        return true;
    if (texture)
        SDL_DestroyTexture(texture);
    texture = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_TARGET, width,
                                height);
    return texture && SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
}
bool Presenter::target_pass(SDL_Texture* source, SDL_Texture* destination) {
    if (!SDL_SetRenderTarget(renderer_, destination))
        return false;
    const bool ok =
        SDL_SetRenderLogicalPresentation(renderer_, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED) &&
        SDL_RenderTexture(renderer_, source, nullptr, nullptr);
    const bool restored = SDL_SetRenderTarget(renderer_, nullptr);
    return ok && restored;
}
bool Presenter::present(const uint8_t* rgb, unsigned width, unsigned height) {
    if (!renderer_ || !rgb || !width || !height || width > INT_MAX / 8 || height > INT_MAX / 8)
        return SDL_SetError("Invalid video frame");
    if (!frame_ || width_ != static_cast<int>(width) || height_ != static_cast<int>(height)) {
        if (frame_)
            SDL_DestroyTexture(frame_);
        frame_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING,
                                   static_cast<int>(width), static_cast<int>(height));
        width_ = static_cast<int>(width);
        height_ = static_cast<int>(height);
    }
    return frame_ && SDL_UpdateTexture(frame_, nullptr, rgb, width_ * 3) && render_last() &&
           SDL_RenderPresent(renderer_);
}
bool Presenter::render_last() {
    if (!renderer_ || !frame_ || logical_width_ <= 0 || logical_height_ <= 0)
        return SDL_SetError("No video frame available");
    if (!SDL_SetRenderTarget(renderer_, nullptr) ||
        !configure_presentation(renderer_, logical_width_, logical_height_,
                                options_.pixel_perfect) ||
        !SDL_RenderClear(renderer_))
        return false;
    int output_width = 0, output_height = 0;
    if (!presentation_size(renderer_, &output_width, &output_height))
        return false;
    SDL_Texture* source = frame_;
    if (width_ > output_width || height_ > output_height) {
        // Always filter reductions, including Nearest: retain details from
        // higher-resolution game renderers instead of discarding most samples.
        if (!SDL_SetTextureScaleMode(frame_, SDL_SCALEMODE_LINEAR))
            return false;
        int w = width_, h = height_;
        unsigned step = 0;
        while (w / 2 >= output_width && h / 2 >= output_height) {
            w /= 2;
            h /= 2;
            auto*& reduced = reduction_[step++ % 2];
            if (!target(reduced, w, h) || !target_pass(source, reduced))
                return false;
            source = reduced;
        }
    } else {
        if (!SDL_SetTextureScaleMode(frame_, options_.scaling == Scaling::Smooth
                                                 ? SDL_SCALEMODE_LINEAR
                                                 : SDL_SCALEMODE_NEAREST))
            return false;
        if (options_.scaling == Scaling::Sharp) {
            const int factor = std::clamp(std::max((output_width + width_ - 1) / width_,
                                                   (output_height + height_ - 1) / height_),
                                          1, 8);
            if (factor > 1) {
                if (!target(prescale_, width_ * factor, height_ * factor) ||
                    !target_pass(frame_, prescale_))
                    return false;
                source = prescale_;
            }
        }
    }
    return SDL_RenderTexture(renderer_, source, nullptr, nullptr);
}
SDL_Surface* Presenter::capture() {
    if (!render_last())
        return nullptr;
    // Read the full drawable, not the logical content viewport.
    if (!SDL_SetRenderLogicalPresentation(renderer_, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED))
        return nullptr;
    auto* surface = SDL_RenderReadPixels(renderer_, nullptr);
    configure_presentation(renderer_, logical_width_, logical_height_, options_.pixel_perfect);
    return surface;
}
}  // namespace ipod::video
