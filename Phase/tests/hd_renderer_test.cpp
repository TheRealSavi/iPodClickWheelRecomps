// Authored draw commands only; no original game image, assets, or pixels.
#include "hd_renderer.h"
#include "runtime/cpu.h"
#include "runtime/memory.h"

#include <SDL3/SDL.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
std::array<uint8_t, 1024 * 1024> ram{}, image{};
void check(bool ok, const char* reason) {
    if (!ok) {
        std::fprintf(stderr, "FAIL %s: %s\n", reason, SDL_GetError());
        std::exit(1);
    }
}
constexpr uint32_t fb = 0x11010000, context = 0x11001000, vertices = 0x11002000;
void vertex(unsigned i, float x, float y) {
    ipod::st32(vertices + i * 20, uint32_t(x * 65536));
    ipod::st32(vertices + i * 20 + 4, uint32_t(y * 65536));
}
}  // namespace
namespace ipod {
uint32_t watch_address = 0;
void watch_hit(uint32_t, uint32_t) {
    std::abort();
}
uint8_t* guest_pointer(uint32_t address, uint32_t length) {
    if (address >= 0x11000000 && uint64_t(address) + length <= 0x11000000 + ram.size())
        return ram.data() + address - 0x11000000;
    if (address >= 0x18000000 && uint64_t(address) + length <= 0x18000000 + image.size())
        return image.data() + address - 0x18000000;
    check(false, "observer read outside authored fixture");
    return nullptr;
}
}  // namespace ipod
int main(int argc, char** argv) {
    check(SDL_Init(SDL_INIT_VIDEO), "init");
    auto* window = SDL_CreateWindow("HD observer test", 320, 240, SDL_WINDOW_HIDDEN);
    auto* renderer = SDL_CreateRenderer(window, argc > 1 ? argv[1] : "software");
    check(renderer != nullptr, "renderer");
    phase::hd::configure(renderer, 1);
    check(!phase::hd::finish(100), "1x bypasses observer");
    phase::hd::configure(renderer, 4);
    ipod::Cpu cpu;
    cpu.r[0] = context;
    cpu.r[1] = fb;
    cpu.r[2] = vertices;
    ipod::st32(context, 65536);
    ipod::st32(context + 12, 65536);
    ipod::st32(context + 0x6c, 0x180911a8);
    ipod::st32(context + 0x80, 240);
    ipod::st32(context + 0x84, 320);
    vertex(0, 10.25f, 10.25f);
    vertex(1, 30.25f, 10.25f);
    vertex(2, 10.25f, 30.25f);
    static const auto original_ram = ram;
    const auto original_cpu = cpu;
    { phase::hd::TriangleScope draw(cpu); }
    check(ram == original_ram && !std::memcmp(&cpu, &original_cpu, sizeof(cpu)),
          "observer leaves guest state unchanged");
    auto sample = [&](int x, int y) {
        auto* canvas = phase::hd::finish(100);
        check(canvas && canvas->w == 1280 && canvas->h == 960, "HD dimensions");
        check(SDL_SetRenderTarget(renderer, canvas), "read HD target");
        SDL_Rect rect{x, y, 1, 1};
        auto* pixels = SDL_RenderReadPixels(renderer, &rect);
        std::array<Uint8, 4> rgba{};
        check(pixels && SDL_ReadSurfacePixel(pixels, 0, 0, &rgba[0], &rgba[1], &rgba[2], &rgba[3]),
              "HD sample");
        SDL_DestroySurface(pixels);
        SDL_SetRenderTarget(renderer, nullptr);
        return rgba;
    };
    check(sample(42, 42)[0] == 255 && sample(40, 42)[0] == 0, "quarter-pixel edge survives at 4x");
    // A native operation after the geometry must cover it, without erasing the rest.
    ipod::st16(fb + (11 * 320 + 11) * 2, 0x07e0);
    check(sample(45, 45)[1] == 255 && sample(49, 45)[0] == 255,
          "native fallback preserves ordering");
    // The destructor remembers original drawing so it cannot flatten the HD replay.
    {
        phase::hd::TriangleScope draw(cpu);
        ipod::st16(fb + (11 * 320 + 11) * 2, 0xf800);
    }
    check(sample(45, 45)[0] == 255, "accepted native raster is not overlaid");
    ipod::st32(context + 0x7c, 15);
    ipod::st32(context, 0);
    ipod::st32(context + 8, 65536);
    { phase::hd::TriangleScope draw(cpu); }
    check(sample(45, 45)[0] == 255 && sample(65, 45)[2] == 255,
          "logical clipping scales correctly");
    // Alpha-interpolated colors and the gameplay texture/glow variants.
    ipod::st32(context + 0x7c, 0);
    ipod::st32(context + 0x6c, 0x18091374);
    for (unsigned i = 0; i < 3; ++i) {
        ipod::st32(vertices + i * 20 + 16, 0x11007000);
        ipod::st32(vertices + i * 20 + 12, 0x11007100);
    }
    ipod::st32(0x11007004, 65536);
    ipod::st32(0x1100700c, 32768);
    { phase::hd::TriangleScope draw(cpu); }
    auto blended = sample(45, 45);
    check(blended[0] >= 120 && blended[0] <= 135 && blended[1] >= 120 && blended[1] <= 135,
          "gradient vertex alpha blends over previous geometry");
    ipod::st32(context + 0x18, 0x11007200);
    ipod::st32(context + 0x1c, 1);
    ipod::st32(context + 0x20, 1);
    ipod::st32(context + 0x24, 0x11007300);
    ipod::st32(context, 65536);
    ipod::st32(context + 4, 65536);
    ipod::st32(context + 8, 65536);
    ipod::st16(0x11007200, 0x00ff);
    ipod::st32(context + 0x6c, 0x180881f8);
    { phase::hd::TriangleScope draw(cpu); }
    check(sample(45, 45)[2] > 230 && sample(45, 45)[1] == 0, "filtered RGBA texture");
    ipod::st16(0x11007200, 0x0f0f);
    ipod::st32(context + 0x6c, 0x18087984);
    { phase::hd::TriangleScope draw(cpu); }
    check(sample(45, 45)[2] > 230 && sample(45, 45)[1] > 230, "additive glow retains destination");
    ipod::st8(0x11007200, 0x10);
    ipod::st32(0x11007304, 0xf800ff);
    ipod::st32(context + 0x6c, 0x1808a7e0);
    { phase::hd::TriangleScope draw(cpu); }
    check(sample(45, 45)[0] == 255 && sample(45, 45)[2] == 0, "filtered indexed palette texture");
    // Source-alpha RGBA4444 blit, then changed source at the same address.
    constexpr uint32_t state = 0x11004000, source = 0x11005000;
    ipod::st32(0x18011b4c, state);
    ipod::st8(state + 0x848, 1);
    cpu.r[0] = fb + (40 * 320 + 40) * 2;
    cpu.r[1] = cpu.r[0] + 2;
    cpu.r[2] = cpu.r[0] + 640;
    cpu.r[3] = 320;
    cpu.r[ipod::SP] = 0x11006000;
    ipod::st32(cpu.r[ipod::SP], 319);
    ipod::st32(cpu.r[ipod::SP] + 8, source);
    ipod::st16(source, 0xf00f);
    { phase::hd::BlitScope draw(cpu, 0x18011944); }
    check(sample(161, 161)[0] > 230, "direct texture blit");
    ipod::st16(source, 0x00ff);
    { phase::hd::BlitScope draw(cpu, 0x18011944); }
    check(sample(161, 161)[2] > 230 && sample(161, 161)[0] == 0,
          "source updates invalidate texture cache");
    ipod::st8(state + 0x849, 1);
    ipod::st16(source, 0xf00f);
    { phase::hd::BlitScope draw(cpu, 0x18011944); }
    check(sample(161, 161)[2] > 230 && sample(161, 161)[0] > 230,
          "additive blits preserve destination and add source");
    // A font crop has more source detail than the original output. The thin
    // stroke at source x=1 is entirely missed by the native 2-texel step.
    // Use an odd nibble origin and odd sheet pitch to catch row/crop mistakes.
    ipod::st8(state + 0x849, 0);
    constexpr uint32_t palette = 0x11008000, sheet = 0x11008100;
    ipod::st16(palette, 0xfff0);
    ipod::st16(palette + 2, 0xffff);
    for (unsigned row = 0; row < 4; ++row) {
        const auto offset = row * 7 + 2;
        ipod::st8(sheet + offset / 2, ipod::ld8(sheet + offset / 2) | ((offset & 1) ? 1 : 0x10));
    }
    cpu.r[0] = fb + (50 * 320 + 50) * 2;
    cpu.r[1] = cpu.r[0] + 4;
    cpu.r[2] = cpu.r[0] + 1280;
    ipod::st32(cpu.r[ipod::SP], 318);
    ipod::st32(cpu.r[ipod::SP] + 4, palette);
    ipod::st32(cpu.r[ipod::SP] + 8, sheet);
    ipod::st32(cpu.r[ipod::SP] + 12, 1);
    ipod::st32(cpu.r[ipod::SP] + 16, 7);
    ipod::st32(cpu.r[ipod::SP] + 20, 2 * 65536);
    ipod::st32(cpu.r[ipod::SP] + 24, 2 * 65536);
    static const auto before_font = ram;
    { phase::hd::BlitScope draw(cpu, 0x1801ef00); }
    check(sample(202, 202)[0] > 140 && sample(202, 206)[0] > 140 && sample(200, 202)[0] == 0,
          "HD glyph retains stroke lost by native downsampling, including odd pitch");
    check(ram == before_font, "HD font leaves original guest data unchanged");
    const auto font_pixel = sample(202, 202);
    { phase::hd::BlitScope draw(cpu, 0x1801ef00); }
    check(sample(202, 202)[0] >= font_pixel[0], "cached glyph redraw");
    // Palette changes at the same guest address must invalidate the cache.
    ipod::st16(palette + 2, 0x0f0f);
    cpu.r[0] += 20;
    cpu.r[1] += 20;
    cpu.r[2] += 20;
    { phase::hd::BlitScope draw(cpu, 0x1801ef00); }
    check(sample(242, 202)[1] > 140 && sample(242, 202)[0] < 100,
          "glyph palette update invalidates cache");
    // A reused sheet can change while the address, palette and dimensions remain.
    for (unsigned i = 0; i < 16; ++i)
        ipod::st8(sheet + i, 0x11);
    cpu.r[0] += 20;
    cpu.r[1] += 20;
    cpu.r[2] += 20;
    { phase::hd::BlitScope draw(cpu, 0x1801ef00); }
    check(sample(280, 202)[1] > 230, "glyph source update invalidates cache");
    ipod::st32(cpu.r[ipod::SP] + 20, 0xffff0000);
    cpu.r[0] += 20;
    cpu.r[1] += 20;
    cpu.r[2] += 20;
    { phase::hd::BlitScope draw(cpu, 0x1801ef00); }
    check(sample(320, 202)[1] > 230, "mirrored indexed blit");

    // Flat glows below a single RGB565 red step must survive and accumulate.
    phase::hd::configure(renderer, 4);
    cpu.r[0] = context;
    cpu.r[1] = fb;
    cpu.r[2] = vertices;
    ipod::st32(context, 655);
    ipod::st32(context + 4, 0);
    ipod::st32(context + 8, 0);
    ipod::st32(context + 12, 65536);
    ipod::st32(context + 0x6c, 0x1809125c);
    for (unsigned i = 0; i < 10; ++i) {
        phase::hd::TriangleScope draw(cpu);
    }
    const auto faint_glow = sample(45, 45);
    check(faint_glow[0] >= 15 && faint_glow[0] <= 35 && !faint_glow[1],
          "sub-RGB565 flat glow accumulates in RGBA8");
    ipod::st32(context + 0x6c, 0x180914dc);
    { phase::hd::TriangleScope draw(cpu); }
    check(sample(45, 45)[0] == faint_glow[0] && sample(45, 45)[1] >= 120,
          "gradient glow adds vertex color and alpha without dimming destination");
    ipod::st32(context + 0x6c, 0x18091168);
    ipod::st32(context, 13107);
    ipod::st32(context + 12, 0);
    { phase::hd::TriangleScope draw(cpu); }
    check(sample(45, 45)[0] >= 49 && sample(45, 45)[0] <= 52 && !sample(45, 45)[1],
          "opaque flat fill ignores alpha and replaces destination");

    auto setup_blit = [&](unsigned x, unsigned y, unsigned w, unsigned h) {
        cpu.r[0] = fb + (y * 320 + x) * 2;
        cpu.r[1] = cpu.r[0] + w * 2;
        cpu.r[2] = cpu.r[0] + h * 640;
        cpu.r[3] = 320;
        ipod::st32(cpu.r[ipod::SP], 320 - w);
        ipod::st32(cpu.r[ipod::SP] + 8, source);
        ipod::st32(cpu.r[ipod::SP] + 16, 0);
        ipod::st8(state + 0x848, 1);
        ipod::st8(state + 0x849, 0);
        phase::hd::set_color(1, 1, 1, 1);
    };
    setup_blit(80, 80, 1, 1);
    ipod::st16(source, 0x111f);
    { phase::hd::BlitScope draw(cpu, 0x18011944); }
    check(sample(321, 321)[0] == 17 && sample(321, 321)[1] == 17 && sample(321, 321)[2] == 17,
          "RGBA4444 channels expand directly without RGB565 reduction");
    ipod::st16(source, 0xf000);  // Alpha zero still writes RGB when blending is off.
    ipod::st8(state + 0x848, 0);
    { phase::hd::BlitScope draw(cpu, 0x18011944); }
    check(sample(321, 321)[0] == 255 && !sample(321, 321)[1], "opaque bitmap ignores source alpha");
    ipod::st8(state + 0x848, 1);
    ipod::st16(source, 0x0008);
    { phase::hd::BlitScope draw(cpu, 0x18011944); }
    check(sample(321, 321)[0] >= 115 && sample(321, 321)[0] <= 122,
          "bitmap fade after opaque draw restores alpha blend");

    // Scaled RGBA8 effects keep all source channels, in normal and mirrored order.
    setup_blit(90, 80, 1, 1);
    ipod::st32(source, 0xff0000ff);
    ipod::st32(source + 4, 0xffff0000);
    ipod::st32(cpu.r[ipod::SP] + 16, 8);
    ipod::st32(cpu.r[ipod::SP] + 20, 131072);
    ipod::st32(cpu.r[ipod::SP] + 24, 65536);
    { phase::hd::BlitScope draw(cpu, 0x1801968c); }
    check(sample(360, 321)[0] > 240 && sample(363, 321)[2] > 240,
          "scaled RGBA8 retains both source texels");
    cpu.r[0] += 20;
    cpu.r[1] += 20;
    cpu.r[2] += 20;
    ipod::st32(cpu.r[ipod::SP] + 8, source + 4);
    ipod::st32(cpu.r[ipod::SP] + 20, uint32_t(-131072));
    { phase::hd::BlitScope draw(cpu, 0x1801968c); }
    check(sample(400, 321)[2] > 240 && sample(403, 321)[0] > 240,
          "scaled RGBA8 mirror retains orientation");

    // Direct RGBA8 has a byte gap after each row; negative pitch flips rows.
    setup_blit(110, 80, 1, 2);
    ipod::st32(cpu.r[ipod::SP] + 8, source + 4);
    ipod::st32(cpu.r[ipod::SP] + 16, uint32_t(-8));
    phase::hd::set_color(0.5f, 1, 1, 0);  // Native routine intentionally ignores tint alpha.
    { phase::hd::BlitScope draw(cpu, 0x18014d08); }
    check(sample(441, 321)[2] > 240 && sample(441, 326)[0] >= 120,
          "tinted RGBA8 respects signed row gap and source-only alpha");
    setup_blit(120, 80, 1, 1);
    ipod::st32(source, 0x800000ff);
    phase::hd::set_color(0.5f, 1, 1, 0.5f);
    { phase::hd::BlitScope draw(cpu, 0x18014d08); }
    check(sample(481, 321)[0] >= 29 && sample(481, 321)[0] <= 34,
          "partial-alpha RGBA8 texels also multiply tint alpha");
    setup_blit(125, 80, 1, 1);
    ipod::st16(source, 0xf801);
    phase::hd::set_color(1, 1, 1, 0.5f);
    { phase::hd::BlitScope draw(cpu, 0x18014a2c); }
    check(sample(501, 321)[0] >= 120 && sample(501, 321)[0] <= 130,
          "RGBA5551 cutout honors tint alpha");
    phase::hd::reset();
    check(!phase::hd::finish(100), "reset disables observer");
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    std::puts(
        "PASS HD subpixels, clipping, fallback ordering, source updates, guest state preservation");
}
