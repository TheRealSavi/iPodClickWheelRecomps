#pragma once
#include <algorithm>
#include <cstdint>

namespace ipod::video {
struct Rect {
    int x = 0, y = 0, width = 0, height = 0;
};

// Fit the whole frame without stretching or cropping. An odd leftover pixel
// goes on the right/bottom. Zero-sized/minimized outputs have no content area.
inline Rect fit(int source_width, int source_height, int output_width, int output_height) {
    if (source_width <= 0 || source_height <= 0 || output_width <= 0 || output_height <= 0)
        return {};
    int width = output_width, height = output_height;
    if (int64_t(output_width) * source_height <= int64_t(output_height) * source_width)
        height = std::max(1, int(int64_t(output_width) * source_height / source_width));
    else
        width = std::max(1, int(int64_t(output_height) * source_width / source_height));
    return {(output_width - width) / 2, (output_height - height) / 2, width, height};
}
}  // namespace ipod::video
