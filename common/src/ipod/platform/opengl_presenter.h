#pragma once
#include <cstdint>
#include <vector>

namespace ipod::video {
// RGB24 presentation through double-buffered desktop OpenGL 1.1. Call on the context's thread;
// release before destroying that context. Does not own the window or swap it.
class OpenGLPresenter {
public:
    OpenGLPresenter() = default;
    OpenGLPresenter(const OpenGLPresenter&) = delete;
    OpenGLPresenter& operator=(const OpenGLPresenter&) = delete;
    void present(const uint8_t* rgb, int width, int height, int output_width, int output_height);
    void release();

private:
    unsigned texture_ = 0;
    int width_ = 0, height_ = 0, texture_width_ = 0, texture_height_ = 0;
    std::vector<uint8_t> padded_;
};
}  // namespace ipod::video
