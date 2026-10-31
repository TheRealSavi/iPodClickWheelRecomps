#pragma once
#include "runtime/cpu.h"

#include <filesystem>
#include <string>

namespace phase::library {
// Read-only, uncompressed iTunesDB subset used by Phase's SDK music browser.
void open(const std::filesystem::path& root);
void close();
bool service(Cpu& cpu, unsigned ordinal);
std::filesystem::path data_path(const std::string& relative);
std::filesystem::path contained(const std::filesystem::path& root, std::string relative);
bool mounted();
}  // namespace phase::library
