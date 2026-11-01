// Windows preview services for the observed Phase startup path.
// See analysis/startup.md for contracts and evidence; unknown services stay unsupported.
// The translated game owns archive decoding and software rasterization.
#define NOMINMAX
#include "host.h"

#include "audio.h"
#include "hd_renderer.h"
#include "ipod/platform/sdl3/presenter.h"
#include "keyboard.h"
#include "library.h"
#include "persistence.h"
#include "runtime/memory.h"
#include "runtime/runtime.h"

// WGL declarations require the Windows types before gl.h.
// clang-format off
#include <windows.h>
#include <GL/gl.h>
// clang-format on
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace phase::host {
namespace {
HWND window = nullptr;
HDC dc = nullptr;
HGLRC rendering = nullptr;
bool quitting = false;
input::Keyboard keyboard;
ipod::video::Presenter presenter;
SDL_Window* display_window = nullptr;
SDL_Renderer* display_renderer = nullptr;
constexpr uint32_t input_nodes = 0x11001000;
std::filesystem::path asset_root;
std::vector<uint32_t> completions;
struct File {
    std::vector<uint8_t> bytes;
    uint32_t position = 0;
};
std::map<uint32_t, File> files;
uint32_t next_file = 1, next_buffer = 1;
persistence::Store storage;
constexpr uint32_t profile_base = 0x11004000;
std::map<uint32_t, uint32_t> profile_handles;  // Guest handle -> stable profile ID.
uint32_t brightness = 100;  // Window-only backlight emulation; never alters the monitor.
bool dimmed = false;
std::map<uint32_t, persistence::RecordKey> save_slots;
constexpr uint32_t framebuffer = 0x11010000;
uint32_t arg(const Cpu& cpu, unsigned index) {
    return index < 4 ? cpu.r[index] : ld32(cpu.r[SP] + 4 * (index - 4));
}
float floating(uint32_t value) {
    float result;
    std::memcpy(&result, &value, 4);
    return result;
}
float fixed(uint32_t value) {
    return static_cast<int32_t>(value) / 65536.0f;
}
std::string string_at(uint32_t address) {
    std::string result;
    for (unsigned i = 0; i < 1024; ++i) {
        char c = char(ld8(address + i));
        if (!c)
            return result;
        result += c;
    }
    fatal("unterminated asset path");
}
uint32_t expose_profile(const persistence::Profile& profile) {
    for (unsigned slot = 0; slot < 5; ++slot) {
        const auto handle = profile_base + slot * 64;
        if (profile_handles.count(handle))
            continue;
        auto* bytes = guest_pointer(handle, 32);
        std::memset(bytes, 0, 32);
        std::memcpy(bytes, profile.name.c_str(), profile.name.size() + 1);
        profile_handles.emplace(handle, profile.id);
        return handle;
    }
    fatal("profile handle limit exceeded");
}
void video_title() {
    const auto options = presenter.options();
    const std::string title =
        std::string("Phase - ") + presenter.scaling_description() +
        (options.pixel_perfect ? " | Integer" : " | Fit") +
        (hd::scale() > 1 ? " | HD " + std::to_string(hd::scale()) + "x" : "") +
        " | F6: filter, F7: integer scaling";
    SDL_SetWindowTitle(display_window, title.c_str());
}
unsigned physical_scan(SDL_Scancode scan) {
    switch (scan) {
    case SDL_SCANCODE_W:
        return 0x11;
    case SDL_SCANCODE_A:
        return 0x1e;
    case SDL_SCANCODE_S:
        return 0x1f;
    case SDL_SCANCODE_D:
        return 0x20;
    case SDL_SCANCODE_X:
        return 0x2d;
    case SDL_SCANCODE_RETURN:
        return 0x1c;
    case SDL_SCANCODE_O:
        return 0x18;
    case SDL_SCANCODE_P:
        return 0x19;
    case SDL_SCANCODE_SEMICOLON:
        return 0x27;
    case SDL_SCANCODE_SLASH:
        return 0x35;
    case SDL_SCANCODE_PERIOD:
        return 0x34;
    case SDL_SCANCODE_COMMA:
        return 0x33;
    case SDL_SCANCODE_K:
        return 0x25;
    case SDL_SCANCODE_I:
        return 0x17;
    default:
        return 0;
    }
}
std::filesystem::path asset_path(std::string name) {
    std::replace(name.begin(), name.end(), '\\', '/');
    if (name.empty() || name.find(':') != std::string::npos)
        fatal("invalid asset path");
    std::filesystem::path relative(name);
    if (relative.is_absolute() || relative.has_root_name())
        fatal("absolute asset path refused");
    for (const auto& part : relative)
        if (part == "..")
            fatal("parent asset path refused");
    auto path = std::filesystem::weakly_canonical(asset_root / relative);
    auto check = path.lexically_relative(asset_root);
    if (check.empty() || *check.begin() == "..")
        fatal("asset escapes root");
    return path;
}
uint32_t transfer(File& file, uint32_t destination, uint32_t length) {
    if (length > 64 * 1024 * 1024)
        fatal("oversize file read");
    const auto count = std::min<size_t>(
        length, file.bytes.size() - std::min<size_t>(file.position, file.bytes.size()));
    if (count)
        std::memcpy(guest_pointer(destination, uint32_t(count)), file.bytes.data() + file.position,
                    count);
    file.position += uint32_t(count);
    return uint32_t(count);
}
void queue(uint32_t request) {
    if (std::find(completions.begin(), completions.end(), request) != completions.end())
        fatal("duplicate file completion");
    st32(request, 0);
    completions.push_back(request);
}
bool file_service(Cpu& cpu, unsigned ordinal) {
    if (ordinal == 12) {
        if (cpu.r[0] != 1 && cpu.r[0] != 2)
            fatal("unsupported save resource kind");
        const auto name = string_at(cpu.r[1]);
        if (!persistence::valid_name(cpu.r[0], name))
            fatal("invalid save name: %s", name.c_str());
        uint32_t slot = 0;
        while (save_slots.count(slot))
            ++slot;
        if (slot >= 4) {
            cpu.r[0] = 0x19;
            return true;
        }
        save_slots[slot] = {cpu.r[0], name};
        st32(cpu.r[2], slot);
        cpu.r[0] = 0;
        return true;
    }
    if (ordinal == 14 || ordinal == 16) {
        const auto found = save_slots.find(cpu.r[0]);
        if (found == save_slots.end()) {
            cpu.r[0] = 8;
            return true;
        }
        if (ordinal == 16) {
            if (cpu.r[2] > 0x19000) {
                cpu.r[0] = 0x19;
                return true;
            }
            const auto* bytes = guest_pointer(cpu.r[1], cpu.r[2]);
            storage.write(found->second.second, std::vector<uint8_t>(bytes, bytes + cpu.r[2]),
                          found->second.first);
        } else
            save_slots.erase(found);
        cpu.r[0] = 0;
        return true;
    }
    if (ordinal == 0) {
        const uint32_t kind = cpu.r[0];
        const bool saved = (kind == 1 || kind == 2) && cpu.r[2] == 0;
        if (!saved && (cpu.r[0] != 0 || cpu.r[2] != 1))
            fatal("only read-only game assets are supported");
        const uint32_t request = cpu.r[3], object = ld32(request + 0x14);
        const auto name = string_at(cpu.r[1]);
        File file;
        bool exists = false;
        if (saved) {
            const auto found = storage.state().records.find({kind, name});
            if (found != storage.state().records.end()) {
                file.bytes = found->second;
                exists = true;
            }
        }
        if (!saved || (!exists && kind == 1 && library::mounted())) {
            const auto path = saved ? library::data_path(name) : asset_path(name);
            if (std::filesystem::exists(path) &&
                (!std::filesystem::is_regular_file(path) ||
                 std::filesystem::file_size(path) > 64 * 1024 * 1024))
                fatal("invalid or oversized asset");
            std::ifstream input(path, std::ios::binary);
            if (input) {
                file.bytes.assign(std::istreambuf_iterator<char>(input), {});
                exists = true;
            }
        }
        if (!exists) {
            std::fprintf(stderr, "FILE missing %s\n", name.c_str());
            st32(request + 0x28, 0xffffffff);
            queue(request);
            cpu.r[0] = 1;
            return true;
        }
        const uint32_t handle = next_file++, size = uint32_t(file.bytes.size());
        files.emplace(handle, std::move(file));
        st32(request + 0x34, handle);
        uint32_t result = size;
        if (ld32(request + 0x20))
            result = transfer(files.at(handle), ld32(request + 0x20), ld32(request + 0x24));
        st32(request + 0x2c, result);
        st32(request + 0x30, files.at(handle).position);
        st32(request + 0x28, 0);
        if (object)
            st32(object + 8, result);
        queue(request);
        cpu.r[0] = 1;
        std::fprintf(stderr, "FILE open %s size=%u handle=%u\n", name.c_str(), size, handle);
        return true;
    }
    if (ordinal == 1 || ordinal == 2) {
        const uint32_t request = cpu.r[0], object = ld32(request + 0x14),
                       handle = ld32(request + 0x34), operation = ld8(request + 0x10);
        auto found = files.find(handle);
        if (found == files.end())
            fatal("unknown file handle %u op=%u", handle, operation);
        auto& file = found->second;
        uint32_t count = 0;
        if (operation == 2) {
            files.erase(found);
        } else if (operation == 5) {
            const unsigned whence = ld8(request + 0x1c);
            if (whence > 2)
                fatal("invalid seek origin");
            int64_t base = whence == 1 ? file.position : whence == 2 ? file.bytes.size() : 0;
            int64_t position = base + static_cast<int32_t>(ld32(request + 0x18));
            if (position < 0 || uint64_t(position) > file.bytes.size())
                fatal("seek outside asset");
            file.position = uint32_t(position);
            st32(request + 0x30, file.position);
        } else if (operation == 3) {
            count = transfer(file, ld32(request + 0x20), ld32(request + 0x24));
            st32(request + 0x30, file.position);
        } else
            fatal("unsupported file operation %u", operation);
        st32(request + 0x28, 0);
        st32(request + 0x2c, count);
        if (object)
            st32(object + 8, count);
        queue(request);
        cpu.r[0] = 1;
        return true;
    }
    return false;
}
bool graphics(Cpu& cpu, unsigned ordinal) {
    // SDL may use its own GL context; original SDK queries always use this
    // separate compatibility context, which never presents the displayed image.
    if (!wglMakeCurrent(dc, rendering))
        fatal("OpenGL compatibility context unavailable");
    const auto a = [&](unsigned i) { return arg(cpu, i); };
    switch (ordinal) {
    case 0:
        if (a(0) != 0x84c0)
            fatal("unimplemented texture unit %x", a(0));
        break;
    case 5:
        glBlendFunc(a(0), a(1));
        break;
    case 8:
        glClear(a(0));
        break;
    case 9:
        glClearColor(floating(a(0)), floating(a(1)), floating(a(2)), floating(a(3)));
        break;
    case 14:
        if (a(0) != 0x84c0)
            fatal("unimplemented client texture unit");
        break;
    case 18:
        glColor4f(fixed(a(0)), fixed(a(1)), fixed(a(2)), fixed(a(3)));
        hd::set_color(fixed(a(0)), fixed(a(1)), fixed(a(2)), fixed(a(3)));
        break;
    case 26:  // Delete names from the software renderer's unused buffer namespace.
        if (cpu.r[0] > 4096)
            fatal("too many buffer deletions");
        for (uint32_t i = 0; i < cpu.r[0]; ++i) {
            const auto id = ld32(cpu.r[1] + i * 4);
            if (id >= next_buffer)
                fatal("invalid buffer name");
        }
        break;
    case 27:
        if (a(0) > 4096)
            fatal("oversized texture-name request");
        glDeleteTextures(a(0), reinterpret_cast<const GLuint*>(guest_pointer(a(1), 4 * a(0))));
        break;
    case 28:
        glDepthFunc(a(0));
        break;
    case 32:
        glDisable(a(0) == 0x84f5 ? GL_TEXTURE_2D : a(0));
        break;
    case 36:
        glEnable(a(0) == 0x84f5 ? GL_TEXTURE_2D : a(0));
        break;
    case 51:
        if (a(0) > 4096)
            fatal("oversized buffer-name request");
        guest_pointer(a(1), 4 * a(0));
        for (unsigned i = 0; i < a(0); ++i)
            st32(a(1) + i * 4, next_buffer++);
        break;
    case 52:
        if (a(0) > 4096)
            fatal("oversized texture-name request");
        glGenTextures(a(0), reinterpret_cast<GLuint*>(guest_pointer(a(1), 4 * a(0))));
        break;
    case 56: {
        // 1801b868 shifts the signed normalized integer color by 15, not 16.16.
        if (a(0) != GL_CURRENT_COLOR)
            fatal("unsupported integer GL query %x", a(0));
        glGetIntegerv(a(0), reinterpret_cast<GLint*>(guest_pointer(a(1), 16)));
        break;
    }
    case 63:
        if (a(0) != GL_TEXTURE_ENV || a(1) != GL_TEXTURE_ENV_MODE)
            fatal("unsupported texture environment query");
        glGetTexEnviv(a(0), a(1), reinterpret_cast<GLint*>(guest_pointer(a(2), 4)));
        break;
    case 71:
        cpu.r[0] = glIsEnabled(a(0));
        return true;
    case 119:
        glScissor(int(a(0)), int(a(1)), int(a(2)), int(a(3)));
        break;
    case 127:
        glTexEnvi(a(0), a(1), a(2));
        break;
    case 140:
        cpu.r[0] = 1;
        return true;  // EGL display token
    case 160:
        cpu.r[0] = 1;
        return true;  // EGL current surface token
    case 163:
        glFinish();
        break;
    case 165: {
        const auto scale = 20 + (dimmed ? std::min(brightness, 20u) : brightness) * 80 / 100;
        const auto previous_render_scale = hd::scale();
        auto* hd_frame = hd::finish(scale);
        std::vector<uint8_t> rgb;
        if (!hd_frame)
            rgb.resize(320 * 240 * 3);
        for (unsigned i = 0; !hd_frame && i < 320 * 240; ++i) {
            auto p = ld16(framebuffer + i * 2);
            // Keep zero readable like an LCD at minimum backlight. This is a
            // host presentation curve, not a claim about physical iPod luminance.
            rgb[i * 3] = uint8_t((p >> 11) * 255 / 31 * scale / 100);
            rgb[i * 3 + 1] = uint8_t(((p >> 5) & 63) * 255 / 63 * scale / 100);
            rgb[i * 3 + 2] = uint8_t((p & 31) * 255 / 31 * scale / 100);
        }
        const auto* previous_scaling = presenter.scaling_description();
        if (!(hd_frame ? presenter.present_texture(hd_frame)
                       : presenter.present(rgb.data(), 320, 240)))
            fatal("Video presentation failed: %s", SDL_GetError());
        if (std::strcmp(previous_scaling, presenter.scaling_description()) != 0 ||
            previous_render_scale != hd::scale())
            video_title();
        if (!wglMakeCurrent(dc, rendering))
            fatal("OpenGL compatibility context restore failed");
        cpu.r[0] = 1;
        return true;
    }
    default:
        return false;
    }
    cpu.r[0] = 0;
    return true;
}
}  // namespace
void initialize(const std::filesystem::path& assets, bool visible, bool sound,
                const std::filesystem::path& music_root, const std::filesystem::path& save_root,
                unsigned render_scale) {
    storage.open(save_root, {assets, music_root});
    for (const auto& profile : storage.state().profiles)
        expose_profile(profile);
    audio::initialize(sound);
    if (!music_root.empty())
        library::open(music_root);
    asset_root = std::filesystem::canonical(assets);
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
        fatal("SDL video unavailable: %s", SDL_GetError());
    if (!SDL_CreateWindowAndRenderer("Phase", 320, 240,
                                     SDL_WINDOW_RESIZABLE | (visible ? 0 : SDL_WINDOW_HIDDEN),
                                     &display_window, &display_renderer))
        fatal("SDL window unavailable: %s", SDL_GetError());
    presenter.attach(display_renderer, 320, 240);
    hd::configure(display_renderer, render_scale);
    ipod::video::Options options;
    const auto saved = storage.state().records.find({2, "host/video"});
    if (saved != storage.state().records.end()) {
        const auto& bytes = saved->second;
        if (bytes.size() == 3 && bytes[0] == 1 && bytes[1] < ipod::video::SCALING_COUNT &&
            bytes[2] <= 1)
            options = {static_cast<ipod::video::Scaling>(bytes[1]), bytes[2] != 0};
        else
            std::fprintf(stderr, "VIDEO: unrecognized saved options; using defaults\n");
    }
    presenter.set_options(options);
    video_title();
    SDL_SetRenderDrawColor(display_renderer, 0, 0, 0, 255);
    SDL_RenderClear(display_renderer);
    SDL_RenderPresent(display_renderer);
    // Hidden SDK state only; the SDL presenter owns the visible window.
    WNDCLASSW wc{};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PhaseSdkCompatibility";
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        fatal("RegisterClass failed");
    const DWORD style = WS_OVERLAPPEDWINDOW;
    RECT rect{0, 0, 320, 240};
    AdjustWindowRect(&rect, style, FALSE);
    window = CreateWindowW(wc.lpszClassName, L"Phase SDK compatibility", style, CW_USEDEFAULT,
                           CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, nullptr,
                           nullptr, wc.hInstance, nullptr);
    if (!window)
        fatal("CreateWindow failed");
    dc = GetDC(window);
    PIXELFORMATDESCRIPTOR format{};
    format.nSize = sizeof(format);
    format.nVersion = 1;
    format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    format.iPixelType = PFD_TYPE_RGBA;
    format.cColorBits = 24;
    format.cDepthBits = 24;
    format.cStencilBits = 8;
    int chosen = ChoosePixelFormat(dc, &format);
    if (!chosen || !SetPixelFormat(dc, chosen, &format))
        fatal("OpenGL pixel format unavailable");
    rendering = wglCreateContext(dc);
    if (!rendering || !wglMakeCurrent(dc, rendering))
        fatal("OpenGL context unavailable");
    glViewport(0, 0, 320, 240);
}
bool service(Cpu& cpu, const char* framework, unsigned ordinal) {
    if (std::strcmp(framework, "DebugUtil") == 0 && ordinal == 4) {
        // Firmware 082A23F0 -> 082954F4 -> Semihost_DebugVPrintf.
        // The observed destructor supplies an already formatted literal string.
        const auto message = string_at(cpu.r[0]);
        if (message.find('%') != std::string::npos)
            fatal("unsupported formatted debug message");
        std::fprintf(stderr, "GAME: %s\n", message.c_str());
        cpu.r[0] = 0;
        return true;
    }
    if (std::strcmp(framework, "Metadata") == 0)
        return library::service(cpu, ordinal);
    if (std::strcmp(framework, "Audio") == 0 || std::strcmp(framework, "SoundEffect") == 0)
        return audio::service(cpu, framework, ordinal);
    if (std::strcmp(framework, "miscTBD") == 0 && (ordinal == 1 || ordinal == 2)) {
        if (ordinal == 1) {
            // Firmware 082a2538 -> 081971c0 clamps a signed percentage.
            brightness = uint32_t(std::clamp(int32_t(cpu.r[0]), 0, 100));
            std::fprintf(stderr, "DISPLAY brightness=%u\n", brightness);
            cpu.r[0] = 0;
        } else
            cpu.r[0] = brightness;
        return true;
    }
    if (std::strcmp(framework, "miscTBD") == 0 && ordinal == 3) {
        // Firmware 0829ef5c -> 08197184 temporarily caps backlight at 20;
        // it does not replace the user's stored brightness percentage.
        dimmed = cpu.r[0] != 0;
        cpu.r[0] = 0;
        std::fprintf(stderr, "DISPLAY dimmed=%u\n", unsigned(dimmed));
        return true;
    }
    // Native 180051f8 clamps this battery-gauge reading to 22 (full).
    if (std::strcmp(framework, "miscTBD") == 0 && ordinal == 6) {
        cpu.r[0] = 22;
        return true;
    }
    if (std::strcmp(framework, "miscTBD") == 0 && ordinal == 7) {
        cpu.r[0] = 0;
        return true;
    }  // preview: not charging
    if (std::strcmp(framework, "OpenGLES") == 0)
        return graphics(cpu, ordinal);
    if (std::strcmp(framework, "AsyncFileIO") == 0)
        return file_service(cpu, ordinal);
    if (std::strcmp(framework, "InputEvents") == 0 && ordinal == 0) {
        st32(cpu.r[0], 0);
        st32(cpu.r[1], keyboard.wheel());
        cpu.r[0] = 0;
        return true;
    }
    if (std::strcmp(framework, "InputEvents") == 0 && ordinal == 1) {
        cpu.r[0] = 0;
        return true;  // nodes are owned by this frame's host arena
    }
    if (std::strcmp(framework, "Settings") == 0 && ordinal == 0) {
        const auto name = string_at(cpu.r[0]);
        if (name != "Language" || ld32(cpu.r[2]) < 4)
            fatal("unsupported setting %s", name.c_str());
        st32(cpu.r[1], 0);
        st32(cpu.r[2], 4);
        cpu.r[0] = 0;
        return true;
    }
    if (std::strcmp(framework, "Users") == 0) {
        if (ordinal == 0) {
            cpu.r[0] = uint32_t(storage.state().profiles.size());
            return true;
        }
        if (ordinal == 4) {
            const auto name = string_at(cpu.r[0]);
            if (name.empty() || name.size() > 31 || storage.state().profiles.size() >= 5)
                fatal("invalid preview profile");
            const auto id = storage.add_profile(name);
            cpu.r[0] = expose_profile({id, name});
            return true;
        }
        if (ordinal == 1 || ordinal == 2) {
            const auto& profiles = storage.state().profiles;
            const auto id =
                ordinal == 1 ? (cpu.r[0] < profiles.size() ? profiles[cpu.r[0]].id : 0) : cpu.r[0];
            cpu.r[0] = 0;
            for (const auto& [handle, profile_id] : profile_handles)
                if (profile_id == id)
                    cpu.r[0] = handle;
            return true;
        }
        if (ordinal != 3 && ordinal != 5 && ordinal != 8 && ordinal != 9)
            return false;
        const auto handle = cpu.r[0];
        const auto found = profile_handles.find(handle);
        if (ordinal == 3 && found == profile_handles.end()) {
            cpu.r[0] = 0;  // Firmware returns false when no matching object exists.
            return true;
        }
        if (ordinal == 9 && !handle)
            return true;  // Firmware's null-object no-op.
        if (found == profile_handles.end())
            fatal("invalid profile handle");
        if (ordinal == 3) {
            storage.delete_profile(found->second);
            profile_handles.erase(found);
            std::memset(guest_pointer(handle, 32), 0, 32);
            cpu.r[0] = 1;
            return true;
        }
        if (ordinal == 9) {
            const auto name = string_at(cpu.r[1]);
            storage.rename_profile(found->second, name);
            std::memset(guest_pointer(handle, 32), 0, 32);
            std::memcpy(guest_pointer(handle, 32), name.c_str(), name.size() + 1);
            cpu.r[0] = 0;
            return true;
        }
        if (ordinal == 5) {
            cpu.r[0] = handle;
            return true;
        }
        if (ordinal == 8) {
            cpu.r[0] = found->second;
            return true;
        }
    }
    if (std::strcmp(framework, "miscTBD") == 0 && ordinal == 8) {
        // Firmware 0829f3a4 builds a UTF-8 path into a bounded output buffer.
        const auto path = asset_path(string_at(cpu.r[3])).generic_string();
        const uint32_t capacity = ld32(cpu.r[2]);
        if (!cpu.r[1] || capacity <= path.size()) {
            cpu.r[0] = 0xffffffff;
            return true;
        }
        std::memcpy(guest_pointer(cpu.r[1], uint32_t(path.size() + 1)), path.c_str(),
                    path.size() + 1);
        st32(cpu.r[2], uint32_t(path.size()));
        cpu.r[0] = uint32_t(path.size());
        return true;
    }
    return false;
}
void begin_frame(uint32_t context) {
    keyboard.advance();
    st32(context + 0x38, framebuffer);
    uint32_t head = 0;
    for (auto it = completions.rbegin(); it != completions.rend(); ++it) {
        st32(*it, head);
        head = *it;
    }
    completions.clear();
    st32(context + 0x2c, head);
    const auto events = keyboard.take_buttons();
    if (events.size() > 256)
        fatal("input event budget exhausted");
    for (size_t i = 0; i < events.size(); ++i) {
        const auto node = input_nodes + uint32_t(i) * 12;
        st32(node, 0);
        st8(node, events[i].button);
        st8(node + 1, events[i].down ? 2 : 1);
        st32(node + 4, 0);
        st32(node + 8, i + 1 < events.size() ? node + 12 : 0);
    }
    st32(context + 0x30, events.empty() ? 0 : input_nodes);
}
void key(unsigned scan, bool down) {
    keyboard.key(scan, down);
}
bool pump() {
    SDL_Event event{};
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT ||
            (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
             event.window.windowID == SDL_GetWindowID(display_window)))
            quitting = true;
        else if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST &&
                 event.window.windowID == SDL_GetWindowID(display_window))
            keyboard.release_all();
        else if ((event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) &&
                 event.key.windowID == SDL_GetWindowID(display_window)) {
            if (presenter.handle_shortcut(event)) {
                const auto options = presenter.options();
                storage.write("host/video",
                              {1, static_cast<uint8_t>(options.scaling),
                               static_cast<uint8_t>(options.pixel_perfect)},
                              2);
                video_title();
            } else if (!event.key.repeat) {
                if (event.key.scancode == SDL_SCANCODE_ESCAPE && event.key.down)
                    quitting = true;
                else if (const auto scan = physical_scan(event.key.scancode))
                    key(scan, event.type == SDL_EVENT_KEY_DOWN);
            }
        }
    }
    return !quitting;
}
void screenshot(const std::filesystem::path& path) {
    int width = 0, height = 0;
    if (!display_window || !SDL_GetWindowSizeInPixels(display_window, &width, &height) ||
        width <= 0 || height <= 0) {
        std::fprintf(stderr, "SCREENSHOT skipped: window has no drawable area\n");
        return;
    }
    auto* captured = presenter.capture();
    auto* rgb = captured ? SDL_ConvertSurface(captured, SDL_PIXELFORMAT_RGB24) : nullptr;
    SDL_DestroySurface(captured);
    wglMakeCurrent(dc, rendering);
    if (!rgb)
        fatal("Screenshot failed: %s", SDL_GetError());
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << rgb->w << " " << rgb->h << "\n255\n";
    for (int y = 0; y < rgb->h; ++y)
        out.write(static_cast<const char*>(rgb->pixels) + size_t(y) * rgb->pitch, rgb->w * 3);
    SDL_DestroySurface(rgb);
    if (!out)
        fatal("could not write screenshot");
}
void close() {
    presenter.reset();
    hd::reset();
    SDL_DestroyRenderer(display_renderer);
    display_renderer = nullptr;
    SDL_DestroyWindow(display_window);
    display_window = nullptr;
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    if (rendering) {
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(rendering);
        rendering = nullptr;
    }
    if (dc) {
        ReleaseDC(window, dc);
        dc = nullptr;
    }
    if (window) {
        DestroyWindow(window);
        window = nullptr;
    }
    files.clear();
    completions.clear();
    audio::close();
    library::close();
    storage.close();
    profile_handles.clear();
    brightness = 100;
    dimmed = false;
    save_slots.clear();
    next_file = next_buffer = 1;
    quitting = false;
    keyboard = {};
}
}  // namespace phase::host
