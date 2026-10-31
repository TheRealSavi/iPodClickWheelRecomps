#pragma once

namespace ipod::video {
enum class Scaling { Sharp, Nearest, Smooth, NvidiaNis };
constexpr unsigned SCALING_COUNT = 4;
constexpr const char* scaling_label(Scaling scaling) {
    switch (scaling) {
    case Scaling::Nearest:
        return "Nearest";
    case Scaling::Smooth:
        return "Smooth";
    case Scaling::NvidiaNis:
        return "NVIDIA NIS";
    default:
        return "Sharp";
    }
}
struct Options {
    Scaling scaling = Scaling::Sharp;
    bool pixel_perfect = false;
};
}  // namespace ipod::video
