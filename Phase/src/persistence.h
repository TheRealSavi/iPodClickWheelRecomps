#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace phase::persistence {
using RecordKey = std::pair<uint32_t, std::string>;
bool valid_name(uint32_t kind, const std::string& name);
// The game owns its score/settings serialization. This stores opaque SDK records
// together with the profile order that supplies their stable, one-based IDs.
struct State {
    std::vector<std::string> profiles;
    std::map<RecordKey, std::vector<uint8_t>> records;
};
class Store {
public:
    Store() = default;
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    void open(const std::filesystem::path& directory,
              const std::vector<std::filesystem::path>& read_only_roots = {});
    void close();
    const State& state() const { return state_; }
    void add_profile(const std::string& name);
    void write(const std::string& name, std::vector<uint8_t> bytes, uint32_t kind = 1);

private:
    void commit(State next);
    State state_;
    std::filesystem::path directory_;
    void* lock_ = nullptr;
    bool recovered_ = false;
};
}  // namespace phase::persistence
