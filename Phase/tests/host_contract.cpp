// Authored service-contract fixtures. No original executable or assets required.
#include "host.h"
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
    phase::host::initialize(assets, false);
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
