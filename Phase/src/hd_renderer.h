#pragma once
#include <cstdint>

struct SDL_Renderer;
struct SDL_Texture;
namespace ipod {
struct Cpu;
}

namespace phase::hd {
// Observes the original renderer. Never changes guest memory or registers.
void configure(SDL_Renderer* renderer, unsigned scale);
void reset();
SDL_Texture* finish(unsigned brightness_percent);
unsigned scale();
void set_color(float r, float g, float b, float a);
class TriangleScope {
public:
    explicit TriangleScope(const ipod::Cpu& cpu);
    ~TriangleScope();
    TriangleScope(const TriangleScope&) = delete;
    TriangleScope& operator=(const TriangleScope&) = delete;

private:
    bool captured_ = false;
};
class BlitScope {
public:
    BlitScope(const ipod::Cpu& cpu, uint32_t kind);
    ~BlitScope();
    BlitScope(const BlitScope&) = delete;
    BlitScope& operator=(const BlitScope&) = delete;

private:
    bool captured_ = false;
};
}  // namespace phase::hd
