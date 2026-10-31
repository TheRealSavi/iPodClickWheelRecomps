#pragma once
#include "ipod/platform/video_options.h"
#include "nis.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <vector>

namespace ipod::video {
// Owns all frame textures, but not the renderer. Reset before destroying the
// renderer. Use on its main thread, with a renderer dedicated to this presenter.
class Presenter {
public:
    Presenter() = default;
    ~Presenter() { reset(); }
    Presenter(const Presenter&) = delete;
    Presenter& operator=(const Presenter&) = delete;
    void attach(SDL_Renderer* renderer, int logical_width, int logical_height);
    void reset();
    void set_options(Options options) { options_ = options; }
    Options options() const { return options_; }
    bool nis_active() const { return nis_active_; }
    const char* scaling_description() const;
    // Returns true only when an unmodified, non-repeated F6/F7 changes options.
    bool handle_shortcut(const SDL_Event& event);
    bool present(const uint8_t* rgb, unsigned width, unsigned height);
    // Redraws the last upload before reading, since present invalidates the
    // backbuffer. Includes black borders; caller owns the returned surface.
    SDL_Surface* capture();

private:
    bool render_last();
    bool target_pass(SDL_Texture* source, SDL_Texture* target);
    bool target(SDL_Texture*& texture, int width, int height);
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* frame_ = nullptr;
    SDL_Texture* prescale_ = nullptr;
    SDL_Texture* reduction_[2] = {};
    int logical_width_ = 0, logical_height_ = 0;
    int width_ = 0, height_ = 0;
    Options options_;
    NisScaler nis_;
    bool nis_active_ = false;
    std::vector<uint8_t> upload_;
};
}  // namespace ipod::video
