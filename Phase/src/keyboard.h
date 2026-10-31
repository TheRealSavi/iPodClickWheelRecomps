#pragma once
#include <array>
#include <cstdint>
#include <vector>

namespace phase::input {
struct ButtonEvent {
    uint8_t button;
    bool down;
};
// PC set-1 scan codes keep the requested physical key circle independent of Shift.
class Keyboard {
public:
    void key(unsigned scan, bool down);
    void release_all();
    void advance();  // one 60 Hz game tick; at most 5.625 degrees per tick
    uint32_t wheel() const;
    std::vector<ButtonEvent> take_buttons();

private:
    std::array<bool, 128> held_{};
    std::vector<ButtonEvent> buttons_;
    uint8_t angle_ = 0;
    bool touching_ = false;
};
}  // namespace phase::input
