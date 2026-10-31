#pragma once
#include "runtime/cpu.h"

#include <filesystem>
namespace phase::host {
void initialize(const std::filesystem::path& assets, bool visible);
bool service(Cpu& cpu, const char* framework, unsigned ordinal);
void begin_frame(uint32_t context);
bool pump();
void screenshot(const std::filesystem::path& path);
void close();
}  // namespace phase::host
