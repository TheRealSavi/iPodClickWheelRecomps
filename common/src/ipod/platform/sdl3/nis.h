#pragma once
#include <SDL3/SDL.h>

#include <memory>

namespace ipod::video {
// Optional D3D11 compute pass. SDL still owns window presentation and borders.
class NisScaler {
public:
    NisScaler();
    ~NisScaler();
    NisScaler(const NisScaler&) = delete;
    NisScaler& operator=(const NisScaler&) = delete;
    void reset();
    bool available(SDL_Renderer* renderer) const;
    SDL_Texture* upscale(SDL_Renderer* renderer, SDL_Texture* source, int width, int height);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool failed_ = false;
};
}  // namespace ipod::video
