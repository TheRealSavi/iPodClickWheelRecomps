#include "nis.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wunused-const-variable"
#endif
#include "../../../../third_party/nis/NIS_Config.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#include "nis_shader.inc"

#include <algorithm>
#include <d3d11.h>
#include <vector>
#include <wrl/client.h>

namespace ipod::video {
using Microsoft::WRL::ComPtr;
namespace {
bool checked(HRESULT hr, const char* operation) {
    if (FAILED(hr))
        return SDL_SetError("%s (HRESULT 0x%08x)", operation, static_cast<unsigned>(hr));
    return true;
}
ID3D11Device* device_for(SDL_Renderer* renderer) {
    return renderer ? static_cast<ID3D11Device*>(
                          SDL_GetPointerProperty(SDL_GetRendererProperties(renderer),
                                                 SDL_PROP_RENDERER_D3D11_DEVICE_POINTER, nullptr))
                    : nullptr;
}
ID3D11Texture2D* texture_for(SDL_Texture* texture) {
    return static_cast<ID3D11Texture2D*>(SDL_GetPointerProperty(
        SDL_GetTextureProperties(texture), SDL_PROP_TEXTURE_D3D11_TEXTURE_POINTER, nullptr));
}
}  // namespace

struct NisScaler::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> immediate;
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11ShaderResourceView> scaler, usm, input_view;
    ComPtr<ID3D11Texture2D> input;
    struct Stage {
        int width, height;
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> view;
        ComPtr<ID3D11UnorderedAccessView> output;
    };
    std::vector<Stage> stages;
    SDL_Texture* display = nullptr;
    ~Impl() {
        if (display)
            SDL_DestroyTexture(display);
    }

    bool initialize(ID3D11Device* supplied) {
        device = supplied;
        if (!device || device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0)
            return SDL_SetError("Direct3D feature level 11 required");
        device->GetImmediateContext(&immediate);
        // Offline bytecode avoids a shader-compilation hitch during gameplay.
        if (!checked(device->CreateComputeShader(nis_shader, sizeof(nis_shader), nullptr, &shader),
                     "create NIS shader"))
            return false;
        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = sizeof(NISConfig);
        buffer.Usage = D3D11_USAGE_DEFAULT;
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (!checked(device->CreateBuffer(&buffer, nullptr, &constants),
                     "NIS configuration buffer"))
            return false;
        D3D11_SAMPLER_DESC sampling{};
        sampling.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampling.AddressU = sampling.AddressV = sampling.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampling.MaxLOD = D3D11_FLOAT32_MAX;
        if (!checked(device->CreateSamplerState(&sampling, &sampler), "NIS sampler"))
            return false;
        const auto coefficient = [&](const float* values, ComPtr<ID3D11ShaderResourceView>& view) {
            D3D11_TEXTURE2D_DESC description{};
            description.Width = kFilterSize / 4;
            description.Height = kPhaseCount;
            description.MipLevels = description.ArraySize = 1;
            description.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            description.SampleDesc.Count = 1;
            description.Usage = D3D11_USAGE_IMMUTABLE;
            description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA data{values, kFilterSize * sizeof(float), 0};
            ComPtr<ID3D11Texture2D> texture;
            if (!checked(device->CreateTexture2D(&description, &data, &texture),
                         "NIS coefficients"))
                return false;
            if (!checked(device->CreateShaderResourceView(texture.Get(), nullptr, &view),
                         "NIS coefficient view"))
                return false;
            return true;
        };
        if (!coefficient(&coef_scale[0][0], scaler) || !coefficient(&coef_usm[0][0], usm))
            return false;
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC description{};
        if (SUCCEEDED(device.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)) &&
            SUCCEEDED(adapter->GetDesc(&description))) {
            char name[256]{};
            WideCharToMultiByte(CP_UTF8, 0, description.Description, -1, name, sizeof(name),
                                nullptr, nullptr);
            SDL_Log("NVIDIA NIS compute active on %s", name);
        }
        return true;
    }

    SDL_Texture* run(SDL_Renderer* renderer, SDL_Texture* source, int width, int height) {
        auto* native = texture_for(source);
        if (!native) {
            SDL_SetError("SDL source has no D3D11 texture");
            return nullptr;
        }
        if (input.Get() != native) {
            input = native;
            input_view.Reset();
            if (!checked(device->CreateShaderResourceView(input.Get(), nullptr, &input_view),
                         "NIS input view"))
                return nullptr;
        }
        // NVIDIA supports 1x..2x per pass. Cascading bounded passes supports
        // 320x240 -> large displays without feeding invalid ratios to NIS.
        std::vector<std::pair<int, int>> sizes;
        int w = source->w, h = source->h;
        do {
            w = std::min(width, w * 2);
            h = std::min(height, h * 2);
            sizes.emplace_back(w, h);
        } while (w != width || h != height);
        bool rebuild = stages.size() != sizes.size();
        for (size_t i = 0; !rebuild && i < sizes.size(); ++i)
            rebuild = stages[i].width != sizes[i].first || stages[i].height != sizes[i].second;
        if (rebuild) {
            stages.clear();
            for (const auto [sw, sh] : sizes) {
                Stage stage{};
                stage.width = sw;
                stage.height = sh;
                D3D11_TEXTURE2D_DESC description{};
                description.Width = static_cast<UINT>(sw);
                description.Height = static_cast<UINT>(sh);
                description.MipLevels = description.ArraySize = 1;
                description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                description.SampleDesc.Count = 1;
                description.Usage = D3D11_USAGE_DEFAULT;
                description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
                if (!checked(device->CreateTexture2D(&description, nullptr, &stage.texture),
                             "NIS output texture"))
                    return nullptr;
                if (!checked(
                        device->CreateShaderResourceView(stage.texture.Get(), nullptr, &stage.view),
                        "NIS output read view"))
                    return nullptr;
                if (!checked(device->CreateUnorderedAccessView(stage.texture.Get(), nullptr,
                                                               &stage.output),
                             "NIS output write view"))
                    return nullptr;
                stages.push_back(std::move(stage));
            }
        }
        if (!display || display->w != width || display->h != height) {
            if (display)
                SDL_DestroyTexture(display);
            display = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC,
                                        width, height);
            if (!display || !SDL_SetTextureBlendMode(display, SDL_BLENDMODE_NONE) ||
                !SDL_SetTextureScaleMode(display, SDL_SCALEMODE_LINEAR))
                return nullptr;
        }
        auto* destination = texture_for(display);
        if (!destination) {
            SDL_SetError("SDL destination has no D3D11 texture");
            return nullptr;
        }
        D3D11_TEXTURE2D_DESC destination_description{};
        destination->GetDesc(&destination_description);
        if (destination_description.Format != DXGI_FORMAT_R8G8B8A8_UNORM) {
            SDL_SetError("NIS destination format mismatch");
            return nullptr;
        }
        if (!SDL_FlushRenderer(renderer))
            return nullptr;
        // SDL creates a single-threaded D3D11 device, which disallows deferred
        // contexts. Preserve the compute state on its immediate context instead.
        struct ComputeState {
            ID3D11DeviceContext* context;
            ComPtr<ID3D11ComputeShader> shader;
            ID3D11ShaderResourceView* views[3]{};
            ID3D11UnorderedAccessView* output = nullptr;
            ID3D11SamplerState* sampler = nullptr;
            ID3D11Buffer* constants = nullptr;
            explicit ComputeState(ID3D11DeviceContext* c) : context(c) {
                c->CSGetShader(&shader, nullptr, nullptr);
                c->CSGetShaderResources(0, 3, views);
                c->CSGetUnorderedAccessViews(0, 1, &output);
                c->CSGetSamplers(0, 1, &sampler);
                c->CSGetConstantBuffers(0, 1, &constants);
            }
            ~ComputeState() {
                context->CSSetShader(shader.Get(), nullptr, 0);
                context->CSSetShaderResources(0, 3, views);
                context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
                context->CSSetSamplers(0, 1, &sampler);
                context->CSSetConstantBuffers(0, 1, &constants);
                for (auto* v : views)
                    if (v)
                        v->Release();
                if (output)
                    output->Release();
                if (sampler)
                    sampler->Release();
                if (constants)
                    constants->Release();
            }
        } restore(immediate.Get());
        ID3D11ShaderResourceView* view = input_view.Get();
        w = source->w;
        h = source->h;
        for (size_t i = 0; i < stages.size(); ++i) {
            auto& stage = stages[i];
            NISConfig config{};
            // Only the final pass adds sharpening; intermediate passes avoid
            // repeatedly amplifying contrast. 35% is deliberately moderate.
            const auto iw = static_cast<uint32_t>(w), ih = static_cast<uint32_t>(h);
            const auto ow = static_cast<uint32_t>(stage.width),
                       oh = static_cast<uint32_t>(stage.height);
            if (!NVScalerUpdateConfig(config, i + 1 == stages.size() ? 0.35f : 0.f, 0, 0, iw, ih,
                                      iw, ih, 0, 0, ow, oh, ow, oh)) {
                SDL_SetError("NIS scale ratio outside 1x..2x");
                return nullptr;
            }
            immediate->UpdateSubresource(constants.Get(), 0, nullptr, &config, 0, 0);
            ID3D11ShaderResourceView* views[] = {view, scaler.Get(), usm.Get()};
            immediate->CSSetShaderResources(0, 3, views);
            immediate->CSSetUnorderedAccessViews(0, 1, stage.output.GetAddressOf(), nullptr);
            immediate->CSSetSamplers(0, 1, sampler.GetAddressOf());
            immediate->CSSetConstantBuffers(0, 1, constants.GetAddressOf());
            immediate->CSSetShader(shader.Get(), nullptr, 0);
            immediate->Dispatch((ow + 31) / 32, (oh + 23) / 24, 1);
            ID3D11UnorderedAccessView* unbound = nullptr;
            immediate->CSSetUnorderedAccessViews(0, 1, &unbound, nullptr);
            view = stage.view.Get();
            w = stage.width;
            h = stage.height;
        }
        immediate->CopyResource(destination, stages.back().texture.Get());

        if (!checked(device->GetDeviceRemovedReason(), "NIS device status"))
            return nullptr;
        return display;
    }
};

NisScaler::NisScaler() = default;
NisScaler::~NisScaler() = default;
void NisScaler::reset() {
    impl_.reset();
    failed_ = false;
}
bool NisScaler::available(SDL_Renderer* renderer) const {
    return !failed_ && device_for(renderer);
}
SDL_Texture* NisScaler::upscale(SDL_Renderer* renderer, SDL_Texture* source, int width,
                                int height) {
    if (failed_)
        return nullptr;
    if (!impl_) {
        impl_ = std::make_unique<Impl>();
        if (!impl_->initialize(device_for(renderer))) {
            SDL_Log("NVIDIA NIS unavailable: %s; using Sharp scaling", SDL_GetError());
            impl_.reset();
            failed_ = true;
            return nullptr;
        }
    }
    if (auto* result = impl_->run(renderer, source, width, height))
        return result;
    SDL_Log("NVIDIA NIS failed: %s; using Sharp scaling", SDL_GetError());
    impl_.reset();
    failed_ = true;
    return nullptr;
}
}  // namespace ipod::video
#else
namespace ipod::video {
struct NisScaler::Impl {};
NisScaler::NisScaler() = default;
NisScaler::~NisScaler() = default;
void NisScaler::reset() {
    impl_.reset();
    failed_ = false;
}
bool NisScaler::available(SDL_Renderer*) const {
    return false;
}
SDL_Texture* NisScaler::upscale(SDL_Renderer*, SDL_Texture*, int, int) {
    if (!failed_)
        SDL_Log("NVIDIA NIS requires Windows/D3D11; using Sharp scaling");
    failed_ = true;
    return nullptr;
}
}  // namespace ipod::video
#endif
