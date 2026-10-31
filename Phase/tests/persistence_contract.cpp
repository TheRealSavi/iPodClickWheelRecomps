// Authored cross-process fixtures; opaque record bytes are not game scores.
#define NOMINMAX
#include "persistence.h"

#include <cstdio>
#include <stdexcept>
#include <windows.h>

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
}  // namespace
int test_persistence(const std::filesystem::path& directory, const std::string& mode) {
    using phase::persistence::Store;
    Store store;
    if (mode == "overlap") {
        store.open(directory / "forbidden", {directory});
        throw std::runtime_error("overlapping directory accepted");
    }
    store.open(directory);
    if (mode == "seed") {
        require(store.state().profiles.empty(), "fixture not empty");
        store.add_profile("Alice");
        store.add_profile("Bob");
        store.write("save_1.dtb", {0, 255, 3, 17});
        store.write("save_2.dtb", {6, 5, 4});
    } else if (mode == "verify" || mode == "verify-backup") {
        require(store.state().profiles == std::vector<std::string>{"Alice", "Bob"},
                "profiles/IDs did not survive restart");
        require(store.state().records.at({1, "save_1.dtb"}) == std::vector<uint8_t>{0, 255, 3, 17},
                "opaque record changed on restart");
        require(store.state().records.count({1, "save_2.dtb"}) == (mode == "verify" ? 1 : 0),
                "wrong snapshot generation");
    } else if (mode == "update") {
        store.write("save_2.dtb", {6, 5, 4});
    } else if (mode == "failed-write") {
        auto handle = CreateFileW((directory / "state.bin").c_str(), GENERIC_READ, FILE_SHARE_READ,
                                  nullptr, OPEN_EXISTING, 0, nullptr);
        require(handle != INVALID_HANDLE_VALUE, "could not obstruct replacement");
        bool denied = false;
        try {
            store.write("save_1.dtb", {9});
        } catch (const std::exception&) {
            denied = true;
        }
        CloseHandle(handle);
        require(denied && store.state().records.at({1, "save_1.dtb"}) ==
                              std::vector<uint8_t>{0, 255, 3, 17},
                "failed commit acknowledged or changed the in-memory state");
    } else if (mode == "hold") {
        std::puts("LOCKED");
        std::fflush(stdout);
        std::getchar();
    } else if (mode == "limits") {
        for (const auto& name : {"", "../escape", "C:stream", ".", ".."}) {
            bool denied = false;
            try {
                store.write(name, {});
            } catch (const std::exception&) {
                denied = true;
            }
            require(denied, "unsafe record name accepted");
        }
        bool denied = false;
        try {
            store.write("too_big", std::vector<uint8_t>(0x19001));
        } catch (const std::exception&) {
            denied = true;
        }
        require(denied && store.state().records.empty(), "oversized write changed state");
        for (unsigned i = 0; i < 5; ++i)
            store.add_profile(std::to_string(i));
        denied = false;
        try {
            store.add_profile("sixth");
        } catch (const std::exception&) {
            denied = true;
        }
        require(denied && store.state().profiles.size() == 5, "profile cap changed state");
    } else if (mode == "namespaces") {
        store.write("same", {1}, 1);
        store.write("same", {2}, 2);
        store.write("en/stats", {3}, 2);
    } else if (mode == "verify-namespaces") {
        require(store.state().records.at({1, "same"}) == std::vector<uint8_t>{1} &&
                    store.state().records.at({2, "same"}) == std::vector<uint8_t>{2} &&
                    store.state().records.at({2, "en/stats"}) == std::vector<uint8_t>{3},
                "resource kinds collided on restart");
    } else
        throw std::runtime_error("unknown persistence fixture mode");
    std::puts("PASS: persistence contract");
    return 0;
}
