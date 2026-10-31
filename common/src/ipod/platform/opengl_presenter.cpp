#include "opengl_presenter.h"

#include "video.h"

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif
#if defined(__APPLE__)
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif
#include <cstring>
#include <stdexcept>

namespace ipod::video {
void OpenGLPresenter::present(const uint8_t* rgb, int width, int height, int output_width,
                              int output_height) {
    const auto rect = fit(width, height, output_width, output_height);
    if (!rect.width || !rect.height)
        return;
    if (!rgb || width > 4094 || height > 4094)
        throw std::runtime_error("invalid video frame");
    // Power-of-two storage works even on Windows' OpenGL 1.1 fallback. A copied
    // edge around the image prevents linear filtering from sampling padding.
    const bool resized = width != width_ || height != height_;
    if (resized) {
        width_ = width;
        height_ = height;
        texture_width_ = texture_height_ = 1;
        while (texture_width_ < width + 2)
            texture_width_ *= 2;
        while (texture_height_ < height + 2)
            texture_height_ *= 2;
        padded_.resize(size_t(width + 2) * size_t(height + 2) * 3);
    }
    const size_t stride = size_t(width + 2) * 3;
    for (int y = 0; y < height + 2; ++y) {
        const auto* row = rgb + size_t(std::clamp(y - 1, 0, height - 1)) * size_t(width) * 3;
        auto* out = padded_.data() + size_t(y) * stride;
        std::memcpy(out, row, 3);
        std::memcpy(out + 3, row, size_t(width) * 3);
        std::memcpy(out + size_t(width + 1) * 3, row + size_t(width - 1) * 3, 3);
    }
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
    glDrawBuffer(GL_BACK);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);  // Clear the entire drawable, including old bars.
    glViewport(rect.x, output_height - rect.y - rect.height, rect.width, rect.height);
    for (auto capability :
         {GL_BLEND, GL_DEPTH_TEST, GL_STENCIL_TEST, GL_ALPHA_TEST, GL_CULL_FACE, GL_LIGHTING,
          GL_FOG, GL_TEXTURE_GEN_S, GL_TEXTURE_GEN_T, GL_COLOR_LOGIC_OP, GL_POLYGON_STIPPLE})
        glDisable(GLenum(capability));
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    if (!texture_)
        glGenTextures(1, &texture_);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    if (resized)
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, texture_width_, texture_height_, 0, GL_RGB,
                     GL_UNSIGNED_BYTE, nullptr);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width + 2, height + 2, GL_RGB, GL_UNSIGNED_BYTE,
                    padded_.data());
    glMatrixMode(GL_TEXTURE);
    glPushMatrix();
    glLoadIdentity();
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, 1, 1, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    const float left = 1.0f / float(texture_width_), top = 1.0f / float(texture_height_);
    const float right = float(width + 1) / float(texture_width_);
    const float bottom = float(height + 1) / float(texture_height_);
    glBegin(GL_QUADS);
    glTexCoord2f(left, top);
    glVertex2f(0, 0);
    glTexCoord2f(right, top);
    glVertex2f(1, 0);
    glTexCoord2f(right, bottom);
    glVertex2f(1, 1);
    glTexCoord2f(left, bottom);
    glVertex2f(0, 1);
    glEnd();
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_TEXTURE);
    glPopMatrix();
    glPopClientAttrib();
    glPopAttrib();
}

void OpenGLPresenter::release() {
    if (texture_)
        glDeleteTextures(1, &texture_);
    texture_ = 0;
    width_ = height_ = texture_width_ = texture_height_ = 0;
    padded_.clear();
}
}  // namespace ipod::video
