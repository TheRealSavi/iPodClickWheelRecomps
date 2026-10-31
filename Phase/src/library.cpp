#define NOMINMAX
#include "library.h"

#include "runtime/memory.h"
#include "runtime/runtime.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <vector>
#include <windows.h>

namespace phase::library {
namespace {
struct Track {
    uint32_t id, duration, media;
    uint64_t pid;
    std::map<unsigned, std::string> text;
};
struct Playlist {
    uint64_t pid;
    std::string name;
    std::vector<uint32_t> ids;
};
std::filesystem::path root;
std::vector<Track> tracks;
std::vector<Playlist> playlists;
std::vector<size_t> selected;
constexpr uint32_t track_base = 0x60000000, playlist_base = 0x70000000;
uint32_t include_mask = 1, exclude_mask = 0x7e;
int selection = -1;
struct Group {
    unsigned field;
    std::string value;
};
std::vector<Group> groups;
std::map<unsigned, std::string> filters;
std::map<unsigned, std::vector<uint32_t>> group_lists;
constexpr uint32_t group_base = 0x50000000;
struct Reader {
    std::vector<uint8_t> bytes;
    void check(size_t p, size_t n, size_t end) const {
        if (p > end || n > end - p || end > bytes.size())
            fatal("truncated iTunesDB");
    }
    uint32_t word(size_t p) const {
        check(p, 4, bytes.size());
        uint32_t v;
        std::memcpy(&v, bytes.data() + p, 4);
        return v;
    }
    uint64_t wide(size_t p) const { return word(p) | (uint64_t(word(p + 4)) << 32); }
    bool tag(size_t p, const char* value) const {
        check(p, 4, bytes.size());
        return std::memcmp(bytes.data() + p, value, 4) == 0;
    }
    std::map<unsigned, std::string> strings(size_t p, size_t end) const {
        std::map<unsigned, std::string> result;
        while (p < end && tag(p, "mhod")) {
            check(p, 24, end);
            const auto size = word(p + 8), header = word(p + 4), type = word(p + 12);
            if (size < header || header < 24)
                fatal("invalid MHOD");
            check(p, size, end);
            if (type >= 1 && type <= 6) {
                check(p + header, 16, p + size);
                const auto encoding = word(p + header), length = word(p + header + 4);
                check(p + header + 16, length, p + size);
                const auto* data = bytes.data() + p + header + 16;
                if (encoding == 1) {
                    if (length % 2)
                        fatal("odd UTF-16 string");
                    const auto* wide = reinterpret_cast<const wchar_t*>(data);
                    int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide,
                                                    int(length / 2), nullptr, 0, nullptr, nullptr);
                    if (!count && length)
                        fatal("invalid UTF-16 string");
                    std::string text(size_t(count), '\0');
                    if (count)
                        WideCharToMultiByte(CP_UTF8, 0, wide, int(length / 2), text.data(), count,
                                            nullptr, nullptr);
                    result[type] = std::move(text);
                } else if (encoding == 2) {
                    result[type] = std::string(reinterpret_cast<const char*>(data), length);
                } else
                    fatal("unsupported iTunesDB string encoding");
            }
            p += size;
        }
        return result;
    }
    void list(size_t p, size_t end, bool songs) const {
        check(p, 12, end);
        if (!tag(p, songs ? "mhlt" : "mhlp"))
            fatal("invalid iTunesDB list");
        const auto header = word(p + 4), count = word(p + 8);
        if (header < 12 || count > 100000)
            fatal("invalid iTunesDB list header");
        check(p, header, end);
        p += header;
        for (uint32_t i = 0; i < count; ++i) {
            check(p, 12, end);
            auto h = word(p + 4), size = word(p + 8);
            if (size < h || h < (songs ? 0xd4u : 0x24u))
                fatal("short iTunesDB record");
            check(p, size, end);
            if (!tag(p, songs ? "mhit" : "mhyp"))
                fatal("invalid iTunesDB record");
            if (songs) {
                Track t{word(p + 16), word(p + 0x28), word(p + 0xd0), wide(p + 0x70),
                        strings(p + h, p + size)};
                if (!t.id || !t.pid ||
                    std::any_of(tracks.begin(), tracks.end(),
                                [&](const Track& x) { return x.id == t.id || x.pid == t.pid; }))
                    fatal("duplicate or zero iTunesDB track identity");
                tracks.push_back(std::move(t));
            } else if (!bytes[p + 0x14]) {
                auto text = strings(p + h, p + size);
                Playlist pl{wide(p + 0x1c), text[1], {}};
                for (size_t q = p + h; q < p + size;) {
                    check(q, 12, p + size);
                    auto n = word(q + 8), ch = word(q + 4);
                    if (n < ch || ch < 12)
                        fatal("invalid playlist child");
                    check(q, n, p + size);
                    if (tag(q, "mhip")) {
                        if (ch < 0x1c)
                            fatal("short playlist item");
                        if (!bytes[q + 0x11])
                            pl.ids.push_back(word(q + 0x18));
                    }
                    q += n;
                }
                playlists.push_back(std::move(pl));
            }
            p += size;
        }
        if (p != end)
            fatal("iTunesDB list extent mismatch");
    }
};
void refresh() {
    selected.clear();
    auto add = [](uint32_t id) {
        for (size_t i = 0; i < tracks.size(); ++i)
            if (tracks[i].id == id) {
                // Phase passes music=1, exclude=0x7e through its media-kind adapter.
                bool matches =
                    (tracks[i].media & include_mask) && !(tracks[i].media & exclude_mask);
                for (const auto& [field, value] : filters) {
                    const auto found = tracks[i].text.find(field);
                    if ((found == tracks[i].text.end() ? std::string() : found->second) != value)
                        matches = false;
                }
                if (matches)
                    selected.push_back(i);
                return;
            }
        fatal("playlist references absent track");
    };
    if (selection >= 0)
        for (auto id : playlists.at(selection).ids)
            add(id);
    else
        for (const auto& t : tracks)
            add(t.id);
}
const std::vector<uint32_t>& grouped(unsigned field) {
    auto& result = group_lists[field];
    result.clear();
    std::map<std::string, uint32_t> values;
    for (auto i : selected) {
        auto it = tracks[i].text.find(field);
        std::string value = it == tracks[i].text.end() ? std::string() : it->second;
        if (value.empty())
            continue;
        if (values.count(value))
            continue;
        auto found = std::find_if(groups.begin(), groups.end(), [&](const Group& g) {
            return g.field == field && g.value == value;
        });
        uint32_t handle = group_base + uint32_t(found - groups.begin());
        if (found == groups.end())
            groups.push_back({field, value});
        values.emplace(value, handle);
    }
    for (const auto& [value, handle] : values)
        result.push_back(handle);
    return result;
}
void output(Cpu& cpu, const std::string& text) {
    const auto capacity = ld32(cpu.r[2]);
    if (!capacity || !cpu.r[1]) {
        cpu.r[0] = 0xffffffff;
        return;
    }
    auto n = std::min<size_t>(text.size(), capacity - 1);
    // Avoid cutting a UTF-8 continuation sequence.
    while (n && n < text.size() && (uint8_t(text[n]) & 0xc0) == 0x80)
        --n;
    std::memcpy(guest_pointer(cpu.r[1], uint32_t(n + 1)), text.data(), n);
    st8(cpu.r[1] + uint32_t(n), 0);
    st32(cpu.r[2], uint32_t(n));
    cpu.r[0] = 0;
}
}  // namespace
std::filesystem::path contained(const std::filesystem::path& base, std::string relative) {
    std::replace(relative.begin(), relative.end(), '\\', '/');
    std::filesystem::path p = std::filesystem::u8path(relative);
    if (relative.empty() || relative.find(':') != std::string::npos || p.is_absolute() ||
        p.has_root_name())
        fatal("invalid library path");
    for (const auto& part : p)
        if (part == "..")
            fatal("parent library path refused");
    p = std::filesystem::weakly_canonical(base / p);
    auto suffix = p.lexically_relative(base);
    if (suffix.empty() || *suffix.begin() == "..")
        fatal("library path escapes root");
    return p;
}
bool mounted() {
    return !root.empty();
}
std::filesystem::path data_path(const std::string& relative) {
    return contained(root, "iPod_Control/GameData_RW/1D000/" + relative);
}
void close() {
    root.clear();
    tracks.clear();
    playlists.clear();
    selected.clear();
    groups.clear();
    filters.clear();
    group_lists.clear();
    selection = -1;
    include_mask = 1;
    exclude_mask = 0x7e;
}
void open(const std::filesystem::path& directory) {
    close();
    root = std::filesystem::canonical(directory);
    auto path = contained(root, "iPod_Control/iTunes/iTunesDB");
    if (std::filesystem::file_size(path) > 64 * 1024 * 1024)
        fatal("oversized iTunesDB");
    std::ifstream input(path, std::ios::binary);
    Reader r{{std::istreambuf_iterator<char>(input), {}}};
    r.check(0, 20, r.bytes.size());
    if (!r.tag(0, "mhbd") || r.word(8) != r.bytes.size())
        fatal("expected uncompressed iTunesDB");
    std::map<unsigned, std::pair<size_t, size_t>> datasets;
    for (size_t p = r.word(4); p < r.bytes.size();) {
        r.check(p, 16, r.bytes.size());
        auto h = r.word(p + 4), n = r.word(p + 8), type = r.word(p + 12);
        if (!r.tag(p, "mhsd") || h < 16 || n < h)
            fatal("invalid iTunesDB dataset");
        r.check(p, n, r.bytes.size());
        if (!datasets.emplace(type, std::make_pair(p + h, p + n)).second)
            fatal("duplicate iTunesDB dataset");
        p += n;
    }
    if (!datasets.count(1) || (!datasets.count(3) && !datasets.count(2)))
        fatal("missing music datasets");
    auto t = datasets.at(1), p = datasets.at(datasets.count(3) ? 3 : 2);
    r.list(t.first, t.second, true);
    r.list(p.first, p.second, false);
    refresh();
    std::fprintf(stderr, "LIBRARY loaded tracks=%zu playlists=%zu\n", tracks.size(),
                 playlists.size());
}
bool service(Cpu& cpu, unsigned ordinal) {
    auto handle = cpu.r[0];
    switch (ordinal) {
    case 45:
    case 46:
        cpu.r[0] = 0;
        return true;
    case 47:
        selection = -1;
        filters.clear();
        refresh();
        cpu.r[0] = 0;
        return true;
    case 58:
    case 59:
    case 60:
        cpu.r[0] = uint32_t(grouped(ordinal == 58 ? 3 : ordinal == 59 ? 4 : 5).size());
        return true;
    case 80:
    case 81:
    case 82: {
        const auto& list = grouped(ordinal == 80 ? 4 : ordinal == 81 ? 3 : 5);
        cpu.r[0] = cpu.r[1] < list.size() ? list[cpu.r[1]] : 0xffffffff;
        return true;
    }
    case 44:
        if (handle < group_base || handle - group_base >= groups.size())
            fatal("invalid group handle");
        output(cpu, groups[handle - group_base].value);
        return true;
    case 49:
    case 50:
    case 51: {
        auto h = cpu.r[1];
        unsigned field = ordinal == 49 ? 4 : ordinal == 50 ? 3 : 5;
        if (h < group_base || h - group_base >= groups.size() ||
            groups[h - group_base].field != field)
            fatal("invalid group filter");
        filters[field] = groups[h - group_base].value;
        refresh();
        cpu.r[0] = 0;
        return true;
    }
    case 57:
        if (cpu.r[1] != 1 || cpu.r[2] != 0x7e)
            fatal("unsupported Phase media filter");
        include_mask = 1;
        exclude_mask = 0x16e;
        refresh();
        cpu.r[0] = 0;
        return true;
    case 61:
        cpu.r[0] = uint32_t(playlists.size());
        return true;
    case 89:
        cpu.r[0] = cpu.r[1] < playlists.size() ? playlist_base + cpu.r[1] : 0xffffffff;
        return true;
    case 48:
        if (cpu.r[1] < playlist_base || cpu.r[1] - playlist_base >= playlists.size())
            fatal("invalid playlist selection");
        selection = int(cpu.r[1] - playlist_base);
        refresh();
        cpu.r[0] = 0;
        return true;
    case 63:
        cpu.r[0] = uint32_t(selected.size());
        return true;
    case 73:
        refresh();
        cpu.r[0] = 0;
        return true;
    case 67:
    case 68:
    case 69:
    case 72: {
        const unsigned field = ordinal == 67 ? 4 : ordinal == 68 ? 3 : ordinal == 69 ? 5 : 1;
        std::stable_sort(selected.begin(), selected.end(), [&](size_t a, size_t b) {
            auto value = [&](size_t i) {
                auto it = tracks[i].text.find(field);
                return it == tracks[i].text.end() ? std::string() : it->second;
            };
            return value(a) < value(b);
        });
        cpu.r[0] = 0;
        return true;
    }
    case 78:
        cpu.r[0] =
            cpu.r[1] < selected.size() ? track_base + uint32_t(selected[cpu.r[1]]) : 0xffffffff;
        return true;
    case 160:
    case 95:
        // Firmware releases these SDK descriptors. Host handles index immutable
        // records and remain valid for this mounted library's lifetime.
        cpu.r[0] = 0;
        return true;
    case 162:
    case 164: {
        if (handle < playlist_base || handle - playlist_base >= playlists.size())
            fatal("invalid playlist handle");
        const auto& p = playlists[handle - playlist_base];
        if (ordinal == 164)
            output(cpu, p.name);
        else {
            cpu.r[0] = uint32_t(p.pid);
            cpu.r[1] = uint32_t(p.pid >> 32);
        }
        return true;
    }
    case 97:
    case 100:
    case 101:
    case 107:
    case 119:
    case 120: {
        if (handle < track_base || handle - track_base >= tracks.size())
            fatal("invalid track handle");
        const auto& t = tracks[handle - track_base];
        if (ordinal == 97) {
            cpu.r[0] = uint32_t(t.pid);
            cpu.r[1] = uint32_t(t.pid >> 32);
        } else if (ordinal == 119 || ordinal == 120)
            cpu.r[0] = ordinal == 119 ? t.duration / 1000 : t.duration;
        else {
            auto field = t.text.find(ordinal == 100 ? 2 : ordinal == 101 ? 1 : 6);
            auto text = field == t.text.end() ? std::string() : field->second;
            if (ordinal == 100) {
                if (!text.empty() && text[0] == ':')
                    text.erase(0, 1);
                std::replace(text.begin(), text.end(), ':', '/');
                text = contained(root, text).u8string();
            }
            output(cpu, text);
        }
        return true;
    }
    default:
        return false;
    }
}
}  // namespace phase::library
