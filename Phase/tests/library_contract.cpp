#include "library.h"
#include "runtime/memory.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>

int test_library(const std::filesystem::path& root) {
    using namespace ipod;
    auto require = [](bool value, const char* message) {
        if (!value)
            throw std::runtime_error(message);
    };
    Cpu cpu{};
    auto call = [&](unsigned ordinal, std::array<uint32_t, 4> args) {
        std::copy(args.begin(), args.end(), cpu.r);
        require(phase::library::service(cpu, ordinal), "unsupported metadata fixture call");
        return cpu.r[0];
    };
    phase::library::open(root);
    require(call(61, {0}) == 1, "user playlist count");
    auto playlist = call(89, {0, 0});
    call(162, {playlist});
    require(cpu.r[0] == 0x12345678 && cpu.r[1] == 0x87654321, "64-bit playlist ID");
    call(48, {0, playlist});
    call(57, {0, 1, 0x7e});
    require(call(63, {0}) == 3, "filter or duplicate occurrence lost");
    auto a = call(78, {0, 0}), b = call(78, {0, 1}), duplicate = call(78, {0, 2});
    require(a != b && a == duplicate, "playlist order changed");
    call(97, {a});
    require(cpu.r[0] == 0x23456789 && cpu.r[1] == 0xabcdef01, "64-bit track ID");
    require(call(119, {a}) == 169 && call(120, {a}) == 169733, "duration units");
    constexpr uint32_t output = 0x19004000, length = 0x19005000;
    st32(length, 128);
    call(101, {a, output, length});
    require(std::string(reinterpret_cast<char*>(guest_pointer(output, 128))) == "Caf\xc3\xa9",
            "UTF-16 metadata conversion");
    st32(length, 5);
    call(101, {a, output, length});
    require(ld32(length) == 3 && ld8(output + 3) == 0, "bounded UTF-8 output");
    st32(length, 512);
    call(100, {a, output, length});
    require(std::filesystem::u8path(reinterpret_cast<char*>(guest_pointer(output, 512))) ==
                std::filesystem::canonical(root) / "iPod_Control/Music/F00/A.wav",
            "media path");
    require(phase::library::data_path("Phase Data/Phase Library") ==
                std::filesystem::canonical(root) /
                    "iPod_Control/GameData_RW/1D000/Phase Data/Phase Library",
            "chart path");
    bool refused = false;
    try {
        phase::library::data_path("../../../../outside");
    } catch (const std::exception&) {
        refused = true;
    }
    require(refused, "parent path accepted");
    require(call(58, {0}) == 1 && call(59, {0}) == 2 && call(60, {0}) == 1,
            "group counts must use filtered music");
    auto artist = call(80, {0, 0});
    st32(length, 128);
    call(44, {artist, output, length});
    require(std::string(reinterpret_cast<char*>(guest_pointer(output, 128))) == "Alpha",
            "artist group name");
    call(49, {0, artist});
    require(call(63, {0}) == 2, "artist filter lost duplicate membership");
    call(47, {0});
    call(48, {0, playlist});
    call(72, {0});
    require(call(78, {0, 0}) == b, "title sorting");
    call(73, {0});
    require(call(78, {0, 0}) == a, "playlist ordering not restored");
    phase::library::close();
    std::puts("PASS library: iTunesDB, membership, IDs, strings, durations, containment");
    return 0;
}
