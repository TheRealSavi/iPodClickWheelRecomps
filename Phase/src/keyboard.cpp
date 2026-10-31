#include "keyboard.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace phase::input {
void Keyboard::key(unsigned scan, bool down) {
    if (scan >= held_.size() || held_[scan] == down)
        return;
    held_[scan] = down;
    uint8_t button = 0;
    switch (scan) {
    case 0x11:
        button = 1;
        break;  // W: Menu
    case 0x1e:
        button = 4;
        break;  // A: Previous
    case 0x1f:
        button = 2;
        break;  // S: Select
    case 0x2d:
        button = 5;
        break;  // X: Play/Pause
    case 0x20:
        button = 3;
        break;  // D: Next
    case 0x1c:
        button = 2;
        break;  // Enter: Select
    }
    // S and Enter are two physical bindings for one logical Select button.
    if (button == 2 && held_[scan == 0x1f ? 0x1c : 0x1f])
        return;
    if (button)
        buttons_.push_back({button, down});
}
void Keyboard::release_all() {
    for (unsigned i = 0; i < held_.size(); ++i)
        key(i, false);
}
void Keyboard::advance() {
    constexpr std::array<unsigned, 8> ring{0x18, 0x19, 0x27, 0x35, 0x34, 0x33, 0x25, 0x17};
    constexpr double pi = 3.14159265358979323846;
    double x = 0, y = 0;
    for (unsigned i = 0; i < ring.size(); ++i)
        if (held_[ring[i]]) {
            x += std::cos(i * pi / 4);
            y += std::sin(i * pi / 4);
        }
    // Opposite/symmetric chords have no unique angle: lift rather than jump.
    if (x * x + y * y < 1e-10) {
        touching_ = false;
        return;
    }
    // Phase 18005764 converts the SDK byte into (cos(angle), sin(angle)).
    // Its Cartesian wheel is counterclockwise from right: top=64, right=0.
    const auto target = uint8_t((64 - std::lround(std::atan2(y, x) * 128 / pi)) & 255);
    if (!touching_)
        angle_ = target;
    else {
        const int delta = ((int(target) - int(angle_) + 384) % 256) - 128;
        angle_ = uint8_t((int(angle_) + std::clamp(delta, -4, 4) + 256) % 256);
    }
    touching_ = true;
}
uint32_t Keyboard::wheel() const {
    return angle_ | (touching_ ? 0x40000000u : 0);
}
std::vector<ButtonEvent> Keyboard::take_buttons() {
    return std::exchange(buttons_, {});
}
}  // namespace phase::input
