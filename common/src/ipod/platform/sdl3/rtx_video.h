#pragma once
#include <SDL3/SDL.h>

#include <memory>

namespace ipod::video {
// Optional NVIDIA RTX Video SDK inference. All calls stay on the render thread.
class RtxVideoScaler {
public:
    RtxVideoScaler();
    ~RtxVideoScaler();
    RtxVideoScaler(const RtxVideoScaler&) = delete;
    RtxVideoScaler& operator=(const RtxVideoScaler&) = delete;
    void reset();
    bool available(SDL_Renderer* renderer) const;
    SDL_Texture* upscale(SDL_Renderer* renderer, SDL_Texture* source, int width, int height);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool failed_ = false;
};
}  // namespace ipod::video
