// Authored service-contract fixtures. No original executable or assets required.
#include "host.h"
#include "ipod_eapp.h"
#include "keyboard.h"
#include "runtime/memory.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace {
using namespace ipod;
constexpr uint32_t context = 0x19000000, request = 0x19001000, object = 0x19002000;
constexpr uint32_t path = 0x19003000, destination = 0x19004000;
void require(bool condition, const char* description) {
    if (!condition)
        throw std::runtime_error(description);
}
void name(const char* value) {
    std::memcpy(guest_pointer(path, uint32_t(std::strlen(value) + 1)), value,
                std::strlen(value) + 1);
}
uint32_t call(const char* framework, unsigned ordinal, std::array<uint32_t, 4> args) {
    Cpu cpu{};
    std::copy(args.begin(), args.end(), cpu.r);
    require(phase::host::service(cpu, framework, ordinal), "fixture service unsupported");
    return cpu.r[0];
}
void fresh_request(unsigned operation) {
    std::memset(guest_pointer(request, 0x3c), 0, 0x3c);
    st32(request + 8, 0x12345678);
    st32(request + 0xc, 0x98765432);
    st8(request + 0x10, uint8_t(operation));
    st32(request + 0x14, object);
}
void deliver(uint32_t expected = request) {
    phase::host::begin_frame(context);
    require(ld32(context + 0x2c) == expected, "completion not delivered at frame boundary");
    require(ld32(request + 8) == 0x12345678 && ld32(request + 0xc) == 0x98765432,
            "callback or cookie changed");
    phase::host::begin_frame(context);
    require(ld32(context + 0x2c) == 0, "completion delivered twice");
}
}  // namespace

int test_host(const std::filesystem::path& assets) {
    Cpu allocation{};
    allocation.r[0] = 32;
    phase::eapp::framework_call(allocation, "MemoryAlloc", 0, nullptr);
    const auto arena = allocation.r[0];
    phase::eapp::framework_call(allocation, "MemoryAlloc", 1, nullptr);
    allocation.r[0] = arena;
    bool double_free_denied = false;
    try {
        phase::eapp::framework_call(allocation, "MemoryAlloc", 1, nullptr);
    } catch (const std::exception&) {
        double_free_denied = true;
    }
    require(double_free_denied, "SDK arena double free accepted");
    phase::input::Keyboard keyboard;
    constexpr std::array<unsigned, 8> ring{0x18, 0x19, 0x27, 0x35, 0x34, 0x33, 0x25, 0x17};
    for (unsigned i = 0; i < 8; ++i) {
        keyboard.key(ring[i], true);
        keyboard.advance();
        require(keyboard.wheel() == (0x40000000u | ((64 - i * 32) & 255)), "single wheel position");
        keyboard.key(ring[(i + 1) % 8], true);
        for (unsigned frame = 0; frame < 4; ++frame)
            keyboard.advance();
        require(keyboard.wheel() == (0x40000000u | ((64 - i * 32 - 16) & 255)),
                "circular chord midpoint");
        require(keyboard.take_buttons().empty(), "touch key generated button");
        keyboard.release_all();
        keyboard.advance();
        require(!(keyboard.wheel() & 0x40000000u), "finger not lifted");
    }
    keyboard.key(0x18, true);
    keyboard.key(0x34, true);
    keyboard.advance();
    require(!(keyboard.wheel() & 0x40000000u), "opposite keys produced arbitrary angle");
    keyboard.release_all();
    keyboard.key(0x1c, true);
    keyboard.key(0x1c, true);
    keyboard.release_all();
    auto buttons = keyboard.take_buttons();
    require(buttons.size() == 2 && buttons[0].button == 2 && buttons[0].down && !buttons[1].down,
            "button repeat/release");
    keyboard.key(0x1f, true);
    keyboard.key(0x1c, true);
    keyboard.key(0x1f, false);
    buttons = keyboard.take_buttons();
    require(buttons.size() == 1 && buttons[0].down,
            "Select aliases generated duplicate or premature release");
    keyboard.key(0x1c, false);
    buttons = keyboard.take_buttons();
    require(buttons.size() == 1 && !buttons[0].down, "last Select alias did not release");
    phase::input::Keyboard sweep;
    sweep.key(0x18, true);
    sweep.advance();
    sweep.key(0x19, true);
    sweep.advance();
    require((sweep.wheel() & 255) == 60, "first intermediate missing");
    sweep.advance();
    require((sweep.wheel() & 255) == 56, "second intermediate missing");
    sweep.key(0x19, false);
    sweep.advance();
    require((sweep.wheel() & 255) == 60, "direction reversal overshot");
    sweep.release_all();
    sweep.advance();
    require(!(sweep.wheel() & 0x40000000u), "released sweep kept moving");
    sweep.key(0x27, true);
    sweep.advance();
    require((sweep.wheel() & 255) == 0, "reattach did not start at target");
    sweep.key(0x35, true);
    sweep.advance();
    require((sweep.wheel() & 255) == 252, "wraparound took long route");
    constexpr std::array<unsigned, 5> button_keys{0x11, 0x1f, 0x1e, 0x20, 0x2d};
    for (unsigned i = 0; i < button_keys.size(); ++i) {
        keyboard.key(button_keys[i], true);
        keyboard.key(button_keys[i], false);
        const auto events = keyboard.take_buttons();
        require(events.size() == 2 && events[0].button == i + 1 && events[1].button == i + 1 &&
                    events[0].down && !events[1].down,
                "button identity or edges");
    }
    phase::host::initialize(assets, false);
    for (uint32_t repeat = 0; repeat <= 2; ++repeat) {
        call("Audio", 1, {repeat, 0, 0, 0});
        require(call("Audio", 41, {}) == (repeat == 1) && call("Audio", 42, {}) == (repeat == 2) &&
                    call("Audio", 33, {}) == 0,
                "repeat flags confused with playback status");
    }
    call("Audio", 3, {1, 0, 0, 0});
    require(call("Audio", 40, {}) == 1, "shuffle enable/readback");
    call("Audio", 3, {2, 0, 0, 0});
    require(call("Audio", 40, {}) == 0, "shuffle restore/readback");
    call("Audio", 1, {});
    name("fixture.bin");
    fresh_request(1);
    require(call("AsyncFileIO", 0, {0, path, 1, request}) == 1, "open not accepted");
    require(ld32(context + 0x2c) == 0, "completion delivered synchronously");
    require(ld32(request + 0x28) == 0 && ld32(object + 8) == 6, "open size/status");
    const auto handle = ld32(request + 0x34);
    deliver();
    st8(request + 0x10, 3);
    st32(request + 0x20, destination);
    st32(request + 0x24, 4);
    call("AsyncFileIO", 2, {request, 0, 0, 0});
    deliver();
    require(std::memcmp(guest_pointer(destination, 4), "abcdef", 4) == 0, "read bytes");
    require(ld32(request + 0x2c) == 4 && ld32(request + 0x30) == 4, "read count/position");
    st8(request + 0x10, 5);
    st32(request + 0x18, 0xfffffffe);
    st8(request + 0x1c, 2);
    call("AsyncFileIO", 2, {request, 0, 0, 0});
    deliver();
    require(ld32(request + 0x30) == 4, "seek from end");
    st8(request + 0x10, 3);
    call("AsyncFileIO", 2, {request, 0, 0, 0});
    deliver();
    require(ld32(request + 0x2c) == 2 && std::memcmp(guest_pointer(destination, 2), "ef", 2) == 0,
            "short read at EOF");
    call("AsyncFileIO", 2, {request, 0, 0, 0});
    deliver();
    require(ld32(request + 0x2c) == 0, "EOF count");
    st8(request + 0x10, 4);
    bool denied = false;
    try {
        call("AsyncFileIO", 2, {request, 0, 0, 0});
    } catch (const std::exception&) {
        denied = true;
    }
    require(denied, "write request accepted");
    st8(request + 0x10, 2);
    call("AsyncFileIO", 1, {request, 0, 0, 0});
    deliver();
    require(ld32(request + 0x34) == handle, "close corrupted handle");
    name("absent.bin");
    fresh_request(1);
    call("AsyncFileIO", 0, {0, path, 1, request});
    require(ld32(request + 0x28) == 0xffffffff, "missing file not failed");
    deliver();
    for (const auto* invalid : {"../outside.bin", "C:/outside.bin", "fixture.bin:stream"}) {
        name(invalid);
        fresh_request(1);
        denied = false;
        try {
            call("AsyncFileIO", 0, {0, path, 1, request});
        } catch (const std::exception&) {
            denied = true;
        }
        require(denied, "unsafe asset path accepted");
    }
    name("absent.bin");
    fresh_request(1);
    call("AsyncFileIO", 0, {0, path, 1, request});
    std::memcpy(guest_pointer(request + 0x100, 0x3c), guest_pointer(request, 0x3c), 0x3c);
    call("AsyncFileIO", 0, {0, path, 1, request + 0x100});
    phase::host::begin_frame(context);
    require(ld32(context + 0x2c) == request && ld32(request) == request + 0x100 &&
                ld32(request + 0x100) == 0,
            "completion order");
    phase::host::begin_frame(context);

    phase::host::key(0x11, true);
    phase::host::key(0x11, false);
    phase::host::begin_frame(context);
    auto node = ld32(context + 0x30);
    require(ld8(node) == 1 && ld8(node + 1) == 2, "Menu press node");
    node = ld32(node + 8);
    require(ld8(node) == 1 && ld8(node + 1) == 1 && !ld32(node + 8), "Menu release node");
    call("InputEvents", 1, {node, 0, 0, 0});
    phase::host::begin_frame(context);
    require(!ld32(context + 0x30), "input repeated next frame");
    phase::host::key(0x18, true);
    phase::host::key(0x19, true);
    phase::host::begin_frame(context);
    call("InputEvents", 0, {destination, destination + 4, 0, 0});
    require(ld32(destination + 4) == 0x40000030, "wheel poll midpoint");
    phase::host::key(0x18, false);
    phase::host::key(0x19, false);

    require(call("Users", 0, {0, 0, 0, 0}) == 0, "session started with profiles");
    name("Tester");
    const auto profile = call("Users", 4, {path, 0, 0, 0});
    require(profile && call("Users", 0, {0, 0, 0, 0}) == 1, "profile creation");
    const auto id = call("Users", 8, {profile, 0, 0, 0});
    require(call("Users", 1, {0, 0, 0, 0}) == profile && call("Users", 2, {id, 0, 0, 0}) == profile,
            "profile lookup");
    require(std::strcmp(
                reinterpret_cast<char*>(guest_pointer(call("Users", 5, {profile, 0, 0, 0}), 32)),
                "Tester") == 0,
            "profile name");
    name("Renamed");
    call("Users", 9, {profile, path, 0, 0});
    require(std::strcmp(reinterpret_cast<char*>(guest_pointer(profile, 32)), "Renamed") == 0 &&
                call("Users", 8, {profile, 0, 0, 0}) == id,
            "rename changed identity or failed to replace name");
    name("Second");
    const auto second = call("Users", 4, {path, 0, 0, 0});
    const auto second_id = call("Users", 8, {second, 0, 0, 0});
    require(call("Users", 3, {profile, 0, 0, 0}) == 1 &&
                call("Users", 3, {profile, 0, 0, 0}) == 0 && call("Users", 2, {id, 0, 0, 0}) == 0 &&
                call("Users", 1, {0, 0, 0, 0}) == second &&
                call("Users", 8, {second, 0, 0, 0}) == second_id,
            "delete renumbered or invalidated another profile");
    name("Third");
    const auto third = call("Users", 4, {path, 0, 0, 0});
    require(call("Users", 8, {third, 0, 0, 0}) > second_id,
            "new profile inherited a deleted identity");
    for (const auto [input, expected] : std::array<std::pair<uint32_t, uint32_t>, 4>{
             {{42, 42}, {101, 100}, {0xffffffff, 0}, {100, 100}}}) {
        require(call("miscTBD", 1, {input, 0, 0, 0}) == 0 && call("miscTBD", 2, {}) == expected,
                "brightness percentage/clamp/readback");
    }
    name("save_fixture");
    require(call("AsyncFileIO", 12, {1, path, destination, 0}) == 0, "save open");
    const auto slot = ld32(destination);
    std::memcpy(guest_pointer(destination + 16, 6), "saved!", 6);
    require(call("AsyncFileIO", 16, {slot, destination + 16, 6, 0}) == 0, "save write");
    require(call("AsyncFileIO", 14, {slot, 0, 0, 0}) == 0, "save close");
    require(call("AsyncFileIO", 16, {slot, destination + 16, 6, 0}) == 8,
            "closed save handle accepted");
    fresh_request(1);
    call("AsyncFileIO", 0, {1, path, 0, request});
    deliver();
    st8(request + 0x10, 3);
    st32(request + 0x20, destination);
    st32(request + 0x24, 6);
    call("AsyncFileIO", 2, {request, 0, 0, 0});
    deliver();
    require(std::memcmp(guest_pointer(destination, 6), "saved!", 6) == 0, "session save readback");
    name("en/stats");
    require(call("AsyncFileIO", 12, {2, path, destination, 0}) == 0, "stats open");
    auto stats_slot = ld32(destination);
    std::memcpy(guest_pointer(destination + 16, 5), "stats", 5);
    call("AsyncFileIO", 16, {stats_slot, destination + 16, 5, 0});
    call("AsyncFileIO", 14, {stats_slot, 0, 0, 0});
    fresh_request(1);
    st32(request + 0x20, destination);
    st32(request + 0x24, 5);
    call("AsyncFileIO", 0, {2, path, 0, request});
    deliver();
    require(ld32(request + 0x28) == 0 &&
                std::memcmp(guest_pointer(destination, 5), "stats", 5) == 0,
            "stats namespace readback");
    require(call("Metadata", 45, {0, 0, 0, 0}) == 0 && call("Metadata", 61, {0, 0, 0, 0}) == 0,
            "empty library contract");

    call("OpenGLES", 18, {0x10000, 0x8000, 0, 0x4000});
    call("OpenGLES", 56, {0xb00, destination, 0, 0});
    std::array<uint32_t, 4> color{};
    for (unsigned i = 0; i < 4; ++i)
        color[i] = ld32(destination + 4 * i);
    require((color[0] >> 15) == 0xffff && color[2] == 0 && (color[3] >> 15) == 0x3fff,
            "normalized integer color query");
    call("OpenGLES", 36, {0x0de1, 0, 0, 0});
    call("OpenGLES", 36, {0x0be2, 0, 0, 0});
    call("OpenGLES", 127, {0x2300, 0x2200, 0x1e01, 0});
    const auto buffer = ld32(context + 0x38);
    st16(buffer, 0xf800);
    require(call("OpenGLES", 165, {1, 1, 0, 0}) == 1, "presentation failed");
    require(call("OpenGLES", 71, {0x0de1, 0, 0, 0}) == 1 &&
                call("OpenGLES", 71, {0x0be2, 0, 0, 0}) == 1,
            "presentation altered enable state");
    call("OpenGLES", 56, {0xb00, destination, 0, 0});
    for (unsigned i = 0; i < 4; ++i)
        require(ld32(destination + 4 * i) == color[i], "presentation altered color");
    call("OpenGLES", 63, {0x2300, 0x2200, destination, 0});
    require(ld32(destination) == 0x1e01, "presentation altered texture mode");
    Cpu cpu{};
    require(!phase::host::service(cpu, "OpenGLES", 129), "unverified GPU upload accepted");
    phase::host::close();
    std::puts(
        "PASS: asset IO, queued completions, path containment, integer color, presentation state");
    return 0;
}
