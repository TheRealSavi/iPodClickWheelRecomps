#define NOMINMAX
#include "persistence.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <windows.h>

namespace phase::persistence {
bool valid_name(uint32_t kind, const std::string& name) {
    if ((kind != 1 && kind != 2) || name.empty() || name.size() > 255 ||
        name.find_first_of("\\:") != std::string::npos || name.find('\0') != std::string::npos ||
        (kind == 1 && name.find('/') != std::string::npos))
        return false;
    size_t first = 0;
    do {
        const auto end = name.find('/', first);
        const auto part = name.substr(first, end == std::string::npos ? end : end - first);
        if (part.empty() || part == "." || part == "..")
            return false;
        if (end == std::string::npos)
            return true;
        first = end + 1;
    } while (true);
}
namespace {
constexpr size_t maximum_snapshot = 4 * 1024 * 1024;
constexpr char magic[] = "PHSAVE01";
struct UnsupportedVersion : std::runtime_error {
    UnsupportedVersion() : std::runtime_error("Phase save: unsupported snapshot version") {}
};
[[noreturn]] void fail(const char* message) {
    throw std::runtime_error(std::string("Phase save: ") + message);
}
void check_file(const std::filesystem::path& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
        fail("save file must be a regular file, not a link or directory");
}
bool inside(const std::filesystem::path& child, const std::filesystem::path& parent) {
    auto c = child.begin();
    for (auto p = parent.begin(); p != parent.end(); ++p, ++c)
        if (c == child.end() || _wcsicmp(c->c_str(), p->c_str()) != 0)
            return false;
    return true;
}
void number(std::vector<uint8_t>& out, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        out.push_back(uint8_t(value >> (i * 8)));
}
uint32_t checksum(const std::vector<uint8_t>& bytes, size_t size) {
    uint32_t crc = 0xffffffff;
    for (size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}
void string(std::vector<uint8_t>& out, const std::string& value) {
    number(out, uint32_t(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}
void validate(const State& state) {
    if (state.profiles.size() > 5 || state.records.size() > 32)
        fail("profile or record limit exceeded");
    for (const auto& name : state.profiles)
        if (name.empty() || name.size() > 31 || name.find('\0') != std::string::npos)
            fail("invalid profile name");
    for (const auto& [key, bytes] : state.records) {
        const auto& [kind, name] = key;
        if (!valid_name(kind, name) || bytes.size() > 0x19000)
            fail("invalid save record");
    }
}
std::vector<uint8_t> encode(const State& state) {
    validate(state);
    std::vector<uint8_t> out(magic, magic + 8);
    number(out, 1);  // Host snapshot version, independent of the native save version.
    number(out, uint32_t(state.profiles.size()));
    for (const auto& name : state.profiles)
        string(out, name);
    number(out, uint32_t(state.records.size()));
    for (const auto& [key, bytes] : state.records) {
        number(out, key.first);
        string(out, key.second);
        number(out, uint32_t(bytes.size()));
        out.insert(out.end(), bytes.begin(), bytes.end());
    }
    number(out, checksum(out, out.size()));
    if (out.size() > maximum_snapshot)
        fail("snapshot too large");
    return out;
}
State read(const std::filesystem::path& path) {
    check_file(path);
    auto size = std::filesystem::file_size(path);
    if (size < 24 || size > maximum_snapshot)
        fail("invalid snapshot size");
    std::ifstream input(path, std::ios::binary);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(size));
    if (!input || input.peek() != std::char_traits<char>::eof())
        fail("cannot read snapshot");
    if (bytes.size() != size || !std::equal(magic, magic + 8, bytes.begin()))
        fail("invalid snapshot header");
    size_t offset = 8;
    auto word = [&]() {
        if (offset + 4 > bytes.size())
            fail("truncated snapshot");
        uint32_t value = 0;
        for (unsigned i = 0; i < 4; ++i)
            value |= uint32_t(bytes[offset++]) << (i * 8);
        return value;
    };
    auto text = [&](unsigned limit) {
        auto length = word();
        if (length > limit || offset + length > bytes.size() - 4)
            fail("invalid snapshot string");
        std::string value(bytes.begin() + offset, bytes.begin() + offset + length);
        offset += length;
        return value;
    };
    if (word() != 1)
        throw UnsupportedVersion();
    State state;
    auto count = word();
    if (count > 5)
        fail("too many profiles");
    for (unsigned i = 0; i < count; ++i)
        state.profiles.push_back(text(31));
    count = word();
    if (count > 32)
        fail("too many records");
    for (unsigned i = 0; i < count; ++i) {
        auto kind = word();
        auto name = text(255);
        auto length = word();
        if (length > 0x19000 || offset + length > bytes.size() - 4)
            fail("invalid record length");
        std::vector<uint8_t> data(bytes.begin() + offset, bytes.begin() + offset + length);
        offset += length;
        if (!state.records.emplace(RecordKey{kind, name}, std::move(data)).second)
            fail("duplicate save record");
    }
    if (offset != bytes.size() - 4 || word() != checksum(bytes, bytes.size() - 4))
        fail("snapshot checksum mismatch");
    validate(state);
    return state;
}
}  // namespace
Store::~Store() {
    close();
}
void Store::close() {
    if (lock_)
        CloseHandle(lock_);
    lock_ = nullptr;
    directory_.clear();
    state_ = {};
    recovered_ = false;
}
void Store::open(const std::filesystem::path& directory,
                 const std::vector<std::filesystem::path>& read_only_roots) {
    close();
    if (directory.empty())
        return;  // Explicit ephemeral session for diagnostic runs.
    auto root = std::filesystem::weakly_canonical(std::filesystem::absolute(directory));
    for (const auto& protected_path : read_only_roots) {
        if (protected_path.empty())
            continue;
        auto protected_root = std::filesystem::weakly_canonical(protected_path);
        if (inside(root, protected_root) || inside(protected_root, root))
            fail("save directory overlaps the read-only assets or library");
    }
    std::filesystem::create_directories(root);
    check_file(root / L"session.lock");
    auto lock = CreateFileW((root / L"session.lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (lock == INVALID_HANDLE_VALUE)
        fail("cannot lock save directory (another Phase may be using it)");
    lock_ = lock;
    directory_ = root;
    const auto primary = root / L"state.bin", backup = root / L"state.bak";
    if (std::filesystem::exists(primary)) {
        try {
            state_ = read(primary);
        } catch (const UnsupportedVersion&) {
            throw;  // An older preview must not roll a newer save back to its backup.
        } catch (const std::exception& error) {
            std::fprintf(stderr, "SAVE primary rejected: %s\n", error.what());
            state_ = read(backup);  // Never silently reset a damaged save.
            recovered_ = true;
        }
    } else if (std::filesystem::exists(backup)) {
        state_ = read(backup);
        recovered_ = true;
    }
    std::fprintf(stderr, "SAVE loaded profiles=%zu records=%zu recovered=%u\n",
                 state_.profiles.size(), state_.records.size(), unsigned(recovered_));
}
void Store::commit(State next) {
    auto bytes = encode(next);
    if (!directory_.empty()) {
        const auto primary = directory_ / L"state.bin", backup = directory_ / L"state.bak";
        check_file(primary);
        check_file(backup);
        // CREATE_NEW avoids following or truncating a pre-existing staging file.
        const auto stage = directory_ / (L"pending-" + std::to_wstring(GetCurrentProcessId()) +
                                         L"-" + std::to_wstring(GetTickCount64()) + L".tmp");
        auto handle = CreateFileW(stage.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            fail("cannot create staged snapshot");
        DWORD written = 0;
        bool ok = WriteFile(handle, bytes.data(), DWORD(bytes.size()), &written, nullptr) &&
                  written == bytes.size() && FlushFileBuffers(handle);
        CloseHandle(handle);
        if (ok) {
            if (std::filesystem::exists(primary) && !recovered_)
                ok = ReplaceFileW(primary.c_str(), stage.c_str(), backup.c_str(), 0, nullptr,
                                  nullptr);
            else
                ok = MoveFileExW(stage.c_str(), primary.c_str(),
                                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        }
        if (!ok) {
            DeleteFileW(stage.c_str());
            fail("could not commit snapshot; previous save retained for recovery");
        }
        recovered_ = false;
    }
    state_ = std::move(next);
}
void Store::add_profile(const std::string& name) {
    auto next = state_;
    next.profiles.push_back(name);
    commit(std::move(next));
}
void Store::write(const std::string& name, std::vector<uint8_t> bytes, uint32_t kind) {
    const RecordKey key{kind, name};
    const auto found = state_.records.find(key);
    if (found != state_.records.end() && found->second == bytes)
        return;
    auto next = state_;
    next.records[key] = std::move(bytes);
    commit(std::move(next));
    std::fprintf(stderr, "SAVE committed %s bytes=%zu\n", name.c_str(),
                 state_.records.at(key).size());
}
}  // namespace phase::persistence
