#pragma once
#include "runtime/cpu.h"

#include <filesystem>
namespace phase::audio {
void initialize(bool enabled);
void close();
bool service(Cpu& cpu, const char* framework, unsigned ordinal);
void test(const std::filesystem::path& file);
}  // namespace phase::audio
