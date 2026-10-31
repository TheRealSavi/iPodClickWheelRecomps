// Bounded Phase startup. Original routines execute unchanged; unknown SDK calls stop.
#include "funcs.h"
#include "host.h"
#include "instruction-fixtures.h"
#include "ipod_eapp.h"
#include "runtime/memory.h"
#include "runtime/runtime.h"

#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace ipod {
uint32_t watch_address = 0;
uint64_t entries = 0;
uint32_t last_entry = 0;
uint64_t entry_limit = 1000000;
struct Region {
    uint32_t base;
    std::vector<uint8_t> bytes;
};
std::array<Region, 4> memory = {{{RAM_BASE, std::vector<uint8_t>(RAM_SIZE)},
                                 {IMAGE_BASE, std::vector<uint8_t>(IMAGE_SPAN)},
                                 {HEAP_BASE, std::vector<uint8_t>(HEAP_SIZE)},
                                 {IRAM_BASE, std::vector<uint8_t>(IRAM_SIZE)}}};
[[noreturn]] void fatal(const char* format, ...) {
    char text[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    throw std::runtime_error(text);
}
uint8_t* guest_pointer(uint32_t address, uint32_t length) {
    for (auto& region : memory)
        if (address >= region.base &&
            uint64_t(address - region.base) + length <= region.bytes.size())
            return region.bytes.data() + (address - region.base);
    fatal("unmapped guest access %08x + %x", address, length);
}
void watch_hit(uint32_t, uint32_t) {
    fatal("unexpected memory watch");
}
[[noreturn]] void assert_trap(uint32_t address) {
    fatal("game assertion %08x", address);
}
void semihost(Cpu&, uint32_t number) {
    fatal("unimplemented semihost %x", number);
}
void trace_entry_report(uint32_t address) {
    last_entry = address;
    if (++entries > entry_limit)
        fatal("function-entry budget exhausted");
}
}  // namespace ipod

namespace phase::eapp {
uint32_t next_arena = ipod::HEAP_BASE + 0x10000;
uint32_t clock_microseconds = 0;
bool graphical = false;
bool trace_sdk = true;
void framework_call(Cpu& cpu, const char* framework, unsigned ordinal, void (*handler)(Cpu&)) {
    if (trace_sdk)
        std::fprintf(stderr, "SDK %s[%u] lr=%08x r0=%08x r1=%08x r2=%08x r3=%08x\n", framework,
                     ordinal, cpu.r[ipod::LR], cpu.r[0], cpu.r[1], cpu.r[2], cpu.r[3]);
    if (handler) {
        handler(cpu);
        return;
    }
    // Observed native arena initialization requests a byte count and retains the
    // result. This owns backing blocks only; the game's suballocator is unchanged.
    if (std::string(framework) == "MemoryAlloc" && ordinal == 0) {
        const uint64_t size = (uint64_t(cpu.r[0]) + 15) & ~15ULL;
        if (!size || uint64_t(next_arena) + size > uint64_t(ipod::HEAP_BASE) + ipod::HEAP_SIZE)
            ipod::fatal("arena request exceeds bounded host heap");
        cpu.r[0] = next_arena;
        next_arena += uint32_t(size);
        return;
    }
    // Phase 0x180055d0 reads a single output word; 0x180055e4 compares elapsed
    // readings with 1,000,000. Unlike Vortex, this title uses ordinal 12.
    // This deterministic diagnostic clock is not an audio playback clock.
    if (std::string(framework) == "miscTBD" && ordinal == 12) {
        ipod::st32(cpu.r[0], ++clock_microseconds);
        cpu.r[0] = clock_microseconds;
        return;
    }
    if (graphical && host::service(cpu, framework, ordinal))
        return;
    ipod::fatal("unimplemented %s[%u], lr=%08x", framework, ordinal, cpu.r[ipod::LR]);
}
}  // namespace phase::eapp

std::filesystem::path dump_directory;
int test_host(const std::filesystem::path& assets);
int test_instructions() {
    using namespace ipod;
    unsigned index, flags;
    while (scanf("%u %u", &index, &flags) == 2) {
        auto& cpu = registers();
        cpu = {};
        cpu.set_cpsr_flags(flags);
        for (unsigned i = 0; i < 15; ++i)
            if (scanf("%u", &cpu.r[i]) != 1)
                fatal("bad fixture registers");
        for (unsigned i = 0; i < 32; ++i) {
            unsigned value;
            if (scanf("%u", &value) != 1)
                fatal("bad fixture memory");
            st32(0x19050000 + i * 4, value);
        }
        phase::game::instruction_fixture(index, cpu);
        for (unsigned i = 0; i < 15; ++i)
            printf("%u ", cpu.r[i]);
        printf("%u", cpu.cpsr() & 0xff000000u);
        for (unsigned i = 0; i < 32; ++i)
            printf(" %u", ld32(0x19050000 + i * 4));
        printf("\n");
    }
    return 0;
}

void dump_state() {
    if (dump_directory.empty())
        return;
    std::filesystem::create_directories(dump_directory);
    for (const auto& region : ipod::memory) {
        char name[32];
        std::snprintf(name, sizeof(name), "%08x.bin", region.base);
        std::ofstream output(dump_directory / name, std::ios::binary);
        output.write(reinterpret_cast<const char*>(region.bytes.data()), region.bytes.size());
        if (!output)
            throw std::runtime_error("could not write verification snapshot");
    }
    std::ofstream state(dump_directory / "registers.txt");
    const auto& cpu = ipod::registers();
    for (unsigned i = 0; i < 15; ++i)
        state << cpu.r[i] << '\n';
    state << cpu.cpsr() << '\n';
}

int main(int argc, char** argv) {
    std::filesystem::path screenshot_path;
    try {
        if (argc == 2 && std::string(argv[1]) == "--instructions")
            return test_instructions();
        if (argc == 3 && std::string(argv[1]) == "--test-host")
            return test_host(argv[2]);
        if (argc < 2)
            throw std::runtime_error("Run tools/run.py --image PATH");
        unsigned frames = 3;
        bool visible = false;
        std::filesystem::path assets;
        std::filesystem::path input_script;
        for (int i = 2; i < argc; ++i) {
            std::string option = argv[i];
            if (option == "--visible") {
                visible = true;
                continue;
            }
            if (option == "--quiet-sdk") {
                phase::eapp::trace_sdk = false;
                continue;
            }
            if (i + 1 >= argc)
                throw std::runtime_error("missing option value");
            if (option == "--dump-state")
                dump_directory = argv[++i];
            else if (option == "--assets")
                assets = argv[++i];
            else if (option == "--screenshot")
                screenshot_path = argv[++i];
            else if (option == "--input-script")
                input_script = argv[++i];
            else if (option == "--frames") {
                frames = std::stoul(argv[++i]);
                if (!frames || frames > 36000)
                    throw std::runtime_error("invalid frame limit");
            } else
                throw std::runtime_error("unknown option");
        }
        if (assets.empty() && (visible || !screenshot_path.empty()))
            throw std::runtime_error("--visible and --screenshot require --assets");
        struct InputStep {
            unsigned frame, scan;
            bool down;
        };
        std::vector<InputStep> input_steps;
        if (!input_script.empty()) {
            if (assets.empty())
                throw std::runtime_error("input script requires --assets");
            std::ifstream script(input_script);
            if (!script)
                throw std::runtime_error("cannot open input script");
            std::string line;
            while (std::getline(script, line)) {
                if (line.empty() || line[0] == '#')
                    continue;
                std::istringstream row(line);
                unsigned frame, scan, down;
                std::string extra;
                if (!(row >> frame >> std::hex >> scan >> std::dec >> down) || (row >> extra) ||
                    scan >= 128 || down > 1 || frame >= frames ||
                    (!input_steps.empty() && frame < input_steps.back().frame))
                    throw std::runtime_error("invalid input script row (frame hex-scan 0-or-1)");
                input_steps.push_back({frame, scan, down != 0});
            }
        }
        size_t next_input = 0;
        if (!assets.empty()) {
            phase::host::initialize(assets, visible);
            phase::eapp::graphical = true;
        }
        std::ifstream input(argv[1], std::ios::binary);
        std::vector<char> data((std::istreambuf_iterator<char>(input)), {});
        if (data.size() != 947252 || std::memcmp(data.data(), "eapp", 4))
            throw std::runtime_error("unexpected Phase image");
        std::memcpy(ipod::guest_pointer(ipod::IMAGE_BASE, uint32_t(data.size())), data.data(),
                    data.size());
        auto& cpu = ipod::registers();
        cpu.r[ipod::SP] = ipod::RAM_BASE + ipod::RAM_SIZE - 256;
        ipod::trace_entries_enabled() = true;
        const uint32_t context = ipod::HEAP_BASE;
        cpu.r[0] = context;
        cpu.r[1] = context + 0x100;
        cpu.r[ipod::LR] = ipod::RAM_BASE;  // same return sentinel as the ARM oracle
        std::fprintf(stderr, "STAGE initialize\n");
        phase::game::call_indirect(ipod::ld32(ipod::IMAGE_BASE + 0x14));
        std::fprintf(stderr, "STAGE initialized entries=%llu\n", ipod::entries);
        // Header slot 1 is teardown. Slot 4 is the frame callback. Phase reads
        // reason 0 as initial loading and writes its next state into the answer.
        for (unsigned frame = 0; frame < frames; ++frame) {
            ipod::entry_limit = ipod::entries + 1000000;
            if (phase::eapp::graphical) {
                if (!phase::host::pump())
                    break;
                while (next_input < input_steps.size() && input_steps[next_input].frame == frame) {
                    const auto& step = input_steps[next_input++];
                    phase::host::key(step.scan, step.down);
                }
                phase::host::begin_frame(context);
            }
            cpu.r[0] = context;
            cpu.r[1] = context + 0x100;
            cpu.r[2] = cpu.r[3] = 0;
            cpu.r[ipod::LR] = ipod::RAM_BASE;
            phase::eapp::clock_microseconds += 16667;
            std::fprintf(stderr, "STAGE frame=%u state=%u\n", frame, ipod::ld8(context));
            phase::game::call_indirect(ipod::ld32(ipod::IMAGE_BASE + 0x24));
            std::fprintf(stderr, "STAGE frame-returned=%u state=%u\n", frame, ipod::ld8(context));
        }
        dump_state();
        if (!screenshot_path.empty())
            phase::host::screenshot(screenshot_path);
        phase::host::close();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "STOP: %s; last=%08x entries=%llu\n", error.what(), ipod::last_entry,
                     ipod::entries);
        try {
            dump_state();
        } catch (const std::exception& dump_error) {
            std::fprintf(stderr, "snapshot failed: %s\n", dump_error.what());
        }
        if (!screenshot_path.empty() && phase::eapp::graphical) {
            try {
                phase::host::screenshot(screenshot_path);
            } catch (const std::exception& screenshot_error) {
                std::fprintf(stderr, "screenshot failed: %s\n", screenshot_error.what());
            }
        }
        phase::host::close();
        return 2;
    }
}
