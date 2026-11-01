#include "rtx_video.h"

#if defined(_WIN32) && defined(IPOD_HAS_RTX_VIDEO)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11_1.h>
#include <dxgi.h>
#include <nvsdk_ngx_helpers_vsr.h>
#include <string>
#include <wrl/client.h>

namespace ipod::video {
using Microsoft::WRL::ComPtr;
namespace {
bool checked(HRESULT result, const char* operation) {
    return SUCCEEDED(result) || SDL_SetError("%s: HRESULT 0x%08x", operation, unsigned(result));
}
bool checked_ngx(NVSDK_NGX_Result result, const char* operation) {
    return NVSDK_NGX_SUCCEED(result) || SDL_SetError("%s: NGX 0x%08x", operation, unsigned(result));
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
// NGX may change any graphics/compute bindings. Restore SDL's complete state,
// including on errors; restoring only compute slots is insufficient here.
struct ContextScope {
    ID3D11DeviceContext1* context;
    ComPtr<ID3DDeviceContextState> previous;
    ContextScope(ID3D11DeviceContext1* c, ID3DDeviceContextState* state) : context(c) {
        context->SwapDeviceContextState(state, &previous);
    }
    ~ContextScope() { context->SwapDeviceContextState(previous.Get(), nullptr); }
};
}  // namespace
struct RtxVideoScaler::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext1> context;
    ComPtr<ID3DDeviceContextState> state;
    ComPtr<ID3D11Texture2D> output;
    NVSDK_NGX_Parameter* parameters = nullptr;
    NVSDK_NGX_Handle* feature = nullptr;
    bool initialized = false, logged = false;
    SDL_Texture* display = nullptr;
    ~Impl() {
        if (initialized) {
            ContextScope scope(context.Get(), state.Get());
            if (feature)
                NVSDK_NGX_D3D11_ReleaseFeature(feature);
            if (parameters)
                NVSDK_NGX_D3D11_DestroyParameters(parameters);
            NVSDK_NGX_D3D11_Shutdown1(device.Get());
        }
        if (display)
            SDL_DestroyTexture(display);
    }
    bool initialize(ID3D11Device* supplied) {
        device = supplied;
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC description{};
        if (!device || FAILED(device.As(&dxgi)) || FAILED(dxgi->GetAdapter(&adapter)) ||
            FAILED(adapter->GetDesc(&description)) || description.VendorId != 0x10de)
            return SDL_SetError("RTX Video requires an NVIDIA D3D11 adapter");
        ComPtr<ID3D11Device1> device1;
        ComPtr<ID3D11DeviceContext> immediate;
        device->GetImmediateContext(&immediate);
        if (!checked(device.As(&device1), "RTX device interface") ||
            !checked(immediate.As(&context), "RTX context interface"))
            return false;
        const auto level = device->GetFeatureLevel();
        const UINT flags = (device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED)
                               ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED
                               : 0;
        if (!checked(device1->CreateDeviceContextState(flags, &level, 1, D3D11_SDK_VERSION,
                                                       __uuidof(ID3D11Device), nullptr, &state),
                     "RTX isolated context"))
            return false;
        ContextScope scope(context.Get(), state.Get());
        // App ID 0 matches NVIDIA's RTX Video SDK sample integration.
        // SDK searches beside the executable for the separately installed runtime.
        char* path = SDL_GetPrefPath("iPodClickWheelRecomps", "RTXVideo");
        if (!path)
            return false;
        const int size = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
        std::wstring wide(size > 0 ? size_t(size) : 0, L'\0');
        if (size > 0)
            MultiByteToWideChar(CP_UTF8, 0, path, -1, wide.data(), size);
        SDL_free(path);
        if (!size)
            return SDL_SetError("RTX cache path conversion failed");
        if (!checked_ngx(NVSDK_NGX_D3D11_Init(0, wide.c_str(), device.Get()), "RTX initialization"))
            return false;
        initialized = true;
        if (!checked_ngx(NVSDK_NGX_D3D11_GetCapabilityParameters(&parameters), "RTX capabilities"))
            return false;
        int supported = 0;
        if (!checked_ngx(parameters->Get(NVSDK_NGX_Parameter_VSR_Available, &supported),
                         "RTX availability") ||
            !supported)
            return SDL_SetError("RTX Video unavailable; check the NVIDIA runtime and driver");
        NVSDK_NGX_Feature_Create_Params create{};
        return checked_ngx(NGX_D3D11_CREATE_VSR_EXT(context.Get(), &feature, parameters, &create),
                           "RTX create VSR");
    }
    SDL_Texture* run(SDL_Renderer* renderer, SDL_Texture* source, int width, int height) {
        auto* input = texture_for(source);
        if (!input) {
            SDL_SetError("RTX input is not a native D3D11 texture");
            return nullptr;
        }
        if (!display || display->w != width || display->h != height) {
            if (display)
                SDL_DestroyTexture(display);
            display = nullptr;
            output.Reset();
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = unsigned(width);
            desc.Height = unsigned(height);
            desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.BindFlags =
                D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET;
            if (!checked(device->CreateTexture2D(&desc, nullptr, &output), "RTX output texture"))
                return nullptr;
            display = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC,
                                        width, height);
            if (!display || !SDL_SetTextureScaleMode(display, SDL_SCALEMODE_LINEAR))
                return nullptr;
        }
        auto* destination = texture_for(display);
        if (!destination) {
            SDL_SetError("RTX output is not a native D3D11 texture");
            return nullptr;
        }
        ContextScope scope(context.Get(), state.Get());
        NVSDK_NGX_D3D11_VSR_Eval_Params evaluate{};
        evaluate.pInput = input;
        evaluate.pOutput = output.Get();
        evaluate.InputSubrectSize = {unsigned(source->w), unsigned(source->h)};
        evaluate.OutputSubrectSize = {unsigned(width), unsigned(height)};
        evaluate.QualityLevel = NVSDK_NGX_VSR_Quality_Ultra;
        if (!checked_ngx(NGX_D3D11_EVALUATE_VSR_EXT(context.Get(), feature, parameters, &evaluate),
                         "RTX VSR inference"))
            return nullptr;
        context->CopyResource(destination, output.Get());
        if (!logged) {
            SDL_Log("RTX Video AI: NVIDIA VSR Ultra inference active (%dx%d -> %dx%d)", source->w,
                    source->h, width, height);
            logged = true;
        }
        return display;
    }
};
RtxVideoScaler::RtxVideoScaler() = default;
RtxVideoScaler::~RtxVideoScaler() = default;
void RtxVideoScaler::reset() {
    impl_.reset();
    failed_ = false;
}
bool RtxVideoScaler::available(SDL_Renderer* renderer) const {
    return !failed_ && device_for(renderer);
}
SDL_Texture* RtxVideoScaler::upscale(SDL_Renderer* renderer, SDL_Texture* source, int width,
                                     int height) {
    if (failed_)
        return nullptr;
    if (SDL_FlushRenderer(renderer)) {
        if (!impl_) {
            impl_ = std::make_unique<Impl>();
            if (!impl_->initialize(device_for(renderer))) {
                SDL_Log("RTX Video AI unavailable: %s; using Sharp", SDL_GetError());
                impl_.reset();
                failed_ = true;
                return nullptr;
            }
        }
        if (auto* result = impl_->run(renderer, source, width, height))
            return result;
    }
    SDL_Log("RTX Video AI failed: %s; using Sharp", SDL_GetError());
    impl_.reset();
    failed_ = true;
    return nullptr;
}
}  // namespace ipod::video
#else
namespace ipod::video {
struct RtxVideoScaler::Impl {};
RtxVideoScaler::RtxVideoScaler() = default;
RtxVideoScaler::~RtxVideoScaler() = default;
void RtxVideoScaler::reset() {
    failed_ = false;
}
bool RtxVideoScaler::available(SDL_Renderer*) const {
    return false;
}
SDL_Texture* RtxVideoScaler::upscale(SDL_Renderer*, SDL_Texture*, int, int) {
    if (!failed_)
        SDL_Log("RTX Video AI not included in this build; using Sharp");
    failed_ = true;
    return nullptr;
}
}  // namespace ipod::video
#endif
