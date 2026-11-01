#include "hd_renderer.h"

#include "runtime/cpu.h"
#include "runtime/memory.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <set>
#include <tuple>
#include <vector>

namespace phase::hd {
namespace {
constexpr uint32_t framebuffer = 0x11010000;
constexpr unsigned width = 320, height = 240;
SDL_Renderer* renderer = nullptr;
SDL_Texture* canvas = nullptr;
SDL_Texture* patch = nullptr;
unsigned render_scale = 1;
bool failed = false, logged = false;
uint64_t triangles = 0;
uint64_t blits = 0, native_patches = 0;
std::map<uint32_t, uint64_t> native_blits;
std::array<uint16_t, width * height> baseline{};
std::vector<uint8_t> patch_pixels(width* height * 4);
std::set<uint32_t> unsupported;
SDL_FColor current_color{1, 1, 1, 1};
std::map<uint64_t, SDL_Texture*> blit_textures;

struct CachedTexture {
    SDL_Texture* texture = nullptr;
    std::vector<uint8_t> source;
    std::vector<uint8_t> palette;
};
using TextureKey = std::tuple<uint32_t, unsigned, unsigned, bool>;
std::map<TextureKey, CachedTexture> textures;
using IndexedBlitKey = std::tuple<uint32_t, unsigned, unsigned, unsigned, unsigned>;
// Texture pointers are borrowed from blit_textures; source/palette snapshots
// detect reused guest addresses without decoding every letter on every frame.
std::map<IndexedBlitKey, CachedTexture> indexed_blits;
size_t texture_bytes = 0;
constexpr size_t texture_budget = 32 * 1024 * 1024;

void clear_textures() {
    for (auto& entry : textures)
        SDL_DestroyTexture(entry.second.texture);
    textures.clear();
    for (auto& entry : blit_textures)
        SDL_DestroyTexture(entry.second);
    blit_textures.clear();
    indexed_blits.clear();
    texture_bytes = 0;
}
bool checked(bool result) {
    if (!result && !failed) {
        SDL_Log("Phase HD unavailable: %s; using original rendering", SDL_GetError());
        failed = true;
    }
    return result;
}
std::array<uint8_t, 4> rgba565(uint16_t color, unsigned alpha = 255) {
    return {uint8_t((color >> 11) * 255 / 31), uint8_t(((color >> 5) & 63) * 255 / 63),
            uint8_t((color & 31) * 255 / 31), uint8_t(alpha)};
}
std::array<uint8_t, 4> rgba4444(uint16_t packed) {
    // Expand the source channels directly, without an intermediate RGB565
    // truncation. Filtering and blending then retain eight bits per channel.
    return {uint8_t((packed >> 12) * 17), uint8_t(((packed >> 8) & 15) * 17),
            uint8_t(((packed >> 4) & 15) * 17), uint8_t((packed & 15) * 17)};
}
void remember_native() {
    std::memcpy(baseline.data(), ipod::guest_pointer(framebuffer, unsigned(sizeof(baseline))),
                sizeof(baseline));
}
bool target_canvas() {
    return checked(SDL_SetRenderTarget(renderer, canvas)) &&
           checked(SDL_SetRenderLogicalPresentation(renderer, 0, 0,
                                                    SDL_LOGICAL_PRESENTATION_DISABLED)) &&
           checked(SDL_SetRenderViewport(renderer, nullptr)) &&
           checked(SDL_SetRenderScale(renderer, float(render_scale), float(render_scale)));
}
// Unobserved native operations (for example bitmap blits) retain their original
// pixels, in draw order. Accepted triangles update the baseline only AFTER the
// original draw, so their native pixels never overwrite the HD geometry.
bool sync_native() {
    auto* pixels = ipod::guest_pointer(framebuffer, unsigned(sizeof(baseline)));
    if (!std::memcmp(pixels, baseline.data(), sizeof(baseline)))
        return true;
    ++native_patches;
    std::fill(patch_pixels.begin(), patch_pixels.end(), 0);
    for (unsigned i = 0; i < width * height; ++i) {
        uint16_t pixel;
        std::memcpy(&pixel, pixels + i * 2, 2);
        if (pixel != baseline[i]) {
            auto rgba = rgba565(pixel);
            std::copy(rgba.begin(), rgba.end(), patch_pixels.begin() + i * 4);
        }
    }
    remember_native();
    const SDL_FRect rect{0, 0, float(width), float(height)};
    return checked(SDL_UpdateTexture(patch, nullptr, patch_pixels.data(), width * 4)) &&
           checked(SDL_SetRenderClipRect(renderer, nullptr)) &&
           checked(SDL_RenderTexture(renderer, patch, nullptr, &rect));
}
float fixed(uint32_t value) {
    return float(int32_t(value)) / 65536.0f;
}
float color(uint32_t value) {
    return std::clamp(fixed(value), 0.0f, 1.0f);
}

SDL_Texture* texture(uint32_t context, bool indexed) {
    const auto address = ipod::ld32(context + 0x18);
    const auto w = ipod::ld32(context + 0x1c), h = ipod::ld32(context + 0x20);
    if (!address || !w || !h || w > 2048 || h > 2048)
        return nullptr;
    const auto bytes = indexed ? (w * h + 1) / 2 : w * h * 2;
    const auto* source = ipod::guest_pointer(address, bytes);
    const auto palette_address = indexed ? ipod::ld32(context + 0x24) : 0;
    if (indexed && !palette_address)
        return nullptr;
    const auto* palette = indexed ? ipod::guest_pointer(palette_address, 64) : nullptr;
    const TextureKey key{address, w, h, indexed};
    if (!textures.count(key) &&
        (textures.size() >= 128 || texture_bytes + w * h * 4 > texture_budget))
        clear_textures();
    auto& cached = textures[key];
    if (cached.texture && cached.source.size() == bytes &&
        !std::memcmp(cached.source.data(), source, bytes) &&
        (!indexed || !std::memcmp(cached.palette.data(), palette, 64)))
        return cached.texture;
    std::vector<uint8_t> rgba(w * h * 4);
    for (unsigned i = 0; i < w * h; ++i) {
        std::array<uint8_t, 4> p;
        if (indexed) {
            const auto index = (i & 1) ? source[i / 2] & 15 : source[i / 2] >> 4;
            uint32_t packed;
            std::memcpy(&packed, palette + index * 4, 4);
            p = rgba565(uint16_t(packed >> 8), packed & 255);
        } else {
            uint16_t packed;
            std::memcpy(&packed, source + i * 2, 2);
            p = rgba4444(packed);
        }
        std::copy(p.begin(), p.end(), rgba.begin() + i * 4);
    }
    if (!cached.texture) {
        cached.texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                           SDL_TEXTUREACCESS_STATIC, int(w), int(h));
        texture_bytes += w * h * 4;
    }
    if (!checked(cached.texture != nullptr) ||
        !checked(SDL_UpdateTexture(cached.texture, nullptr, rgba.data(), int(w * 4))) ||
        !checked(SDL_SetTextureScaleMode(cached.texture, SDL_SCALEMODE_LINEAR)) ||
        !checked(SDL_SetTextureBlendMode(cached.texture, SDL_BLENDMODE_BLEND)))
        return nullptr;
    cached.source.assign(source, source + bytes);
    if (indexed)
        cached.palette.assign(palette, palette + 64);
    return cached.texture;
}
bool triangle(const ipod::Cpu& cpu) {
    if (!renderer || render_scale == 1 || failed || cpu.r[1] != framebuffer)
        return false;
    const uint32_t context = cpu.r[0], vertices = cpu.r[2];
    const auto shader = ipod::ld32(context + 0x6c);
    const bool indexed = shader == 0x18089fc0 || shader == 0x1808a7e0;
    const bool additive = shader == 0x18087984 || shader == 0x1809125c || shader == 0x180914dc;
    const bool textured = shader == 0x1808751c || shader == 0x180877fc || shader == 0x180881f8 ||
                          indexed || shader == 0x18087984;
    const bool gradient = shader == 0x180912fc || shader == 0x18091374 || shader == 0x180914dc;
    const bool opaque = shader == 0x180912fc || shader == 0x18091168;
    if (!textured && !gradient && shader != 0x180911a8 && shader != 0x1809125c &&
        shader != 0x18091168) {
        if (shader && unsupported.insert(shader).second)
            SDL_Log("Phase HD: preserving native draw type %08x", shader);
        return false;
    }
    SDL_Texture* image = textured ? texture(context, indexed) : nullptr;
    if (textured && !image)
        return false;
    SDL_Vertex points[3]{};
    for (unsigned i = 0; i < 3; ++i) {
        const auto v = vertices + i * 20;
        points[i].position = {fixed(ipod::ld32(v)), fixed(ipod::ld32(v + 4))};
        const auto colors = gradient ? ipod::ld32(v + 16) : context;
        if (gradient && !colors)
            return false;
        points[i].color = {color(ipod::ld32(colors)), color(ipod::ld32(colors + 4)),
                           color(ipod::ld32(colors + 8)),
                           opaque ? 1.0f : color(ipod::ld32(colors + 12))};
        if (shader == 0x1808751c || shader == 0x180881f8 || indexed)
            points[i].color = {1, 1, 1, 1};
        if (textured) {
            const auto uv = ipod::ld32(v + 12);
            if (!uv)
                return false;
            points[i].tex_coord = {fixed(ipod::ld32(uv)) / float(image->w),
                                   fixed(ipod::ld32(uv + 4)) / float(image->h)};
#if !SDL_VERSION_ATLEAST(3, 4, 0)
            if (points[i].tex_coord.x < 0 || points[i].tex_coord.x > 1 ||
                points[i].tex_coord.y < 0 || points[i].tex_coord.y > 1)
                return false;
#endif
        }
    }
    if (!target_canvas() || !sync_native())
        return false;
    SDL_Rect clip{int(ipod::ld32(context + 0x7c)), int(ipod::ld32(context + 0x78)), 0, 0};
    clip.w = int(ipod::ld32(context + 0x84)) - clip.x;
    clip.h = int(ipod::ld32(context + 0x80)) - clip.y;
#if SDL_VERSION_ATLEAST(3, 4, 0)
    // The original shaders clamp texel coordinates, including outside the image.
    SDL_TextureAddressMode old_u, old_v;
    if (!checked(SDL_GetRenderTextureAddressMode(renderer, &old_u, &old_v)) ||
        !checked(SDL_SetRenderTextureAddressMode(renderer, SDL_TEXTURE_ADDRESS_CLAMP,
                                                 SDL_TEXTURE_ADDRESS_CLAMP)))
        return false;
#endif
    if (image && !checked(SDL_SetTextureBlendMode(image, additive ? SDL_BLENDMODE_ADD
                                                                  : SDL_BLENDMODE_BLEND)))
        return false;
    const bool ok =
        checked(SDL_SetRenderClipRect(renderer, &clip)) &&
        checked(SDL_SetRenderDrawBlendMode(renderer, opaque     ? SDL_BLENDMODE_NONE
                                                     : additive ? SDL_BLENDMODE_ADD
                                                                : SDL_BLENDMODE_BLEND)) &&
        checked(SDL_RenderGeometry(renderer, image, points, 3, nullptr, 0));
#if SDL_VERSION_ATLEAST(3, 4, 0)
    checked(SDL_SetRenderTextureAddressMode(renderer, old_u, old_v));
#endif
    if (ok)
        ++triangles;
    SDL_SetRenderTarget(renderer, nullptr);
    return ok;
}
bool blit(const ipod::Cpu& cpu, uint32_t kind) {
    if (!renderer || render_scale == 1 || failed || cpu.r[0] < framebuffer ||
        cpu.r[0] >= framebuffer + width * height * 2 || cpu.r[3] != width || cpu.r[1] <= cpu.r[0] ||
        cpu.r[2] <= cpu.r[0])
        return false;
    // Build 3168002's blitters share these flags: disabled blending overwrites
    // RGB regardless of alpha; otherwise the second flag selects saturated add.
    const auto state = ipod::ld32(0x18011b4c);
    if (!state)
        return false;
    const auto blend = !ipod::ld8(state + 0x848)  ? SDL_BLENDMODE_NONE
                       : ipod::ld8(state + 0x849) ? SDL_BLENDMODE_ADD
                                                  : SDL_BLENDMODE_BLEND;
    const auto argument = [&](unsigned i) {
        return i < 4 ? cpu.r[i] : ipod::ld32(cpu.r[ipod::SP] + (i - 4) * 4);
    };
    const unsigned w = (cpu.r[1] - cpu.r[0]) / 2;
    const unsigned h = (cpu.r[2] - cpu.r[0]) / (width * 2);
    const unsigned x = ((cpu.r[0] - framebuffer) / 2) % width;
    const unsigned y = ((cpu.r[0] - framebuffer) / 2) / width;
    if (!w || !h || x + w > width || y + h > height || argument(4) + w != width ||
        (cpu.r[2] - cpu.r[0]) % (width * 2))
        return false;
    const bool indexed = kind == 0x1801ef00;
    const bool rgba8 = kind == 0x1801968c || kind == 0x18014d08;
    const bool rgba5551 = kind == 0x18014a2c;
    const bool scaled = indexed || kind == 0x1801968c;
    if (!indexed && !rgba8 && !rgba5551 && kind != 0x18011944 && kind != 0x180146f8)
        return false;
    // The indexed blitter also draws Phase's font sheets. Its final arguments
    // are source-texel steps, not output dimensions: the original often throws
    // away more than half the glyph detail when sampling into the 320x240 frame.
    // Recover the complete source crop and let the HD canvas sample it directly.
    unsigned source_width = w, source_height = h;
    int direction_x = 1, direction_y = 1;
    if (scaled) {
        const int64_t step_x = int32_t(argument(9)), step_y = int32_t(argument(10));
        if (!step_x || !step_y || std::abs(step_x) > 16 * 65536 || std::abs(step_y) > 16 * 65536)
            return false;
        direction_x = step_x < 0 ? -1 : 1;
        direction_y = step_y < 0 ? -1 : 1;
        source_width = unsigned((uint64_t(w) * std::abs(step_x) + 65535) >> 16);
        source_height = unsigned((uint64_t(h) * std::abs(step_y) + 65535) >> 16);
        if (source_width > 2048 || source_height > 2048)
            return false;
    }
    const auto source = argument(6);
    const auto palette = indexed ? argument(5) : 0;
    const int64_t stride =
        scaled ? int32_t(argument(8)) : int64_t(w) * (rgba8 ? 4 : 2) + int32_t(argument(8));
    if (!source || std::abs(stride) > 16384 ||
        (indexed && (!palette || argument(7) > 1 || source_width > stride)) ||
        (scaled && rgba8 && int64_t(source_width) * 4 > stride))
        return false;
    const bool cache_indexed = indexed && direction_x > 0 && direction_y > 0;
    const IndexedBlitKey key{source, source_width, source_height, unsigned(stride),
                             indexed ? argument(7) : 0};
    const auto source_bytes =
        cache_indexed
            ? unsigned(((source_height - 1) * stride + source_width + argument(7) + 1) / 2)
            : 0;
    const auto* raw = cache_indexed ? ipod::guest_pointer(source, source_bytes) : nullptr;
    const auto* raw_palette = cache_indexed ? ipod::guest_pointer(palette, 32) : nullptr;
    SDL_Texture* image = nullptr;
    if (cache_indexed) {
        const auto found = indexed_blits.find(key);
        if (found != indexed_blits.end() && found->second.source.size() == source_bytes &&
            !std::memcmp(found->second.source.data(), raw, source_bytes) &&
            !std::memcmp(found->second.palette.data(), raw_palette, 32))
            image = found->second.texture;
    }
    if (!image) {
        if (cache_indexed &&
            (indexed_blits.size() >= 512 || texture_bytes + source_bytes > texture_budget))
            clear_textures();
        std::vector<uint8_t> rgba(source_width * source_height * 4);
        for (unsigned row = 0; row < source_height; ++row)
            for (unsigned column = 0; column < source_width; ++column) {
                std::array<uint8_t, 4> p;
                if (rgba8) {
                    auto* bytes =
                        ipod::guest_pointer(uint32_t(source + int64_t(row) * direction_y * stride +
                                                     int64_t(column) * direction_x * 4),
                                            4);
                    std::copy(bytes, bytes + 4, p.begin());
                    // 18014d08's opaque-texel fast branch ignores tint alpha;
                    // its partial-alpha branch multiplies by it (18014df0).
                    if (kind == 0x18014d08 && p[3] != 255)
                        p[3] = uint8_t(float(p[3]) * current_color.a);
                } else {
                    uint16_t packed;
                    if (indexed) {
                        const auto offset = int64_t(row) * direction_y * stride +
                                            int64_t(column) * direction_x + argument(7);
                        const auto byte_offset =
                            offset / 2 - (offset < 0 && offset % 2 != 0 ? 1 : 0);
                        const auto byte = ipod::ld8(uint32_t(source + byte_offset));
                        const auto index = (offset & 1) ? byte & 15 : byte >> 4;
                        packed = uint16_t(ipod::ld16(palette + index * 2));
                    } else
                        packed = uint16_t(
                            ipod::ld16(uint32_t(source + int64_t(row) * stride + column * 2)));
                    p = rgba5551 ? std::array<uint8_t, 4>{uint8_t((packed >> 11) * 255 / 31),
                                                          uint8_t(((packed >> 6) & 31) * 255 / 31),
                                                          uint8_t(((packed >> 1) & 31) * 255 / 31),
                                                          uint8_t((packed & 1) * 255)}
                                 : rgba4444(packed);
                }
                std::copy(p.begin(), p.end(), rgba.begin() + (row * source_width + column) * 4);
            }
        uint64_t hash = 14695981039346656037ULL;
        for (auto byte : rgba) {
            hash ^= byte;
            hash *= 1099511628211ULL;
        }
        hash ^= uint64_t(source_width) << 32 | source_height;
        if (!blit_textures.count(hash) &&
            (blit_textures.size() >= 512 || texture_bytes + rgba.size() > texture_budget))
            clear_textures();
        image = blit_textures[hash];
        if (!image) {
            image = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC,
                                      int(source_width), int(source_height));
            blit_textures[hash] = image;
            texture_bytes += rgba.size();
            if (!checked(image != nullptr) ||
                !checked(SDL_UpdateTexture(image, nullptr, rgba.data(), int(source_width * 4))) ||
                !checked(SDL_SetTextureScaleMode(image, SDL_SCALEMODE_LINEAR)) ||
                !checked(SDL_SetTextureBlendMode(image, SDL_BLENDMODE_BLEND)))
                return false;
        }
        if (cache_indexed) {
            auto& cached = indexed_blits[key];
            texture_bytes -= cached.source.size();
            cached.texture = image;
            cached.source.assign(raw, raw + source_bytes);
            cached.palette.assign(raw_palette, raw_palette + 32);
            texture_bytes += cached.source.size();
        }
    }
    auto tint = kind == 0x18011944 || kind == 0x1801968c ? SDL_FColor{1, 1, 1, 1} : current_color;
    // Tinted RGBA8 applies its native per-texel alpha rule while decoding above.
    if (kind == 0x18014d08)
        tint.a = 1;
    if (!target_canvas() || !sync_native() || !checked(SDL_SetRenderClipRect(renderer, nullptr)) ||
        !checked(SDL_SetTextureColorModFloat(image, tint.r, tint.g, tint.b)) ||
        !checked(SDL_SetTextureBlendMode(image, blend)) ||
        !checked(SDL_SetTextureAlphaModFloat(image, tint.a)))
        return false;
    SDL_FRect rect{float(x), float(y), float(w), float(h)};
    const bool ok = checked(SDL_RenderTexture(renderer, image, nullptr, &rect));
    SDL_SetRenderTarget(renderer, nullptr);
    return ok;
}
}  // namespace

void configure(SDL_Renderer* supplied, unsigned requested_scale) {
    reset();
    renderer = supplied;
    render_scale = std::clamp(requested_scale, 1u, 4u);
    if (render_scale == 1)
        return;
    canvas = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET,
                               int(width * render_scale), int(height * render_scale));
    patch = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, width,
                              height);
    if (!checked(canvas && patch) ||
        !checked(SDL_SetTextureBlendMode(canvas, SDL_BLENDMODE_NONE)) ||
        !checked(SDL_SetTextureBlendMode(patch, SDL_BLENDMODE_BLEND)) ||
        !checked(SDL_SetTextureScaleMode(patch, SDL_SCALEMODE_NEAREST)) || !target_canvas())
        return;
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    checked(SDL_RenderClear(renderer));
    SDL_SetRenderTarget(renderer, nullptr);
    remember_native();
}
void reset() {
    if (renderer && render_scale > 1) {
        SDL_Log("Phase HD draws: triangles=%llu blits=%llu native-patches=%llu",
                (unsigned long long)triangles, (unsigned long long)blits,
                (unsigned long long)native_patches);
        for (const auto& entry : native_blits)
            SDL_Log("Phase HD native blit %08x: %llu draws", entry.first,
                    (unsigned long long)entry.second);
    }
    clear_textures();
    SDL_DestroyTexture(canvas);
    SDL_DestroyTexture(patch);
    canvas = patch = nullptr;
    renderer = nullptr;
    render_scale = 1;
    failed = logged = false;
    triangles = 0;
    blits = native_patches = 0;
    native_blits.clear();
    unsupported.clear();
    current_color = {1, 1, 1, 1};
}
void set_color(float r, float g, float b, float a) {
    current_color = {std::clamp(r, 0.0f, 1.0f), std::clamp(g, 0.0f, 1.0f),
                     std::clamp(b, 0.0f, 1.0f), std::clamp(a, 0.0f, 1.0f)};
}
unsigned scale() {
    return failed ? 1 : render_scale;
}
SDL_Texture* finish(unsigned brightness_percent) {
    if (render_scale == 1 || failed || !canvas)
        return nullptr;
    if (!target_canvas() || !sync_native())
        return nullptr;
    SDL_SetRenderTarget(renderer, nullptr);
    const auto brightness = uint8_t(std::min(brightness_percent, 100u) * 255 / 100);
    if (!checked(SDL_SetTextureColorMod(canvas, brightness, brightness, brightness)))
        return nullptr;
    if (!logged && triangles) {
        SDL_Log("Phase HD: %ux%u GPU canvas; native gameplay coordinates preserved",
                width * render_scale, height * render_scale);
        logged = true;
    }
    return canvas;
}
TriangleScope::TriangleScope(const ipod::Cpu& cpu) : captured_(triangle(cpu)) {}
TriangleScope::~TriangleScope() {
    if (captured_)
        remember_native();
}
BlitScope::BlitScope(const ipod::Cpu& cpu, uint32_t kind) : captured_(blit(cpu, kind)) {
    if (renderer && render_scale > 1 && !failed && cpu.r[0] >= framebuffer &&
        cpu.r[0] < framebuffer + width * height * 2) {
        if (captured_)
            ++blits;
        else
            ++native_blits[kind];
    }
}
BlitScope::~BlitScope() {
    if (captured_)
        remember_native();
}
}  // namespace phase::hd
