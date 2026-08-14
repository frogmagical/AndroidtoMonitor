#include "capture.h"

#include <d3d11_1.h>
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

bool DesktopCapture::Initialize(int wantW, int wantH) {
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

bool DesktopCapture::PumpFrame(int timeoutMs) {
    if (!dupl_) {
        if (!CreateDuplication()) {
            Sleep(200);
            return false;
        }
    }

    if (frameHeld_) {
        dupl_->ReleaseFrame();
        frameHeld_ = false;
    }

    DXGI_OUTDUPL_FRAME_INFO fi{};
    ComPtr<IDXGIResource> resource;
    HRESULT hr = dupl_->AcquireNextFrame(static_cast<UINT>(timeoutMs), &fi, &resource);

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        LOGW("duplication access lost, re-creating");
        ReleaseDuplication();
        return false;
    }
    if (FAILED(hr)) {
        LOGE("AcquireNextFrame failed: %s", HrString(hr).c_str());
        ReleaseDuplication();
        return false;
    }
    frameHeld_ = true;

    ComPtr<ID3D11Texture2D> acquired;
    if (FAILED(resource.As(&acquired))) return false;

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
            return false;
        }
        LOGI("desktop surface: %ux%u format=%d", desc.Width, desc.Height,
             static_cast<int>(desc.Format));
    }

    context_->CopyResource(lastBgra_.Get(), acquired.Get());
    haveFrame_ = true;
    return true;
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

ID3D11Texture2D* DesktopCapture::ConvertLatestToNv12() {
    if (!haveFrame_ || !lastBgra_) return nullptr;

    if (!vpInputView_) {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
        ivd.FourCC = 0;
        ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        ivd.Texture2D.MipSlice = 0;
        ivd.Texture2D.ArraySlice = 0;
        HRESULT hr = videoDevice_->CreateVideoProcessorInputView(lastBgra_.Get(), vpEnum_.Get(),
                                                                 &ivd, &vpInputView_);
        if (FAILED(hr)) {
            LOGE("CreateVideoProcessorInputView failed: %s", HrString(hr).c_str());
            return nullptr;
        }
    }

    const int slot = nv12Next_;
    nv12Next_ = (nv12Next_ + 1) % kNv12PoolSize;

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.OutputIndex = 0;
    stream.InputFrameOrField = 0;
    stream.PastFrames = 0;
    stream.FutureFrames = 0;
    stream.pInputSurface = vpInputView_.Get();

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
