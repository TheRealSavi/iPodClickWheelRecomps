#pragma once
#include "runtime/cpu.h"

#include <filesystem>
namespace phase::host {
void initialize(const std::filesystem::path& assets, bool visible, bool audio = false,
                const std::filesystem::path& library = {},
                const std::filesystem::path& save_directory = {});
bool service(Cpu& cpu, const char* framework, unsigned ordinal);
void begin_frame(uint32_t context);
bool pump();
void key(unsigned scan, bool down);
void screenshot(const std::filesystem::path& path);
void close();
}  // namespace phase::host
