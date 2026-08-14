#include "capture.h"

#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>

#include "util.h"

using Microsoft::WRL::ComPtr;

namespace a2m {
namespace {

const char* RotationName(UINT r) {
    switch (r) {
        case DXGI_MODE_ROTATION_IDENTITY:
            return "identity";
        case DXGI_MODE_ROTATION_ROTATE90:
            return "rotate90";
        case DXGI_MODE_ROTATION_ROTATE180:
            return "rotate180";
        case DXGI_MODE_ROTATION_ROTATE270:
            return "rotate270";
        default:
            return "unspecified";
    }
}

// Textured-quad pass used to blend the cursor over the desktop copy. The quad is
// generated from SV_VertexID (no vertex/index buffers), and the destination rect
// arrives in clip space through the constant buffer.
constexpr char kCursorShaderHlsl[] = R"(
cbuffer Params : register(b0) { float4 gRect; };  // x0, y0(top), x1, y1(bottom) in clip space

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VSOut VSMain(uint vid : SV_VertexID) {
    float2 t = float2(vid & 1, (vid >> 1) & 1);
    VSOut o;
    o.pos = float4(lerp(gRect.x, gRect.z, t.x), lerp(gRect.y, gRect.w, t.y), 0.0f, 1.0f);
    o.uv  = t;
    return o;
}

Texture2D    gTex : register(t0);
SamplerState gSmp : register(s0);

float4 PSMain(VSOut i) : SV_Target {
    float4 c = gTex.Sample(gSmp, i.uv);
    // Fully transparent texels must not reach the inverting blend state, which
    // ignores source alpha.
    if (c.a < 0.004f) discard;
    return c;
}
)";

struct CursorParams {
    float rect[4];
};

bool CompileShader(const char* entry, const char* target, ComPtr<ID3DBlob>* blob) {
    ComPtr<ID3DBlob> errors;
    const UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS;
    const HRESULT hr =
        D3DCompile(kCursorShaderHlsl, sizeof(kCursorShaderHlsl) - 1, "cursor.hlsl", nullptr,
                   nullptr, entry, target, flags, 0, &(*blob), &errors);
    if (FAILED(hr)) {
        LOGE("cursor shader %s compile failed: %s %s", entry, HrString(hr).c_str(),
             errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
        return false;
    }
    return true;
}

}  // namespace

DesktopCapture::~DesktopCapture() { ReleaseDuplication(); }

std::vector<OutputInfo> DesktopCapture::EnumerateOutputs() {
    std::vector<OutputInfo> result;

    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return result;

    for (UINT ai = 0;; ++ai) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(ai, &adapter) == DXGI_ERROR_NOT_FOUND) break;

        DXGI_ADAPTER_DESC1 ad{};
        adapter->GetDesc1(&ad);

        for (UINT oi = 0;; ++oi) {
            ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(oi, &output) == DXGI_ERROR_NOT_FOUND) break;

            DXGI_OUTPUT_DESC od{};
            output->GetDesc(&od);

            OutputInfo info;
            info.adapterName = WideToUtf8(ad.Description);
            info.deviceName = WideToUtf8(od.DeviceName);
            info.width = od.DesktopCoordinates.right - od.DesktopCoordinates.left;
            info.height = od.DesktopCoordinates.bottom - od.DesktopCoordinates.top;
            info.adapterIndex = static_cast<int>(ai);
            info.outputIndex = static_cast<int>(oi);
            info.rotation = static_cast<UINT>(od.Rotation);
            info.attached = od.AttachedToDesktop != FALSE;
            result.push_back(std::move(info));
        }
    }
    return result;
}

bool DesktopCapture::Initialize(int wantW, int wantH, bool drawCursor) {
    drawCursor_ = drawCursor;
    const std::vector<OutputInfo> outputs = EnumerateOutputs();

    const OutputInfo* match = nullptr;
    for (const auto& o : outputs) {
        if (o.attached && o.width == wantW && o.height == wantH) {
            match = &o;
            break;
        }
    }

    if (!match) {
        LOGE("no attached DXGI output with %dx%d found. Available outputs:", wantW, wantH);
        for (const auto& o : outputs) {
            LOGE("  adapter[%d] output[%d] %s on '%s'  %dx%d  rotation=%s attached=%s",
                 o.adapterIndex, o.outputIndex, o.deviceName.c_str(), o.adapterName.c_str(), o.width,
                 o.height, RotationName(o.rotation), o.attached ? "yes" : "no");
        }
        LOGE("Is the Virtual Display Driver installed and set to %dx%d? (docs/REQUIREMENTS.md §8)",
             wantW, wantH);
        return false;
    }

    selected_ = *match;
    width_ = wantW;
    height_ = wantH;
    LOGI("capture target: adapter[%d] output[%d] %s on '%s' %dx%d rotation=%s",
         selected_.adapterIndex, selected_.outputIndex, selected_.deviceName.c_str(),
         selected_.adapterName.c_str(), selected_.width, selected_.height,
         RotationName(selected_.rotation));

    if (selected_.rotation != DXGI_MODE_ROTATION_IDENTITY &&
        selected_.rotation != DXGI_MODE_ROTATION_UNSPECIFIED) {
        LOGW("output rotation is %s; only identity is handled, image may be wrong",
             RotationName(selected_.rotation));
    }

    // Re-acquire the adapter/output objects (the enumeration above dropped them).
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        LOGE("CreateDXGIFactory1 failed: %s", HrString(hr).c_str());
        return false;
    }
    hr = factory->EnumAdapters1(static_cast<UINT>(selected_.adapterIndex), &adapter_);
    if (FAILED(hr)) {
        LOGE("EnumAdapters1(%d) failed: %s", selected_.adapterIndex, HrString(hr).c_str());
        return false;
    }
    ComPtr<IDXGIOutput> output;
    hr = adapter_->EnumOutputs(static_cast<UINT>(selected_.outputIndex), &output);
    if (FAILED(hr)) {
        LOGE("EnumOutputs(%d) failed: %s", selected_.outputIndex, HrString(hr).c_str());
        return false;
    }
    hr = output.As(&output_);
    if (FAILED(hr)) {
        LOGE("IDXGIOutput1 query failed: %s", HrString(hr).c_str());
        return false;
    }

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL got{};
    hr = D3D11CreateDevice(adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                           D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                           levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &device_, &got, &context_);
    if (FAILED(hr)) {
        LOGE("D3D11CreateDevice failed: %s", HrString(hr).c_str());
        return false;
    }

    // Media Foundation hardware MFTs share this device across threads.
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(device_.As(&mt))) mt->SetMultithreadProtected(TRUE);

    if (FAILED(device_.As(&videoDevice_)) || FAILED(context_.As(&videoContext_))) {
        LOGE("ID3D11VideoDevice/ID3D11VideoContext not available on this device");
        return false;
    }

    if (!CreateDuplication()) return false;
    if (!EnsureVideoProcessor()) return false;
    if (!EnsureNv12Pool()) return false;
    if (drawCursor_ && !EnsureCursorPipeline()) {
        LOGW("cursor pipeline unavailable, continuing without cursor compositing");
        drawCursor_ = false;
    }
    LOGI("cursor compositing: %s", drawCursor_ ? "enabled" : "disabled");
    return true;
}

bool DesktopCapture::CreateDuplication() {
    dupl_.Reset();
    frameHeld_ = false;

    HRESULT hr = output_->DuplicateOutput(device_.Get(), &dupl_);
    if (FAILED(hr)) {
        if (hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE) {
            LOGE("DuplicateOutput unavailable: another process already duplicates this output");
        } else {
            LOGE("DuplicateOutput failed: %s", HrString(hr).c_str());
        }
        return false;
    }

    DXGI_OUTDUPL_DESC dd{};
    dupl_->GetDesc(&dd);
    if (static_cast<int>(dd.ModeDesc.Width) != width_ ||
        static_cast<int>(dd.ModeDesc.Height) != height_) {
        LOGW("duplication mode %ux%u differs from desktop rect %dx%d", dd.ModeDesc.Width,
             dd.ModeDesc.Height, width_, height_);
    }
    return true;
}

void DesktopCapture::ReleaseDuplication() {
    if (dupl_ && frameHeld_) {
        dupl_->ReleaseFrame();
        frameHeld_ = false;
    }
    dupl_.Reset();
}

void DesktopCapture::UpdatePointerShape(const DXGI_OUTDUPL_FRAME_INFO& info) {
    if (info.PointerShapeBufferSize == 0) return;

    if (shapeBuffer_.size() < info.PointerShapeBufferSize) {
        shapeBuffer_.resize(info.PointerShapeBufferSize);
    }
    UINT required = 0;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO shapeInfo{};
    const HRESULT hr =
        dupl_->GetFramePointerShape(static_cast<UINT>(shapeBuffer_.size()), shapeBuffer_.data(),
                                    &required, &shapeInfo);
    if (FAILED(hr)) {
        LOGW("GetFramePointerShape failed: %s", HrString(hr).c_str());
        return;
    }

    cursorStats_.lastShapeType = shapeInfo.Type;
    DecodedCursor decoded;
    if (!DecodePointerShape(shapeInfo, shapeBuffer_.data(), required, &decoded)) {
        ++cursorStats_.unsupportedShapes;
        LOGW("unsupported pointer shape type=%u (%s) %ux%u - cursor will not be drawn",
             shapeInfo.Type, PointerShapeTypeName(shapeInfo.Type), shapeInfo.Width,
             shapeInfo.Height);
        cursorShape_ = DecodedCursor{};
        return;
    }

    ++cursorStats_.shapeUpdates;
    switch (shapeInfo.Type) {
        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME:
            ++cursorStats_.monochromeShapes;
            break;
        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR:
            ++cursorStats_.colorShapes;
            break;
        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR:
            ++cursorStats_.maskedColorShapes;
            break;
        default:
            break;
    }
    if (decoded.hasInvert) ++cursorStats_.invertingShapes;

    if (!UploadCursorTextures(decoded)) {
        cursorShape_ = DecodedCursor{};
        return;
    }
    cursorShape_ = std::move(decoded);
}

bool DesktopCapture::UploadCursorTextures(const DecodedCursor& decoded) {
    if (decoded.width <= 0 || decoded.height <= 0) return false;

    // Recreate whenever the shape size changes (cursors are tiny, so this is cheap
    // and only happens when the shape itself changes).
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(decoded.width);
    desc.Height = static_cast<UINT>(decoded.height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    auto upload = [&](const std::vector<uint8_t>& pixels, bool present,
                      ComPtr<ID3D11Texture2D>* tex, ComPtr<ID3D11ShaderResourceView>* srv) -> bool {
        tex->Reset();
        srv->Reset();
        if (!present || pixels.empty()) return true;

        D3D11_SUBRESOURCE_DATA init{};
        init.pSysMem = pixels.data();
        init.SysMemPitch = static_cast<UINT>(decoded.width) * 4;
        HRESULT hr = device_->CreateTexture2D(&desc, &init, &(*tex));
        if (FAILED(hr)) {
            LOGW("cursor texture create failed: %s", HrString(hr).c_str());
            return false;
        }
        hr = device_->CreateShaderResourceView(tex->Get(), nullptr, &(*srv));
        if (FAILED(hr)) {
            LOGW("cursor SRV create failed: %s", HrString(hr).c_str());
            tex->Reset();
            return false;
        }
        return true;
    };

    const bool okColor =
        upload(decoded.colorBgra, decoded.hasColor, &cursorColorTex_, &cursorColorSrv_);
    const bool okInvert =
        upload(decoded.invertBgra, decoded.hasInvert, &cursorInvertTex_, &cursorInvertSrv_);
    return okColor && okInvert;
}

PumpResult DesktopCapture::PumpFrame(int timeoutMs) {
    PumpResult result;

    if (!dupl_) {
        if (!CreateDuplication()) {
            Sleep(200);
            return result;
        }
    }

    if (frameHeld_) {
        dupl_->ReleaseFrame();
        frameHeld_ = false;
    }

    DXGI_OUTDUPL_FRAME_INFO fi{};
    ComPtr<IDXGIResource> resource;
    HRESULT hr = dupl_->AcquireNextFrame(static_cast<UINT>(timeoutMs), &fi, &resource);

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return result;
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        LOGW("duplication access lost, re-creating");
        ReleaseDuplication();
        return result;
    }
    if (FAILED(hr)) {
        LOGE("AcquireNextFrame failed: %s", HrString(hr).c_str());
        ReleaseDuplication();
        return result;
    }
    frameHeld_ = true;

    // A pointer-only update also returns S_OK, and in that case the returned
    // surface holds no new desktop content (LastPresentTime == 0), so it must not
    // be copied over the good frame we already have.
    if (fi.LastMouseUpdateTime.QuadPart != 0) {
        result.pointerUpdated = true;
        cursorVisible_ = fi.PointerPosition.Visible != FALSE;
        cursorStats_.visible = cursorVisible_;
        if (cursorVisible_) {
            cursorX_ = fi.PointerPosition.Position.x;
            cursorY_ = fi.PointerPosition.Position.y;
        }
        if (drawCursor_) UpdatePointerShape(fi);
    }

    if (fi.LastPresentTime.QuadPart == 0 && fi.AccumulatedFrames == 0) {
        return result;  // pointer-only update
    }

    ComPtr<ID3D11Texture2D> acquired;
    if (FAILED(resource.As(&acquired))) return result;

    D3D11_TEXTURE2D_DESC desc{};
    acquired->GetDesc(&desc);

    if (!lastBgra_) {
        D3D11_TEXTURE2D_DESC copyDesc = desc;
        copyDesc.Usage = D3D11_USAGE_DEFAULT;
        copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        copyDesc.CPUAccessFlags = 0;
        copyDesc.MiscFlags = 0;
        HRESULT chr = device_->CreateTexture2D(&copyDesc, nullptr, &lastBgra_);
        if (FAILED(chr)) {
            LOGE("CreateTexture2D(BGRA copy) failed: %s", HrString(chr).c_str());
            return result;
        }
        LOGI("desktop surface: %ux%u format=%d", desc.Width, desc.Height,
             static_cast<int>(desc.Format));
    }

    context_->CopyResource(lastBgra_.Get(), acquired.Get());
    haveFrame_ = true;
    result.desktopUpdated = true;
    return result;
}

bool DesktopCapture::EnsureVideoProcessor() {
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputWidth = static_cast<UINT>(width_);
    cd.InputHeight = static_cast<UINT>(height_);
    cd.OutputWidth = static_cast<UINT>(width_);
    cd.OutputHeight = static_cast<UINT>(height_);
    cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    HRESULT hr = videoDevice_->CreateVideoProcessorEnumerator(&cd, &vpEnum_);
    if (FAILED(hr)) {
        LOGE("CreateVideoProcessorEnumerator failed: %s", HrString(hr).c_str());
        return false;
    }
    hr = videoDevice_->CreateVideoProcessor(vpEnum_.Get(), 0, &vp_);
    if (FAILED(hr)) {
        LOGE("CreateVideoProcessor failed: %s", HrString(hr).c_str());
        return false;
    }

    videoContext_->VideoProcessorSetStreamFrameFormat(vp_.Get(), 0,
                                                     D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    videoContext_->VideoProcessorSetStreamAutoProcessingMode(vp_.Get(), 0, FALSE);

    // Desktop content is full-range RGB; H.264 output is BT.709 studio-range YCbCr
    // (matching the MF_MT_YUV_MATRIX/NOMINAL_RANGE attributes set on the encoder).
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE inCs{};
    inCs.Usage = 0;          // playback
    inCs.RGB_Range = 0;      // 0-255
    inCs.YCbCr_Matrix = 1;   // BT.709
    inCs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
    videoContext_->VideoProcessorSetStreamColorSpace(vp_.Get(), 0, &inCs);

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE outCs{};
    outCs.Usage = 0;
    outCs.RGB_Range = 0;
    outCs.YCbCr_Matrix = 1;  // BT.709
    outCs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    videoContext_->VideoProcessorSetOutputColorSpace(vp_.Get(), &outCs);
    return true;
}

bool DesktopCapture::EnsureNv12Pool() {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width_);
    desc.Height = static_cast<UINT>(height_);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    for (int i = 0; i < kNv12PoolSize; ++i) {
        HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &nv12Pool_[i]);
        if (FAILED(hr)) {
            // Some drivers reject SHADER_RESOURCE on NV12; retry render-target only.
            D3D11_TEXTURE2D_DESC rtOnly = desc;
            rtOnly.BindFlags = D3D11_BIND_RENDER_TARGET;
            hr = device_->CreateTexture2D(&rtOnly, nullptr, &nv12Pool_[i]);
        }
        if (FAILED(hr)) {
            LOGE("CreateTexture2D(NV12 %d) failed: %s", i, HrString(hr).c_str());
            return false;
        }

        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
        ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        ovd.Texture2D.MipSlice = 0;
        hr = videoDevice_->CreateVideoProcessorOutputView(nv12Pool_[i].Get(), vpEnum_.Get(), &ovd,
                                                          &nv12Views_[i]);
        if (FAILED(hr)) {
            LOGE("CreateVideoProcessorOutputView(%d) failed: %s", i, HrString(hr).c_str());
            return false;
        }
    }
    return true;
}

bool DesktopCapture::EnsureCursorPipeline() {
    ComPtr<ID3DBlob> vsBlob, psBlob;
    if (!CompileShader("VSMain", "vs_5_0", &vsBlob)) return false;
    if (!CompileShader("PSMain", "ps_5_0", &psBlob)) return false;

    HRESULT hr = device_->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                             nullptr, &vs_);
    if (FAILED(hr)) {
        LOGE("CreateVertexShader failed: %s", HrString(hr).c_str());
        return false;
    }
    hr = device_->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr,
                                    &ps_);
    if (FAILED(hr)) {
        LOGE("CreatePixelShader failed: %s", HrString(hr).c_str());
        return false;
    }

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(CursorParams);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device_->CreateBuffer(&bd, nullptr, &cb_);
    if (FAILED(hr)) {
        LOGE("CreateBuffer(cursor cbuffer) failed: %s", HrString(hr).c_str());
        return false;
    }

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;  // 1:1 blit, keep cursor edges crisp
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    hr = device_->CreateSamplerState(&sd, &sampler_);
    if (FAILED(hr)) {
        LOGE("CreateSamplerState failed: %s", HrString(hr).c_str());
        return false;
    }

    // Normal cursor pixels: standard source-alpha blend.
    D3D11_BLEND_DESC alpha{};
    alpha.RenderTarget[0].BlendEnable = TRUE;
    alpha.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    alpha.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    alpha.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    alpha.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    alpha.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    alpha.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    alpha.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = device_->CreateBlendState(&alpha, &blendAlpha_);
    if (FAILED(hr)) {
        LOGE("CreateBlendState(alpha) failed: %s", HrString(hr).c_str());
        return false;
    }

    // Inverting pixels: dst = (1 - dst) * src, with src forced to white by the
    // shader, i.e. an exact colour inversion of whatever is underneath.
    D3D11_BLEND_DESC invert{};
    invert.RenderTarget[0].BlendEnable = TRUE;
    invert.RenderTarget[0].SrcBlend = D3D11_BLEND_INV_DEST_COLOR;
    invert.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
    invert.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    invert.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    invert.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    invert.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    invert.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = device_->CreateBlendState(&invert, &blendInvert_);
    if (FAILED(hr)) {
        LOGE("CreateBlendState(invert) failed: %s", HrString(hr).c_str());
        return false;
    }

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    hr = device_->CreateRasterizerState(&rd, &raster_);
    if (FAILED(hr)) {
        LOGE("CreateRasterizerState failed: %s", HrString(hr).c_str());
        return false;
    }
    return true;
}

bool DesktopCapture::EnsureComposeTarget() {
    if (composedBgra_) return true;
    if (!lastBgra_) return false;

    D3D11_TEXTURE2D_DESC desc{};
    lastBgra_->GetDesc(&desc);
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &composedBgra_);
    if (FAILED(hr)) {
        LOGE("CreateTexture2D(compose target) failed: %s", HrString(hr).c_str());
        return false;
    }
    hr = device_->CreateRenderTargetView(composedBgra_.Get(), nullptr, &composedRtv_);
    if (FAILED(hr)) {
        LOGE("CreateRenderTargetView(compose target) failed: %s", HrString(hr).c_str());
        composedBgra_.Reset();
        return false;
    }

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
    ivd.FourCC = 0;
    ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    ivd.Texture2D.MipSlice = 0;
    ivd.Texture2D.ArraySlice = 0;
    hr = videoDevice_->CreateVideoProcessorInputView(composedBgra_.Get(), vpEnum_.Get(), &ivd,
                                                     &composedView_);
    if (FAILED(hr)) {
        LOGE("CreateVideoProcessorInputView(compose target) failed: %s", HrString(hr).c_str());
        composedBgra_.Reset();
        composedRtv_.Reset();
        return false;
    }
    return true;
}

void DesktopCapture::DrawCursorLayer(ID3D11ShaderResourceView* srv, ID3D11BlendState* blend) {
    if (!srv) return;

    const float w = static_cast<float>(width_);
    const float h = static_cast<float>(height_);
    const float x0 = static_cast<float>(cursorX_);
    const float y0 = static_cast<float>(cursorY_);
    const float x1 = x0 + static_cast<float>(cursorShape_.width);
    const float y1 = y0 + static_cast<float>(cursorShape_.height);

    // Pixel rect -> clip space. Clip Y points up, texture V points down, so the
    // top edge maps to gRect.y and the bottom edge to gRect.w.
    CursorParams params{};
    params.rect[0] = (x0 / w) * 2.0f - 1.0f;
    params.rect[1] = 1.0f - (y0 / h) * 2.0f;
    params.rect[2] = (x1 / w) * 2.0f - 1.0f;
    params.rect[3] = 1.0f - (y1 / h) * 2.0f;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    memcpy(mapped.pData, &params, sizeof(params));
    context_->Unmap(cb_.Get(), 0);

    const float blendFactor[4] = {0, 0, 0, 0};
    context_->OMSetBlendState(blend, blendFactor, 0xFFFFFFFF);
    context_->PSSetShaderResources(0, 1, &srv);
    context_->Draw(4, 0);
}

ID3D11Texture2D* DesktopCapture::ComposeCursor() {
    const bool wantCursor = drawCursor_ && cursorVisible_ && cursorShape_.width > 0 &&
                            (cursorColorSrv_ || cursorInvertSrv_);
    if (!wantCursor) return lastBgra_.Get();
    if (!EnsureComposeTarget()) return lastBgra_.Get();

    // Start from the untouched desktop image so the previous cursor position is
    // implicitly erased.
    context_->CopyResource(composedBgra_.Get(), lastBgra_.Get());

    D3D11_VIEWPORT vp{};
    vp.Width = static_cast<float>(width_);
    vp.Height = static_cast<float>(height_);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;

    ID3D11RenderTargetView* rtv = composedRtv_.Get();
    ID3D11SamplerState* samp = sampler_.Get();
    ID3D11Buffer* cb = cb_.Get();

    context_->OMSetRenderTargets(1, &rtv, nullptr);
    context_->RSSetViewports(1, &vp);
    context_->RSSetState(raster_.Get());
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context_->IASetInputLayout(nullptr);
    context_->VSSetShader(vs_.Get(), nullptr, 0);
    context_->VSSetConstantBuffers(0, 1, &cb);
    context_->PSSetShader(ps_.Get(), nullptr, 0);
    context_->PSSetSamplers(0, 1, &samp);

    DrawCursorLayer(cursorColorSrv_.Get(), blendAlpha_.Get());
    DrawCursorLayer(cursorInvertSrv_.Get(), blendInvert_.Get());

    // Unbind so the video processor can read the texture we just rendered into.
    ID3D11RenderTargetView* nullRtv = nullptr;
    ID3D11ShaderResourceView* nullSrv = nullptr;
    context_->OMSetRenderTargets(1, &nullRtv, nullptr);
    context_->PSSetShaderResources(0, 1, &nullSrv);
    return composedBgra_.Get();
}

ID3D11Texture2D* DesktopCapture::ConvertLatestToNv12() {
    if (!haveFrame_ || !lastBgra_) return nullptr;

    if (!lastBgraView_) {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
        ivd.FourCC = 0;
        ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        ivd.Texture2D.MipSlice = 0;
        ivd.Texture2D.ArraySlice = 0;
        HRESULT hr = videoDevice_->CreateVideoProcessorInputView(lastBgra_.Get(), vpEnum_.Get(),
                                                                 &ivd, &lastBgraView_);
        if (FAILED(hr)) {
            LOGE("CreateVideoProcessorInputView failed: %s", HrString(hr).c_str());
            return nullptr;
        }
    }

    ID3D11Texture2D* source = ComposeCursor();
    ID3D11VideoProcessorInputView* sourceView =
        (source == composedBgra_.Get()) ? composedView_.Get() : lastBgraView_.Get();
    if (!sourceView) return nullptr;

    const int slot = nv12Next_;
    nv12Next_ = (nv12Next_ + 1) % kNv12PoolSize;

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.OutputIndex = 0;
    stream.InputFrameOrField = 0;
    stream.PastFrames = 0;
    stream.FutureFrames = 0;
    stream.pInputSurface = sourceView;

    HRESULT hr = videoContext_->VideoProcessorBlt(vp_.Get(), nv12Views_[slot].Get(), 0, 1, &stream);
    if (FAILED(hr)) {
        LOGE("VideoProcessorBlt failed: %s", HrString(hr).c_str());
        return nullptr;
    }
    // The encoder MFT reads this texture on its own threads; flush so the blt is
    // actually submitted before the sample is handed over.
    context_->Flush();
    return nv12Pool_[slot].Get();
}

}  // namespace a2m
