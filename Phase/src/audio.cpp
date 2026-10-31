#define NOMINMAX
#include "audio.h"

#include "runtime/memory.h"
#include "runtime/runtime.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <memory>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <thread>
#include <vector>
#include <windows.h>
#include <wrl/client.h>
#include <xaudio2.h>

namespace phase::audio {
namespace {
using Microsoft::WRL::ComPtr;
ComPtr<IXAudio2> engine;
IXAudio2MasteringVoice* master = nullptr;
bool enabled = false, com_started = false, mf_started = false;
void check(HRESULT hr, const char* action) {
    if (FAILED(hr))
        fatal("audio %s failed HRESULT=%08lx", action, unsigned long(hr));
}
struct Clip {
    WAVEFORMATEX format{};
    std::vector<uint8_t> bytes;
    uint32_t samples() const { return uint32_t(bytes.size() / format.nBlockAlign); }
};
std::shared_ptr<Clip> decode(const std::filesystem::path& path) {
    constexpr DWORD stream = MF_SOURCE_READER_FIRST_AUDIO_STREAM;
    ComPtr<IMFSourceReader> reader;
    check(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader), "open file");
    check(reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE), "deselect streams");
    check(reader->SetStreamSelection(stream, TRUE), "select audio");
    ComPtr<IMFMediaType> wanted;
    check(MFCreateMediaType(&wanted), "create type");
    check(wanted->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio), "audio type");
    check(wanted->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM), "PCM type");
    check(wanted->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16), "PCM bits");
    check(reader->SetCurrentMediaType(stream, nullptr, wanted.Get()), "decode PCM");
    ComPtr<IMFMediaType> actual;
    check(reader->GetCurrentMediaType(stream, &actual), "actual type");
    UINT32 channels = 0, rate = 0;
    check(actual->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels), "channels");
    check(actual->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate), "sample rate");
    if (!channels || channels > 2 || !rate || rate > 192000)
        fatal("unsupported audio shape");
    auto clip = std::make_shared<Clip>();
    clip->format = {
        WAVE_FORMAT_PCM, WORD(channels), rate, rate * channels * 2, WORD(channels * 2), 16, 0};
    for (unsigned iterations = 0;; ++iterations) {
        if (iterations > 1000000)
            fatal("audio decode iteration limit");
        ComPtr<IMFSample> sample;
        DWORD flags = 0;
        check(reader->ReadSample(stream, 0, nullptr, &flags, nullptr, &sample), "read PCM");
        if (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED))
            fatal("audio stream format changed");
        if (sample) {
            ComPtr<IMFMediaBuffer> buffer;
            check(sample->ConvertToContiguousBuffer(&buffer), "PCM buffer");
            BYTE* bytes = nullptr;
            DWORD length = 0;
            check(buffer->Lock(&bytes, nullptr, &length), "lock PCM");
            if (clip->bytes.size() + length > 384u * 1024 * 1024) {
                buffer->Unlock();
                fatal("decoded track exceeds 384 MiB");
            }
            clip->bytes.insert(clip->bytes.end(), bytes, bytes + length);
            check(buffer->Unlock(), "unlock PCM");
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
            break;
    }
    if (clip->bytes.empty() || clip->bytes.size() % clip->format.nBlockAlign)
        fatal("empty or unaligned decoded audio");
    std::fprintf(stderr, "AUDIO decoded %s rate=%u channels=%u samples=%u\n",
                 path.u8string().c_str(), rate, channels, clip->samples());
    return clip;
}
struct Playback {
    IXAudio2SourceVoice* voice = nullptr;
    std::shared_ptr<Clip> clip;
    uint32_t start = 0;
    bool paused = false, loop = false;
    ~Playback() { stop(); }
    void stop() {
        if (voice) {
            voice->DestroyVoice();
            voice = nullptr;
        }
        paused = false;
    }
    bool playing() const {
        XAUDIO2_VOICE_STATE state{};
        if (voice)
            voice->GetState(&state);
        return state.BuffersQueued && !paused;
    }
    uint32_t position() const {
        if (!voice || !clip)
            return start;
        XAUDIO2_VOICE_STATE state{};
        voice->GetState(&state);
        if (!state.BuffersQueued && !loop)
            return clip->samples();
        uint64_t samples = start + state.SamplesPlayed;
        return loop ? uint32_t(samples % clip->samples())
                    : uint32_t(std::min<uint64_t>(samples, clip->samples()));
    }
    void play(std::shared_ptr<Clip> data, uint32_t offset, bool repeat, float volume = 1,
              float rate = 1, float pan = 0) {
        stop();
        clip = std::move(data);
        start = std::min(offset, clip->samples());
        loop = repeat;
        if (start == clip->samples())
            return;
        check(engine->CreateSourceVoice(&voice, &clip->format, 0, 4), "create voice");
        check(voice->SetVolume(std::clamp(volume, 0.0f, 1.0f)), "volume");
        check(voice->SetFrequencyRatio(std::clamp(rate, 0.25f, 4.0f)), "rate");
        pan = std::clamp(pan, -1.0f, 1.0f);
        // The mastering voice is explicitly stereo. Preserve stereo channel separation.
        float matrix[4] = {std::min(1.0f, 1 - pan), 0, 0, std::min(1.0f, 1 + pan)};
        if (clip->format.nChannels == 1)
            matrix[1] = matrix[3];
        check(voice->SetOutputMatrix(master, clip->format.nChannels, 2, matrix), "pan");
        XAUDIO2_BUFFER buffer{};
        buffer.Flags = XAUDIO2_END_OF_STREAM;
        buffer.AudioBytes = UINT32(clip->bytes.size());
        buffer.pAudioData = clip->bytes.data();
        buffer.PlayBegin = start;
        if (loop) {
            buffer.LoopBegin = 0;
            buffer.LoopLength = clip->samples();
            buffer.LoopCount = XAUDIO2_LOOP_INFINITE;
        }
        check(voice->SubmitSourceBuffer(&buffer), "submit PCM");
        check(voice->Start(), "start voice");
    }
    void pause(bool value) {
        if (voice)
            check(value ? voice->Stop() : voice->Start(), "pause/resume");
        paused = value;
    }
};
Playback music;
struct Sound {
    std::array<uint32_t, 19> fields{};
    Playback playback;
};
std::vector<std::unique_ptr<Sound>> sounds;
struct QueueItem {
    std::filesystem::path path;
    uint32_t duration;
};
std::vector<QueueItem> queue;
uint32_t repeat_mode = 0, volume = 192, current = 0;
bool silent_playing = false, silent_paused = false;
std::string guest_string(uint32_t p) {
    std::string s;
    for (unsigned i = 0; i < 4096; ++i) {
        auto c = char(ld8(p + i));
        if (!c)
            return s;
        s += c;
    }
    fatal("unterminated music path");
}
void stop_music() {
    if (enabled && music.clip)
        std::fprintf(
            stderr, "AUDIO stopped at %u ms\n",
            uint32_t(uint64_t(music.position()) * 1000 / music.clip->format.nSamplesPerSec));
    music.stop();
    music.clip.reset();
    music.start = 0;
    silent_playing = silent_paused = false;
}
}  // namespace
void initialize(bool value) {
    enabled = value;
    if (!enabled)
        return;
    auto hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (hr != RPC_E_CHANGED_MODE) {
        check(hr, "COM initialization");
        com_started = true;
    }
    check(MFStartup(MF_VERSION, MFSTARTUP_LITE), "Media Foundation startup");
    mf_started = true;
    check(XAudio2Create(&engine), "XAudio2 startup");
    check(engine->CreateMasteringVoice(&master, 2, 48000), "output device");
}
void close() {
    sounds.clear();
    stop_music();
    queue.clear();
    if (master) {
        master->DestroyVoice();
        master = nullptr;
    }
    engine.Reset();
    if (mf_started) {
        MFShutdown();
        mf_started = false;
    }
    if (com_started) {
        CoUninitialize();
        com_started = false;
    }
    enabled = false;
    repeat_mode = 0;
    volume = 192;
    current = 0;
}
bool service(Cpu& cpu, const char* framework, unsigned ordinal) {
    const auto index = cpu.r[0], value = cpu.r[1];
    if (!std::strcmp(framework, "SoundEffect")) {
        if (ordinal == 33) {
            cpu.r[0] = 0x10000;
            return true;
        }
        if (ordinal == 0) {
            if (sounds.size() >= 256)
                fatal("too many sound voices");
            cpu.r[0] = uint32_t(sounds.size());
            sounds.push_back(std::make_unique<Sound>());
            return true;
        }
        if (index >= sounds.size())
            fatal("invalid sound voice");
        auto& s = *sounds[index];
        if (ordinal >= 7 && ordinal <= 18) {
            s.fields[ordinal] = value;
            cpu.r[0] = 0;
            return true;
        }
        if (ordinal == 35) {
            cpu.r[0] = enabled && s.playback.playing();
            return true;
        }
        if (ordinal == 5) {
            s.playback.stop();
            cpu.r[0] = 0;
            return true;
        }
        if (ordinal == 2) {
            if (enabled) {
                const auto& f = s.fields;
                if (!f[10] || f[10] > 192000 || !f[11] || f[11] > 2 ||
                    (f[12] != 8 && f[12] != 16) || f[8] > 8 * 1024 * 1024)
                    fatal("invalid sound format");
                auto clip = std::make_shared<Clip>();
                auto align = WORD(f[11] * f[12] / 8);
                clip->format = {WAVE_FORMAT_PCM, WORD(f[11]), f[10], f[10] * align,
                                align,           WORD(f[12]), 0};
                auto* data = guest_pointer(f[7], f[8]);
                clip->bytes.assign(data, data + f[8]);
                if (!clip->bytes.empty())
                    s.playback.play(clip, 0, f[16] == 0, f[13] / 65536.0f,
                                    int32_t(f[15]) / 65536.0f, int32_t(f[14]) / 65536.0f);
            }
            cpu.r[0] = 0;
            return true;
        }
        return false;
    }
    if (std::strcmp(framework, "Audio"))
        return false;
    switch (ordinal) {
    case 14:
        if (queue.size() >= 1024)
            fatal("audio queue full");
        queue.push_back({std::filesystem::u8path(guest_string(index)), cpu.r[3]});
        cpu.r[0] = 0;
        return true;
    case 16:
        stop_music();
        queue.clear();
        cpu.r[0] = 0;
        return true;
    case 19:
        stop_music();
        cpu.r[0] = 0;
        return true;
    case 29:
        cpu.r[0] = uint32_t(queue.size());
        return true;
    case 18:
        if (index >= queue.size())
            fatal("invalid audio queue index");
        current = index;
        silent_playing = true;
        silent_paused = false;
        if (enabled)
            music.play(decode(queue[index].path), 0, repeat_mode != 0, volume / 255.0f);
        cpu.r[0] = 0;
        return true;
    case 20:
        if (enabled)
            music.pause(!music.paused);
        else
            silent_paused = !silent_paused;
        std::fprintf(stderr, "AUDIO %s\n",
                     (enabled ? music.paused : silent_paused) ? "paused" : "resumed");
        cpu.r[0] = 0;
        return true;
    case 41:
        cpu.r[0] = enabled ? music.playing() : silent_playing && !silent_paused;
        return true;
    case 42:
        cpu.r[0] = enabled ? music.paused : silent_paused;
        return true;
    case 33:
        cpu.r[0] = (enabled ? music.paused : silent_paused)       ? 2
                   : (enabled ? music.playing() : silent_playing) ? 1
                                                                  : 0;
        return true;
    case 28:
        cpu.r[0] = current;
        return true;
    case 31:
        cpu.r[0] =
            enabled && music.clip
                ? uint32_t(uint64_t(music.position()) * 1000 / music.clip->format.nSamplesPerSec)
                : 0;
        return true;
    case 40:
        cpu.r[0] = repeat_mode;
        return true;
    case 1:
        if (index > 2)
            fatal("invalid audio repeat mode");
        if (enabled && music.voice && (repeat_mode != index)) {
            auto offset = music.position();
            bool paused = music.paused;
            music.play(music.clip, offset, index != 0, volume / 255.0f);
            music.pause(paused);
        }
        repeat_mode = index;
        cpu.r[0] = 0;
        return true;
    case 47:
        cpu.r[0] = volume;
        return true;
    case 48:
        cpu.r[0] = 255;
        return true;
    case 49:
        volume = std::min(index, 255u);
        if (music.voice)
            check(music.voice->SetVolume(volume / 255.0f), "music volume");
        cpu.r[0] = 0;
        return true;
    default:
        return false;
    }
}
void test(const std::filesystem::path& path) {
    initialize(true);
    auto clip = decode(path);
    music.play(clip, 0, false, 0.15f);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    auto first = music.position();
    if (!first || !music.playing())
        fatal("playback clock did not advance");
    music.pause(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    auto paused = music.position();
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    if (music.position() != paused || music.playing())
        fatal("paused clock advanced");
    music.pause(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    if (music.position() <= paused)
        fatal("resume clock did not advance");
    music.stop();
    if (music.playing())
        fatal("stop left audio playing");
    auto short_clip = std::make_shared<Clip>(*clip);
    short_clip->bytes.resize(size_t(clip->format.nSamplesPerSec / 20) * clip->format.nBlockAlign);
    music.play(short_clip, 0, true, 0.15f);
    std::this_thread::sleep_for(std::chrono::milliseconds(140));
    if (!music.playing() || music.position() >= short_clip->samples())
        fatal("repeat failed");
    music.play(short_clip, 0, false, 0.15f);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    if (music.playing() || music.position() != short_clip->samples())
        fatal("end of stream failed");
    std::printf(
        "PASS audio: decoded %u frames; output clock=%u paused=%u resumed; stop/repeat/end\n",
        clip->samples(), first, paused);
    close();
}
}  // namespace phase::audio
