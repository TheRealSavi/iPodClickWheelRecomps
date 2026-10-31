// The desktop platform on SDL3: a scaled window for the 320×240 screen, the keyboard as the click
// wheel, and SDL audio streams for both the sound effects and the music.
//
// The keyboard is the only way in. A mouse wheel and a trackpad were once read as the click wheel
// as well, and are not any more: what a trackpad sends cannot be told apart from what a swipe up
// the pad sends by accident (a quarter of the travel of an upward swipe arrives on the sideways
// axis, and its tail arrives on that axis alone), so a gesture nobody bound kept turning the menu.
// Every control is now one a player can see and change in Settings ▸ Input.
//
// The defaults, all rebindable:
//   ← / →                  turn the click wheel (one row a press)
//   Space                  Select (centre button)
//   ↑                      Menu    [  Previous    ↓  Play/Pause    ]  Next
//   P                      screenshot  Q  quit
//   - / =                  step the window through whole multiples; F or F11 full screen
//
#include "ipod/platform/sdl3/presenter.h"
#include "platform/input_bindings.h"
#include "platform/platform.h"
#include "platform/sdl3/music_decoder.h"
#include "platform/sdl3/settings_window.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace bowling::platform {

namespace {

constexpr int WINDOW_SCALE = 3;
// The wheel has 120 detents to a turn and a menu moves one row every eight of them (the oracle
// scripts turn it in eights for exactly this reason), so a key press is worth a row. One detent a
// press meant eight presses per letter.
constexpr int DETENTS_PER_ROW = 8;

// The modifier that, with comma, opens the settings window: the Command key on a Mac, where
// ⌘, is what every application uses, and Control elsewhere.
#if defined(__APPLE__)
constexpr SDL_Keymod SETTINGS_MODIFIER = SDL_KMOD_GUI;
#else
constexpr SDL_Keymod SETTINGS_MODIFIER = SDL_KMOD_CTRL;
#endif

// The keys offered in the settings window. F, F11, L, P and Q are left out: they are the window's
// own and the program's, and a player who bound one would lose the shortcut. Escape is offered —
// it is the Menu button by default, which is what a player reaches for to back out of a screen.
constexpr unsigned ASSIGNABLE_KEY_COUNT = 48;
// A ceiling on Sharp's intermediate texture: eight times 320x240 is 2560x1920, enough to cover
// any screen worth playing this on, and a bound on what a wildly resized window can ask for.
constexpr int MAX_PRESCALE = 8;

// BOWLING_TRACE_INPUT=1 prints every key the window receives, with what it is bound to. What a
// device actually sends differs by device and by the system's own settings, and there is no other
// way to see it from here; three rounds of guessing at a rebinding fault taught that lesson.
bool trace_input() {
    static const bool on = SDL_getenv("BOWLING_TRACE_INPUT") != nullptr;
    return on;
}

// BOWLING_TRACE_AUDIO=1 prints what the audio is asked to do — the track opened and its shape, the
// loops, the stops, the volume. Sound that does not come out has no other symptom, and there is
// nothing on screen to read: this is the difference between seeing the answer and guessing at it,
// as the input trace above it is. It came with the music player from the Mini Golf recomp.
bool trace_audio() {
    static const bool on = SDL_getenv("BOWLING_TRACE_AUDIO") != nullptr;
    return on;
}
constexpr size_t VOICE_LIMIT = 4;                 // the device's sound-effect polyphony
constexpr Uint64 TITLE_REFRESH_NS = 500'000'000;  // how often the frame rate in the title updates

// A sound effect playing on its own SDL audio stream.
// A sound effect's samples, read from its .wav once. The game plays the same handful of sounds
// over and over — every menu row moved is one — so the file is read once and kept.
// The game's own format, as SDL spells it. It uses the two the hardware took — eight-bit
// unsigned and sixteen-bit signed — and says which in the sound's description.
bool spec_for(const SoundClip& clip, SDL_AudioSpec& spec) {
    if (clip.bits != 8 && clip.bits != 16) {
        return false;
    }
    spec.format = clip.bits == 8 ? SDL_AUDIO_U8 : SDL_AUDIO_S16LE;
    spec.channels = static_cast<int>(clip.channels);
    spec.freq = static_cast<int>(clip.sample_rate);
    return spec.channels > 0 && spec.freq > 0;
}

// One sound playing, on an audio stream of its own.
//
// The stream is opened once and kept for the life of the program, which is the whole point of
// this class: opening one costs about 15 ms — most of a frame at 60 Hz — while putting samples
// into one that is already open costs nothing measurable. A voice that has finished goes quiet
// and waits to be used again rather than being destroyed.
class Voice {
public:
    ~Voice() { close(); }

    // Give the stream back to SDL, and be able to say when.
    //
    // A destructor is too late. Members are destroyed *after* the body of the destructor that
    // owns them, and that body is where `SDL_Quit` is — so a voice left to clean itself up
    // destroys its stream after the audio subsystem it belongs to has gone, and dereferences
    // freed memory on the way out of the program. It is a crash that only happens once a sound
    // has actually played, which is why it went unseen: a voice that never opened a stream has
    // nothing to destroy.
    void close() {
        if (stream_ != nullptr) {
            SDL_DestroyAudioStream(stream_);
            stream_ = nullptr;
        }
        samples_.clear();
    }

    Voice() = default;
    Voice(const Voice&) = delete;
    Voice& operator=(const Voice&) = delete;

    [[nodiscard]] bool idle() const { return samples_.empty(); }
    [[nodiscard]] uint32_t sounding() const { return voice_; }

    // Start (or restart) this voice on `clip`. False if there is no audio device to play it on.
    //
    // The samples are copied rather than referred to: the buffer belongs to the game, which is
    // free to load something else over it the moment this returns, and a looping voice has to be
    // able to put them in again later.
    bool start(uint32_t voice, const SoundClip& clip, bool looping) {
        SDL_AudioSpec spec{};
        if (!spec_for(clip, spec)) {
            std::fprintf(stderr, "sound: voice %u has no format to play (%u-bit, %u channel)\n",
                         voice, clip.bits, clip.channels);
            return false;
        }
        if (stream_ == nullptr) {
            stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr,
                                                nullptr);
            if (stream_ == nullptr) {
                std::fprintf(stderr, "sound: no audio device: %s\n", SDL_GetError());
                return false;
            }
        } else if (!SDL_SetAudioStreamFormat(stream_, &spec, nullptr)) {
            // A sound in a format the open stream cannot take: rare, and not worth a new device.
            std::fprintf(stderr, "sound: cannot play voice %u: %s\n", voice, SDL_GetError());
            return false;
        }
        SDL_ClearAudioStream(stream_);
        voice_ = voice;
        samples_.assign(clip.samples, clip.samples + clip.bytes);
        looping_ = looping;
        SDL_PutAudioStreamData(stream_, samples_.data(), static_cast<int>(samples_.size()));
        SDL_ResumeAudioStreamDevice(stream_);
        return true;
    }

    void stop() {
        if (stream_ != nullptr) {
            SDL_ClearAudioStream(stream_);
        }
        samples_.clear();
    }

    // A looping voice refills its queue; a one-shot voice falls idle when drained.
    void service() {
        if (samples_.empty() || stream_ == nullptr) {
            return;
        }
        if (SDL_GetAudioStreamQueued(stream_) > 0) {
            return;
        }
        if (looping_) {
            SDL_PutAudioStreamData(stream_, samples_.data(), static_cast<int>(samples_.size()));
            return;
        }
        samples_.clear();
    }

private:
    uint32_t voice_ = 0;
    std::vector<uint8_t> samples_;
    bool looping_ = false;
    SDL_AudioStream* stream_ = nullptr;
};

// The music, decoded here and fed to SDL like the sound effects are.
//
// This used to spawn `afplay`, which played the file and offered nothing else: no volume
// this program could set, no way to stop it that was not a signal, and macOS only. The
// Mini Golf recomp had already replaced that with a real decoder, and this is that same
// class over the same shared `MusicDecoder` — see ../../../../common/README.md. The old
// TODO here asked for exactly this and is answered.
// The game's music, on the same audio device as everything else.
//
// The tracks are AAC (`.m4a`), which SDL does not decode; `music_decoder.h` does that and hands
// back PCM, and this feeds it to an SDL audio stream a chunk at a time exactly as a sound effect
// is fed. It used to be a child `afplay` process, which played the file and offered nothing
// else: no volume this program could set, no way to stop it that was not a signal, and macOS
// only. Everything the game asks of the music — stop it, turn it down — needs the audio to be
// ours, so it is.
//
// A track is decoded as it plays. `service()` tops the stream up to about half a second ahead,
// which is far more than a frame's worth of slack and small enough that no track costs more
// than a few hundred kilobytes of buffer.
class MusicPlayer {
public:
    ~MusicPlayer() { close_stream(); }

    MusicPlayer() = default;
    MusicPlayer(const MusicPlayer&) = delete;
    MusicPlayer& operator=(const MusicPlayer&) = delete;

    void play(const std::string& path, bool repeat) {
        stop();
        if (!music_decoding_supported()) {
            if (!warned_) {
                warned_ = true;
                std::fprintf(stderr, "music: no decoder in this build (wanted %s)\n", path.c_str());
            }
            return;
        }
        SDL_AudioSpec spec{};
        if (!decoder_.open(path, spec)) {
            return;  // the decoder has said why
        }
        if (trace_audio()) {
            std::fprintf(stderr, "audio: music %s (%d Hz, %d ch)%s\n", path.c_str(), spec.freq,
                         spec.channels, repeat ? ", repeating" : "");
        }
        repeat_ = repeat;
        if (!open_stream(spec)) {
            decoder_.close();
            return;
        }
        service();  // start with the queue filled, so the track opens without a gap
        SDL_ResumeAudioStreamDevice(stream_);
    }

    // Silence it until another track is asked for. What Music: OFF does.
    void stop() {
        if (trace_audio() && decoder_.is_open()) {
            std::fprintf(stderr, "audio: music stopped\n");
        }
        if (stream_ != nullptr) {
            SDL_ClearAudioStream(stream_);
            SDL_PauseAudioStreamDevice(stream_);
        }
        decoder_.close();
    }

    // Called each frame: keep the queue ahead of the device, and loop or finish at the end.
    void service() {
        if (!decoder_.is_open() || stream_ == nullptr) {
            return;
        }
        while (SDL_GetAudioStreamQueued(stream_) < static_cast<int>(QUEUE_TARGET_BYTES)) {
            const int frames = decoder_.read(buffer_.data(), CHUNK_FRAMES);
            if (frames <= 0) {
                if (!repeat_) {
                    decoder_.close();
                    return;
                }
                if (trace_audio()) {
                    std::fprintf(stderr, "audio: music looped\n");
                }
                decoder_.restart();
                // A track that decodes to nothing at all would spin here for ever.
                if (decoder_.read(buffer_.data(), CHUNK_FRAMES) <= 0) {
                    decoder_.close();
                    return;
                }
                continue;
            }
            SDL_PutAudioStreamData(stream_, buffer_.data(), frames * bytes_per_frame_);
        }
    }

    // The device's volume, 0 to 1. Kept for the next track as well as applied to this one.
    void set_gain(float gain) {
        gain_ = gain;
        if (stream_ != nullptr) {
            (void)SDL_SetAudioStreamGain(stream_, gain_);
        }
    }

    // How much is queued ahead of the device, for the trace and for the tests.
    [[nodiscard]] int queued_bytes() const {
        return stream_ == nullptr ? 0 : SDL_GetAudioStreamQueued(stream_);
    }

private:
    static constexpr int CHUNK_FRAMES = 8192;
    static constexpr size_t QUEUE_TARGET_BYTES = 88'200;  // about half a second of 44.1 kHz stereo

    bool open_stream(const SDL_AudioSpec& spec) {
        // A new track may be a different shape from the last; reopening only when it is keeps
        // the usual case (six tracks, all 44.1 kHz stereo) down to one device open per run.
        if (stream_ != nullptr && (spec.format != spec_.format || spec.channels != spec_.channels ||
                                   spec.freq != spec_.freq)) {
            close_stream();
        }
        if (stream_ == nullptr) {
            stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr,
                                                nullptr);
            if (stream_ == nullptr) {
                std::fprintf(stderr, "music: no audio device: %s\n", SDL_GetError());
                return false;
            }
            (void)SDL_SetAudioStreamGain(stream_, gain_);
        }
        spec_ = spec;
        bytes_per_frame_ =
            static_cast<int>(SDL_AUDIO_BYTESIZE(static_cast<unsigned>(spec.format))) *
            static_cast<int>(spec.channels);
        buffer_.assign(static_cast<size_t>(CHUNK_FRAMES) * static_cast<size_t>(bytes_per_frame_),
                       0);
        return true;
    }

    void close_stream() {
        decoder_.close();
        if (stream_ != nullptr) {
            SDL_DestroyAudioStream(stream_);
            stream_ = nullptr;
        }
    }

    MusicDecoder decoder_;
    SDL_AudioStream* stream_ = nullptr;
    SDL_AudioSpec spec_{};
    std::vector<Uint8> buffer_;
    int bytes_per_frame_ = 4;
    bool repeat_ = false;
    bool warned_ = false;
    float gain_ = 1.0f;
};

// SDL is shut down when this goes.
//
// It exists so that the shutdown can be made to happen *last* by declaring it first: members are
// destroyed in reverse order of declaration, and the destructor body runs before any of them. Put
// `SDL_Quit` in that body — which is where it was — and every member holding something of SDL's
// is destroyed after the subsystem it belongs to has gone. A voice with an audio stream did
// exactly that and took the program down on its way out, and only once a sound had actually
// played, since a voice that never opened a stream has nothing to give back.
struct SdlSession {
    ~SdlSession() { SDL_Quit(); }
};

class Sdl3Platform final : public Platform {
public:
    Sdl3Platform(const char* title, unsigned frames_per_second)
        : title_(title), paced_rate_(frames_per_second == 0 ? 60 : frames_per_second) {
        // The rate this run was started at is the default until a saved one is read over it
        // (runtime/main.cpp).
        settings().frame_rate = frames_per_second;
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
            std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
            return;
        }
        if (!SDL_CreateWindowAndRenderer(title, static_cast<int>(SCREEN_WIDTH) * WINDOW_SCALE,
                                         static_cast<int>(SCREEN_HEIGHT) * WINDOW_SCALE,
                                         SDL_WINDOW_RESIZABLE, &window_, &renderer_)) {
            std::fprintf(stderr, "cannot create window: %s\n", SDL_GetError());
            return;
        }
        // The keys this platform starts with. A player's own bindings, if there are any, are
        // loaded over these once the save store is in place (runtime/main.cpp).
        //
        // The menus are a vertical list the wheel scrolls, so Up and Down *turn* the wheel: a
        // press is one row (DETENTS_PER_ROW — measured in the emulator, eight detents move the
        // highlight exactly one row and six move it nothing, and a positive turn goes down the
        // list), a held key keeps turning. Left and Right are the two buttons beside the wheel,
        // which is where a player's hand expects "previous" and "next". The four *sides* of the
        // wheel — a finger resting rather than turning, the gesture Lost walks by — are bound to
        // nothing: no screen of this game has been seen to read them, and an input does exactly
        // one thing. The second slots keep the keys the other titles use, so nothing anyone has
        // learned stops working; Settings > Input changes any of it.
        const InputCode defaults[ACTION_COUNT][BINDING_SLOTS] = {
            {SDLK_UP, SDLK_COMMA},            // SwipeLeft: up the list
            {SDLK_DOWN, SDLK_PERIOD},         // SwipeRight: down the list
            {NO_INPUT, NO_INPUT},             // TouchUp
            {NO_INPUT, NO_INPUT},             // TouchRight
            {NO_INPUT, NO_INPUT},             // TouchBottom
            {NO_INPUT, NO_INPUT},             // TouchLeft
            {SDLK_SPACE, NO_INPUT},           // Select: Return belongs to typing
            {SDLK_TAB, NO_INPUT},             // PlayPause
            {SDLK_ESCAPE, NO_INPUT},          // Menu
            {SDLK_LEFT, SDLK_LEFTBRACKET},    // Rewind
            {SDLK_RIGHT, SDLK_RIGHTBRACKET},  // FastForward
        };
        set_default_bindings(defaults);
        set_assignable_inputs(assignable_keys(), ASSIGNABLE_KEY_COUNT);
        // Typing is how a name gets entered on a machine with a keyboard; spelling a name out
        // on the wheel keys still works.
        SDL_StartTextInput(window_);
        // Resizing is unrestricted; shared presentation preserves the frame aspect ratio.

        // Rebinding writes straight into the table the key handling reads, so saving is all the
        // window asks of us; the frame rate it does not own at all, and only asks us to flip.
        SettingsHooks hooks;
        hooks.on_bindings_changed = [](void*) { save_input_bindings(); };
        hooks.on_frame_rate_chosen = [](void* context, unsigned rate) {
            static_cast<Sdl3Platform*>(context)->set_frame_rate(rate);
        };
        hooks.on_show_frame_rate_changed = [](void* context, bool show) {
            static_cast<Sdl3Platform*>(context)->set_show_frame_rate(show);
        };
        hooks.frame_rate = settings().frame_rate;
        hooks.show_frame_rate = settings().show_frame_rate;
        hooks.on_scaling_chosen = [](void* context, Scaling scaling) {
            static_cast<Sdl3Platform*>(context)->set_scaling(scaling);
        };
        hooks.on_pixel_perfect_changed = [](void* context, bool pixel_perfect) {
            static_cast<Sdl3Platform*>(context)->set_pixel_perfect(pixel_perfect);
        };
        hooks.on_render_scale_chosen = [](void* context, unsigned scale) {
            static_cast<Sdl3Platform*>(context)->set_render_scale(scale);
        };
        hooks.on_high_resolution_text_changed = [](void* context, bool resolve) {
            static_cast<Sdl3Platform*>(context)->set_high_resolution_text(resolve);
        };
        hooks.scaling = settings().scaling;
        hooks.pixel_perfect = settings().pixel_perfect;
        hooks.render_scale = settings().render_scale;
        hooks.high_resolution_text = settings().high_resolution_text;
        hooks.context = this;
        hooks.game_window = window_;
        settings_window_install(hooks);
        presenter_.attach(renderer_, SCREEN_WIDTH, SCREEN_HEIGHT);
        apply_presentation();
        // Show black until the game has drawn something.
        //
        // The frame pump does not present a frame the game drew nothing into (gfx::anything_drawn)
        // — its first frame is the firmware telling it that it is running, and the buffer is still
        // the magenta that marks an un-drawn region. Without this the window would instead show
        // whatever the renderer happened to start with for that one frame, which is not something
        // to leave to chance in the first thing a player sees.
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        SDL_RenderClear(renderer_);
        SDL_RenderPresent(renderer_);
        next_frame_ns_ = SDL_GetTicksNS();
        title_updated_ns_ = next_frame_ns_;
    }

    // What this body releases is what nothing else would: the window, the renderer and their
    // textures are raw handles with no owner. Everything with a destructor of its own — a voice
    // and its audio stream — is released after this runs and before SDL goes, which is what
    // `sdl_session_` is for.
    ~Sdl3Platform() override {
        presenter_.reset();
        if (renderer_ != nullptr) {
            SDL_DestroyRenderer(renderer_);
        }
        if (window_ != nullptr) {
            SDL_DestroyWindow(window_);
        }
    }

    void poll(FrameInput& input) override {
        input = FrameInput{};
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_KEY_DOWN &&
                event.key.windowID == SDL_GetWindowID(window_) &&
                presenter_.handle_shortcut(event)) {
                settings().scaling = presenter_.options().scaling;
                settings().pixel_perfect = presenter_.options().pixel_perfect;
                save_settings();
                continue;
            }
            switch (event.type) {
            case SDL_EVENT_QUIT:
                input.quit = true;
                break;
            case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
                // Dragged to another screen, which may refresh at another rate: what a whole
                // number of refreshes means has changed with it.
                apply_frame_pacing();
                break;
            case SDL_EVENT_KEY_DOWN:
                // Command-comma opens the settings window on macOS, where every application
                // keeps it; Control-comma everywhere else. The Mac menu item carries the same
                // shortcut, but whether a key equivalent reaches the menu depends on how the
                // window that has focus was made; handling it here works either way.
                if (event.key.key == SDLK_COMMA && (event.key.mod & SETTINGS_MODIFIER) != 0 &&
                    !event.key.repeat) {
                    settings_window_open();
                    break;
                }
                // Backspace and Return belong to whatever is being typed. Neither is offered in
                // the settings window, so neither can be a control.
                if (event.key.key == SDLK_BACKSPACE) {
                    ++input.typed.backspaces;
                    break;
                }
                if (event.key.key == SDLK_RETURN && !event.key.repeat) {
                    input.typed.confirm = true;
                    break;
                }
                // What the player has bound comes first, so any key can be rebound to anything;
                // the window's own shortcuts only get the keys nothing else wants.
                if (handle_key(event.key.key, event.key.repeat, input)) {
                    swallow_next_text_ = true;
                    break;
                }
                if (event.key.repeat) {
                    break;
                }
                if (event.key.key == SDLK_F || event.key.key == SDLK_F11) {
                    toggle_fullscreen();
                } else if (event.key.key == SDLK_L) {
                    toggle_frame_rate_lock();
                } else if (event.key.key == SDLK_MINUS || event.key.key == SDLK_KP_MINUS) {
                    step_window_scale(-1);
                } else if (event.key.key == SDLK_EQUALS || event.key.key == SDLK_PLUS ||
                           event.key.key == SDLK_KP_EQUALS || event.key.key == SDLK_KP_PLUS) {
                    step_window_scale(1);
                }
                break;
            case SDL_EVENT_TEXT_INPUT:
                // A key bound to a control is a control, not a letter: SDL sends the key press
                // before the text it produces, so the press that was just used swallows it.
                if (swallow_next_text_) {
                    swallow_next_text_ = false;
                } else {
                    input.typed.characters += event.text.text;
                }
                break;
            default:
                break;
            }
        }
        input.touch = touched_side();
        input.spin = spin_direction();
        input.buttons_down = buttons_held();
        service_audio();
    }

    [[nodiscard]] bool text_input_supported() const override { return true; }

    void present(const uint8_t* rgb, unsigned width, unsigned height) override {
        if (!presenter_.present(rgb, width, height))
            std::fprintf(stderr, "video: %s\n", SDL_GetError());
        update_frame_rate_display();
    }

    void wait_for_next_frame() override {
        // Where the display is doing the pacing on its own, `present` has already waited the
        // whole frame and a sleep on top would only make it late. See `apply_frame_pacing`.
        if (vsync_paces_) {
            return;
        }
        const Uint64 interval_ns = frame_interval_ns();
        if (interval_ns == 0) {
            return;
        }
        next_frame_ns_ += interval_ns;
        const Uint64 now = SDL_GetTicksNS();
        if (next_frame_ns_ > now) {
            SDL_DelayNS(next_frame_ns_ - now);
        } else if (now - next_frame_ns_ > interval_ns * 4) {
            next_frame_ns_ = now;  // fell far behind (window dragged, machine busy): don't catch up
        }
    }

    // Wait for the display before showing a frame, so that a frame is never half of one and
    // half of the next.
    //
    // Presenting whenever a timer expires means presenting in the middle of a refresh, and the
    // seam between the two frames is a tear. It is worst at a rate that is not the display's: at
    // 30 frames a second on a 120 Hz screen the two drift against each other, so the tear line
    // marches down the picture instead of sitting still.
    //
    // Two ways to wait, and the better one is not always available. A vsync *interval* — present
    // every Nth refresh — paces the frames exactly and needs no timer at all, but macOS's Metal
    // renderer takes only 1 and refuses anything above it. So the interval is asked for first and
    // plain vsync is the fallback: presenting still waits for a refresh, which is what stops the
    // tearing, and the timer below decides which refresh to land on. Running unlocked asks for
    // frames as fast as they come, which means no waiting and a torn picture by choice.
    void apply_frame_pacing() {
        vsync_paces_ = false;
        if (renderer_ == nullptr) {
            return;
        }
        const unsigned rate = settings().frame_rate;
        const float refresh = display_refresh_hz();
        int interval = 0;
        if (rate != 0) {
            interval = 1;
            if (refresh > 0.0f) {
                const float refreshes = refresh / static_cast<float>(rate);
                const int whole = static_cast<int>(std::lround(refreshes));
                if (whole >= 1 && std::fabs(refreshes - static_cast<float>(whole)) < 0.01f &&
                    SDL_SetRenderVSync(renderer_, whole)) {
                    vsync_paces_ = true;
                    interval = whole;
                }
            }
        }
        if (!vsync_paces_ &&
            !SDL_SetRenderVSync(renderer_, interval == 0 ? SDL_RENDERER_VSYNC_DISABLED : 1)) {
            std::fprintf(stderr,
                         "display: renderer %s refuses vsync (%s) — the picture will tear\n",
                         SDL_GetRendererName(renderer_), SDL_GetError());
            interval = 0;
        }
        report_frame_pacing(rate, refresh, interval);
        next_frame_ns_ = SDL_GetTicksNS();
    }

    // Said once per change rather than per frame, because "why does it tear" is a question this
    // line answers, and guessing at it is what cost the afternoon it was written in.
    void report_frame_pacing(unsigned rate, float refresh, int interval) {
        if (interval == reported_interval_ && rate == reported_rate_) {
            return;
        }
        reported_interval_ = interval;
        reported_rate_ = rate;
        if (rate == 0) {
            std::printf("display: unlocked — frames as fast as they come, and it will tear\n");
        } else if (vsync_paces_) {
            std::printf("display: %.4g Hz, %u fps — every %d refresh%s, paced by the display\n",
                        static_cast<double>(refresh), rate, interval, interval == 1 ? "" : "es");
        } else if (interval != 0) {
            std::printf("display: %.4g Hz, %u fps — waits for a refresh, paced by a timer\n",
                        static_cast<double>(refresh), rate);
        }
    }

    // What the display the window is on refreshes at, or 0 if it will not say.
    [[nodiscard]] float display_refresh_hz() const {
        const SDL_DisplayID display = SDL_GetDisplayForWindow(window_);
        const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(display);
        return mode == nullptr ? 0.0f : mode->refresh_rate;
    }

    void play_sound(uint32_t voice, const SoundClip& clip, bool looping) override {
        // The voice already playing this sound takes it again — a retrigger restarts it — and
        // otherwise any idle one will do. All of them busy means the device's polyphony is used
        // up, as it was on the iPod.
        Voice* chosen = nullptr;
        for (Voice& candidate : voices_) {
            if (!candidate.idle() && candidate.sounding() == voice) {
                chosen = &candidate;
                break;
            }
            if (chosen == nullptr && candidate.idle()) {
                chosen = &candidate;
            }
        }
        if (chosen != nullptr) {
            (void)chosen->start(voice, clip, looping);
        }
    }

    void stop_sound(uint32_t voice) override {
        for (Voice& candidate : voices_) {
            if (!candidate.idle() && candidate.sounding() == voice) {
                candidate.stop();
            }
        }
    }

    void play_music(const std::string& path, bool repeat) override { music_.play(path, repeat); }

    // SDL's dialog answers through a callback from the event loop, so pump events until it
    // does. The answer is shared, not a local: SDL calls back when the dialog closes, which may
    // be after the player has quit and this function has returned. Both sides hold a reference,
    // so whichever finishes last releases it.
    bool choose_file(const std::string& prompt, const std::string& extension,
                     std::string& chosen_path) override {
        const auto answer = std::make_shared<Answer>();
        // The dialog belongs to the game's window, and the system puts it where that window is:
        // behind a terminal, on a machine where the program was started from one, unless the
        // window is brought forward first. The console is told as well, since a dialog nobody
        // can see looks exactly like a program that has stopped.
        SDL_RaiseWindow(window_);
        std::fprintf(stderr, "%s: opening the file browser\n", prompt.c_str());
        const SDL_DialogFileFilter filters[] = {{prompt.c_str(), extension.c_str()}};
        SDL_ShowOpenFileDialog(on_dialog_answer, new std::shared_ptr<Answer>(answer), window_,
                               filters, 1, nullptr, false);
        return wait_for_dialog(*answer, chosen_path);
    }

    bool choose_directory(const std::string& prompt, std::string& chosen_path) override {
        // SDL's folder dialog takes no filter, so the prompt has nowhere to go — the window
        // title is the system's. It is still worth printing: a dialog that appears with no
        // explanation is the same fault as no dialog at all.
        std::printf("%s\n", prompt.c_str());
        const auto answer = std::make_shared<Answer>();
        SDL_ShowOpenFolderDialog(on_dialog_answer, new std::shared_ptr<Answer>(answer), window_,
                                 nullptr, false);
        return wait_for_dialog(*answer, chosen_path);
    }

private:
    // Declared before every other member, so that it is destroyed after every other member: SDL
    // outlives everything of SDL's that this class holds. See `SdlSession`.
    SdlSession sdl_session_;

    // What a file or folder dialog reports back. `done` is written from SDL's callback, which
    // may run on another thread, and read by `wait_for_dialog`.
    struct Answer {
        std::atomic<bool> done{false};
        std::string path;
        std::string error;  // why there was no dialog, when there was none
    };

    // SDL hands the callback the `userdata` it was given; that is a reference this class made
    // over to it, and dropping it here is what keeps the answer alive until the callback comes
    // even if the caller has given up waiting.
    static void on_dialog_answer(void* userdata, const char* const* paths, int /*filter*/) {
        const std::unique_ptr<std::shared_ptr<Answer>> held(
            static_cast<std::shared_ptr<Answer>*>(userdata));
        Answer& result = **held;
        if (paths == nullptr) {
            // The dialog could not be shown at all. SDL's error is per thread, and this may
            // not be the thread `wait_for_dialog` waits on, so it is taken here.
            result.error = SDL_GetError();
        } else if (paths[0] != nullptr) {
            result.path = paths[0];
        }
        result.done.store(true, std::memory_order_release);
    }

    // Pump events until the dialog answers. Closing the window while it is open gives up on it —
    // the answer object outlives this call, so the callback still has somewhere to land.
    bool wait_for_dialog(Answer& answer, std::string& chosen_path) {
        SDL_Event event;
        while (!answer.done.load(std::memory_order_acquire)) {
            if (SDL_WaitEventTimeout(&event, 50) && event.type == SDL_EVENT_QUIT) {
                return false;
            }
        }
        if (!answer.error.empty()) {
            std::fprintf(stderr, "the file browser could not be opened: %s\n",
                         answer.error.c_str());
        }
        chosen_path = answer.path;
        return !chosen_path.empty();
    }

    // The keys a settings window may offer. Naming them here rather than asking the player to
    // press one keeps the window out of this platform's event handling entirely: it only has to
    // show a list. SDL names each key for us, so the labels match what the keyboard says.
    static const InputChoice* assignable_keys() {
        static InputChoice keys[ASSIGNABLE_KEY_COUNT];
        static bool built = false;
        if (!built) {
            built = true;
            static const SDL_Keycode codes[ASSIGNABLE_KEY_COUNT] = {
                SDLK_ESCAPE,      SDLK_LEFT,
                SDLK_RIGHT,       SDLK_UP,
                SDLK_DOWN,        SDLK_SPACE,
                SDLK_RETURN,      SDLK_TAB,
                SDLK_COMMA,       SDLK_PERIOD,
                SDLK_SLASH,       SDLK_SEMICOLON,
                SDLK_LEFTBRACKET, SDLK_RIGHTBRACKET,
                SDLK_MINUS,       SDLK_EQUALS,
                SDLK_A,           SDLK_B,
                SDLK_C,           SDLK_D,
                SDLK_E,           SDLK_G,
                SDLK_H,           SDLK_I,
                SDLK_J,           SDLK_K,
                SDLK_M,           SDLK_N,
                SDLK_O,           SDLK_R,
                SDLK_S,           SDLK_T,
                SDLK_U,           SDLK_V,
                SDLK_W,           SDLK_X,
                SDLK_Y,           SDLK_Z,
                SDLK_1,           SDLK_2,
                SDLK_3,           SDLK_4,
                SDLK_5,           SDLK_6,
                SDLK_7,           SDLK_8,
                SDLK_9,           SDLK_0,
            };
            static_assert(sizeof(codes) / sizeof(codes[0]) == ASSIGNABLE_KEY_COUNT,
                          "ASSIGNABLE_KEY_COUNT must match the list");
            for (unsigned i = 0; i < ASSIGNABLE_KEY_COUNT; ++i) {
                keys[i].code = static_cast<InputCode>(codes[i]);
                keys[i].label = SDL_GetKeyName(codes[i]);  // SDL owns the string; it is static
            }
        }
        return keys;
    }

    // Which of the device's five buttons an action presses. The wheel is handled before this,
    // because turning it repeats while a button press does not.
    // Which side of the wheel an action rests a finger on, if it is one of those.
    static WheelTouch side_for(Action action) {
        switch (action) {
        case Action::TouchUp:
            return WheelTouch::Top;
        case Action::TouchRight:
            return WheelTouch::Right;
        case Action::TouchBottom:
            return WheelTouch::Bottom;
        case Action::TouchLeft:
            return WheelTouch::Left;
        default:
            return WheelTouch::None;
        }
    }

    // What the player's finger is on right now. Read from the keyboard's *state* rather than
    // from key events, because this is the one control that is held rather than pressed: the
    // game walks for as long as it lasts. Two sides at once is not a thing a finger does, so the
    // first in this order wins.
    static WheelTouch touched_side() {
        const bool* keys = SDL_GetKeyboardState(nullptr);
        if (keys == nullptr) {
            return WheelTouch::None;
        }
        for (const Action action :
             {Action::TouchUp, Action::TouchRight, Action::TouchBottom, Action::TouchLeft}) {
            for (unsigned slot = 0; slot < BINDING_SLOTS; ++slot) {
                const InputCode code = input_bindings().code(action, slot);
                if (code == NO_INPUT) {
                    continue;
                }
                const SDL_Scancode scancode =
                    SDL_GetScancodeFromKey(static_cast<SDL_Keycode>(code), nullptr);
                if (scancode != SDL_SCANCODE_UNKNOWN && keys[scancode]) {
                    return side_for(action);
                }
            }
        }
        return WheelTouch::None;
    }

    // Which way the wheel is being turned right now, from the keyboard's *state*.
    //
    // A held scroll key is a finger going round the wheel and not letting go, which is a
    // different thing from a press: the game watches the angle *change while contact lasts*, and
    // an eight-detent flick that ends in the queue running dry is a finger lifted. Anything that
    // asks for a sustained turn never sees one.
    static int spin_direction() {
        const bool* keys = SDL_GetKeyboardState(nullptr);
        if (keys == nullptr) {
            return 0;
        }
        const auto held = [keys](Action action) {
            for (unsigned slot = 0; slot < BINDING_SLOTS; ++slot) {
                const InputCode code = input_bindings().code(action, slot);
                if (code == NO_INPUT) {
                    continue;
                }
                const SDL_Scancode scancode =
                    SDL_GetScancodeFromKey(static_cast<SDL_Keycode>(code), nullptr);
                if (scancode != SDL_SCANCODE_UNKNOWN && keys[scancode]) {
                    return true;
                }
            }
            return false;
        };
        // Both at once is no rotation at all, which is what two fingers pushing opposite ways
        // would do.
        return static_cast<int>(held(Action::SwipeRight)) -
               static_cast<int>(held(Action::SwipeLeft));
    }

    // Which buttons are down right now, from the keyboard's state — the level the game times a
    // hold by (platform.h, `FrameInput::buttons_down`).
    static uint32_t buttons_held() {
        const bool* keys = SDL_GetKeyboardState(nullptr);
        if (keys == nullptr) {
            return 0;
        }
        uint32_t down = 0;
        for (const Action action : {Action::Select, Action::PlayPause, Action::Menu, Action::Rewind,
                                    Action::FastForward}) {
            for (unsigned slot = 0; slot < BINDING_SLOTS; ++slot) {
                const InputCode code = input_bindings().code(action, slot);
                if (code == NO_INPUT) {
                    continue;
                }
                const SDL_Scancode scancode =
                    SDL_GetScancodeFromKey(static_cast<SDL_Keycode>(code), nullptr);
                if (scancode != SDL_SCANCODE_UNKNOWN && keys[scancode]) {
                    down |= button_for(action);
                }
            }
        }
        return down;
    }

    static uint32_t button_for(Action action) {
        switch (action) {
        case Action::Select:
            return static_cast<uint32_t>(Button::Select);
        case Action::PlayPause:
            return static_cast<uint32_t>(Button::Play);
        case Action::Menu:
            return static_cast<uint32_t>(Button::Menu);
        case Action::Rewind:
            return static_cast<uint32_t>(Button::Previous);
        case Action::FastForward:
            return static_cast<uint32_t>(Button::Next);
        default:
            return 0;
        }
    }

    // A key press, through whatever the player has bound it to. The two keys that are not the
    // device's — a screenshot and quitting — are fixed, because they are the program's rather
    // than the game's.
    // True when the key did something, so the caller knows not to offer it to anything else.
    static bool handle_key(SDL_Keycode key, bool repeat, FrameInput& input) {
        Action action = Action::Select;
        const bool bound = input_bindings().action_for(static_cast<InputCode>(key), action);
        if (trace_input() && !repeat) {
            std::fprintf(stderr, "key 0x%x (%s) %s\n", static_cast<unsigned>(key),
                         input_label(static_cast<InputCode>(key)),
                         bound ? action_label(action) : "not bound");
        }
        if (bound) {
            // A *press* of a scroll key is a flick worth one row. Holding it is a different
            // gesture — see `spin_direction` — so the repeats that follow are left alone.
            if (action == Action::SwipeLeft) {
                if (!repeat) {
                    input.wheel_detents -= DETENTS_PER_ROW;
                }
                return true;
            }
            if (action == Action::SwipeRight) {
                if (!repeat) {
                    input.wheel_detents += DETENTS_PER_ROW;
                }
                return true;
            }
            if (side_for(action) != WheelTouch::None) {
                return true;  // a level, read from the keyboard in `poll` rather than from events
            }
            if (!repeat) {  // a held button is one press
                input.buttons |= button_for(action);
            }
            return true;
        }
        if (repeat) {
            return false;
        }
        switch (key) {
        case SDLK_P:
            input.screenshot = true;
            return true;
        case SDLK_Q:
            input.quit = true;
            return true;
        default:
            return false;
        }
    }

    // Grow or shrink the window by one whole multiple of the screen, between 1x and MAX_PRESCALE.
    // This explicit shortcut sizes the window; ordinary dragging remains unrestricted.
    void step_window_scale(int by) {
        int width = 0, height = 0;
        SDL_GetWindowSize(window_, &width, &height);
        const int now = std::max(1, (width + static_cast<int>(SCREEN_WIDTH) / 2) /
                                        static_cast<int>(SCREEN_WIDTH));
        const int wanted = std::clamp(now + by, 1, MAX_PRESCALE);
        SDL_SetWindowSize(window_, static_cast<int>(SCREEN_WIDTH) * wanted,
                          static_cast<int>(SCREEN_HEIGHT) * wanted);
    }

    void toggle_fullscreen() {
        fullscreen_ = !fullscreen_;
        if (!SDL_SetWindowFullscreen(window_, fullscreen_)) {
            std::fprintf(stderr, "cannot change full screen: %s\n", SDL_GetError());
            fullscreen_ = !fullscreen_;
        }
        apply_presentation();
    }

    // How long a frame is meant to take, or 0 when the rate is unlocked and it may take as long
    // as it likes.
    [[nodiscard]] Uint64 frame_interval_ns() const {
        const unsigned rate = settings().frame_rate;
        return rate == 0 ? 0 : 1'000'000'000ull / rate;
    }

    // Frames are paced to `rate` a second; 0 runs as fast as the machine allows, which
    // fast-forwards the game. Settings ▸ General and the L key both come through here.
    void set_frame_rate(unsigned rate) {
        settings().frame_rate = rate;
        if (rate != 0) {
            paced_rate_ = rate;  // what L goes back to
        }
        apply_frame_pacing();  // also paces from now, not from where the unlocked run left off
        settings_window_set_frame_rate(rate);
        save_settings();
    }

    // L, the shortcut for the two rates a player switches between while testing.
    void toggle_frame_rate_lock() { set_frame_rate(settings().frame_rate == 0 ? paced_rate_ : 0); }

    // How the picture reaches the window: the filter, and whether it may be scaled by a fraction
    // at all. Settings ▸ Graphics comes through here.
    void set_scaling(Scaling scaling) {
        settings().scaling = scaling;
        apply_presentation();
        save_settings();
    }

    void set_pixel_perfect(bool pixel_perfect) {
        settings().pixel_perfect = pixel_perfect;
        apply_presentation();
        save_settings();
    }

    // The three that are the renderer's business rather than the window's. Each one only records
    // the choice and saves it: the frame pump reads `settings()` at the top of every frame and
    // hands the renderer whatever it now says (src/runtime/main.cpp), so there is nothing to
    // apply here and no second copy of the answer to keep in step.
    void set_render_scale(unsigned scale) {
        settings().render_scale = std::clamp(scale, MIN_RENDER_SCALE, MAX_RENDER_SCALE);
        save_settings();
    }

    void set_high_resolution_text(bool resolve) {
        settings().high_resolution_text = resolve;
        save_settings();
    }

    // Everything at once, for the settings read from the store at start-up.
    void apply_settings() override {
        paced_rate_ = settings().frame_rate == 0 ? paced_rate_ : settings().frame_rate;
        apply_frame_pacing();
        apply_presentation();
        set_title_now();
        settings_window_set_frame_rate(settings().frame_rate);
    }

    void apply_presentation() {
        presenter_.set_options({settings().scaling, settings().pixel_perfect});
    }

    void set_show_frame_rate(bool show) {
        settings().show_frame_rate = show;
        set_title_now();
        save_settings();
    }

    // The plain title, and a fresh start for the frame counter behind the rate.
    void set_title_now() {
        if (!settings().show_frame_rate) {
            SDL_SetWindowTitle(window_, title_.c_str());
        }
        title_updated_ns_ = SDL_GetTicksNS();
        frames_since_title_ = 0;
    }

    // The window title carries the live frame rate, refreshed twice a second, for as long as the
    // player wants to see it (Settings ▸ General).
    void update_frame_rate_display() {
        if (!settings().show_frame_rate) {
            return;
        }
        ++frames_since_title_;
        const Uint64 now = SDL_GetTicksNS();
        if (now - title_updated_ns_ < TITLE_REFRESH_NS) {
            return;
        }
        const double seconds = static_cast<double>(now - title_updated_ns_) / 1e9;
        const double fps = static_cast<double>(frames_since_title_) / seconds;
        char title[96];
        std::snprintf(title, sizeof title, "%s — %.1f fps%s", title_.c_str(), fps,
                      settings().frame_rate == 0 ? " (unlocked)" : "");
        SDL_SetWindowTitle(window_, title);
        title_updated_ns_ = now;
        frames_since_title_ = 0;
    }

    void service_audio() {
        for (Voice& voice : voices_) {
            voice.service();
        }
        music_.service();
    }

    std::string title_;
    Uint64 title_updated_ns_ = 0;
    unsigned frames_since_title_ = 0;
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    ipod::video::Presenter presenter_;
    bool fullscreen_ = false;
    unsigned paced_rate_;  // the rate L goes back to from unlocked

    Uint64 next_frame_ns_ = 0;
    // Whether the vsync interval is pacing the frames by itself. False when the renderer would
    // not take an interval above 1 and the timer in `wait_for_next_frame` is doing it instead.
    bool vsync_paces_ = false;
    int reported_interval_ = -1;  // what was last reported, so it is said only on a change
    unsigned reported_rate_ = 0;
    bool swallow_next_text_ = false;  // the press that produced this text was a control
    Voice voices_[VOICE_LIMIT];       // opened as they are first needed, then kept
    MusicPlayer music_;
};

}  // namespace

std::unique_ptr<Platform> create_platform(const char* window_title, unsigned frames_per_second) {
    return std::make_unique<Sdl3Platform>(window_title, frames_per_second);
}

}  // namespace bowling::platform
