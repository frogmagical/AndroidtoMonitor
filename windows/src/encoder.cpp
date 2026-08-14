#include "encoder.h"

#include <mferror.h>
#include <mftransform.h>

#include <algorithm>

#include "protocol.h"
#include "util.h"

using Microsoft::WRL::ComPtr;

namespace a2m {
namespace {

// Sets an ICodecAPI VT_BOOL/VT_UI4/VT_UI8 property, logging (but tolerating) failure.
bool SetCodecUI32(ICodecAPI* api, const GUID& id, UINT32 value, const char* name) {
    if (!api) return false;
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_UI4;
    v.ulVal = value;
    const HRESULT hr = api->SetValue(&id, &v);
    VariantClear(&v);
    if (FAILED(hr)) {
        LOGW("ICodecAPI %s=%u not accepted: %s", name, value, HrString(hr).c_str());
        return false;
    }
    return true;
}

bool SetCodecBool(ICodecAPI* api, const GUID& id, bool value, const char* name) {
    if (!api) return false;
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BOOL;
    v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    const HRESULT hr = api->SetValue(&id, &v);
    VariantClear(&v);
    if (FAILED(hr)) {
        LOGW("ICodecAPI %s=%d not accepted: %s", name, value ? 1 : 0, HrString(hr).c_str());
        return false;
    }
    return true;
}

}  // namespace

Encoder::~Encoder() { Shutdown(); }

// ---------------------------------------------------------------------------
// IUnknown / IMFAsyncCallback
// ---------------------------------------------------------------------------

STDMETHODIMP Encoder::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == __uuidof(IMFAsyncCallback)) {
        *ppv = static_cast<IMFAsyncCallback*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) Encoder::AddRef() { return ++refCount_; }

STDMETHODIMP_(ULONG) Encoder::Release() {
    // Lifetime is owned by the caller (a stack/member object); never self-delete.
    const ULONG n = --refCount_;
    return n;
}

STDMETHODIMP Encoder::GetParameters(DWORD* flags, DWORD* queue) {
    if (flags) *flags = 0;
    if (queue) *queue = 0;
    return E_NOTIMPL;
}

STDMETHODIMP Encoder::Invoke(IMFAsyncResult* result) {
    if (!running_.load()) return S_OK;

    ComPtr<IMFMediaEvent> ev;
    HRESULT hr = eventGen_ ? eventGen_->EndGetEvent(result, &ev) : MF_E_SHUTDOWN;
    if (FAILED(hr) || !ev) return S_OK;

    MediaEventType met = MEUnknown;
    ev->GetType(&met);

    // Re-arm before doing the work so METransformNeedInput is not queued behind a
    // METransformHaveOutput that is still being copied out; input and output then
    // overlap on the MF work queue instead of serialising per frame.
    if (running_.load() && eventGen_) eventGen_->BeginGetEvent(this, nullptr);

    switch (met) {
        case METransformNeedInput: {
            std::lock_guard<std::mutex> lock(mutex_);
            ++needInput_;
            TryFeedLocked();
            break;
        }
        case METransformHaveOutput: {
            std::lock_guard<std::mutex> lock(outputMutex_);
            PullOutput();
            break;
        }
        case METransformDrainComplete:
            break;
        default:
            break;
    }
    return S_OK;
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

bool Encoder::ActivateTransform(int width, int height) {
    (void)width;
    (void)height;

    MFT_REGISTER_TYPE_INFO inInfo{MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO outInfo{MFMediaType_Video, MFVideoFormat_H264};

    struct Attempt {
        UINT32 flags;
        bool hardware;
        const char* label;
    };
    const Attempt attempts[] = {
        {MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, true, "hardware"},
        {MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_LOCALMFT |
             MFT_ENUM_FLAG_SORTANDFILTER,
         false, "software"},
    };

    for (const Attempt& attempt : attempts) {
        IMFActivate** activates = nullptr;
        UINT32 count = 0;
        HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, attempt.flags, &inInfo, &outInfo,
                               &activates, &count);
        if (FAILED(hr) || count == 0) {
            LOGW("no %s H.264 encoder MFT found (hr=%s count=%u)", attempt.label,
                 HrString(hr).c_str(), count);
            if (activates) CoTaskMemFree(activates);
            continue;
        }

        for (UINT32 i = 0; i < count; ++i) {
            wchar_t* nameW = nullptr;
            UINT32 nameLen = 0;
            activates[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &nameW, &nameLen);
            const std::string name = nameW ? WideToUtf8(nameW) : "(unnamed)";
            if (nameW) CoTaskMemFree(nameW);

            ComPtr<IMFTransform> mft;
            hr = activates[i]->ActivateObject(IID_PPV_ARGS(&mft));
            if (FAILED(hr)) {
                LOGW("ActivateObject('%s') failed: %s", name.c_str(), HrString(hr).c_str());
                continue;
            }

            ComPtr<IMFAttributes> attrs;
            bool d3dAware = false;
            bool async = false;
            if (SUCCEEDED(mft->GetAttributes(&attrs)) && attrs) {
                UINT32 v = 0;
                if (SUCCEEDED(attrs->GetUINT32(MF_SA_D3D11_AWARE, &v))) d3dAware = (v != 0);
                v = 0;
                if (SUCCEEDED(attrs->GetUINT32(MF_TRANSFORM_ASYNC, &v))) async = (v != 0);
                if (async) {
                    hr = attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
                    if (FAILED(hr)) {
                        LOGW("MF_TRANSFORM_ASYNC_UNLOCK failed on '%s': %s", name.c_str(),
                             HrString(hr).c_str());
                        activates[i]->ShutdownObject();
                        continue;
                    }
                }
                attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
            }

            if (attempt.hardware) {
                if (!d3dAware) {
                    LOGW("hardware MFT '%s' is not MF_SA_D3D11_AWARE, skipping", name.c_str());
                    activates[i]->ShutdownObject();
                    continue;
                }
                hr = mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                         reinterpret_cast<ULONG_PTR>(deviceManager_.Get()));
                if (FAILED(hr)) {
                    LOGW("MFT_MESSAGE_SET_D3D_MANAGER failed on '%s': %s", name.c_str(),
                         HrString(hr).c_str());
                    activates[i]->ShutdownObject();
                    continue;
                }
            }

            transform_ = mft;
            encoderName_ = name;
            isHardware_ = attempt.hardware;
            isAsync_ = async;
            transform_.As(&codecApi_);

            for (UINT32 j = 0; j < count; ++j) activates[j]->Release();
            CoTaskMemFree(activates);

            LOGI("encoder MFT: '%s' (%s, %s, d3d11_aware=%s)", encoderName_.c_str(),
                 isHardware_ ? "HARDWARE" : "SOFTWARE", isAsync_ ? "async" : "sync",
                 d3dAware ? "yes" : "no");
            if (!attempt.hardware) {
                LOGW("falling back to a SOFTWARE H.264 encoder - CPU load will be higher than the "
                     "5%% target in REQUIREMENTS §5");
            }
            return true;
        }

        for (UINT32 j = 0; j < count; ++j) activates[j]->Release();
        CoTaskMemFree(activates);
    }

    LOGE("no usable H.264 encoder MFT (hardware or software)");
    return false;
}

bool Encoder::ConfigureCodecApi(bool afterTypes) {
    if (!codecApi_) {
        if (!afterTypes) LOGW("encoder does not expose ICodecAPI; low-latency knobs unavailable");
        return false;
    }

    // REQUIREMENTS §4.2: low latency, CBR, no B-frames, IDR every 2 seconds.
    SetCodecBool(codecApi_.Get(), CODECAPI_AVLowLatencyMode, true, "AVLowLatencyMode");
    SetCodecUI32(codecApi_.Get(), CODECAPI_AVEncCommonRateControlMode,
                 eAVEncCommonRateControlMode_CBR, "AVEncCommonRateControlMode(CBR)");
    SetCodecUI32(codecApi_.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0,
                 "AVEncMPVDefaultBPictureCount");
    SetCodecUI32(codecApi_.Get(), CODECAPI_AVEncMPVGOPSize, static_cast<UINT32>(fps_ * 2),
                 "AVEncMPVGOPSize");
    SetCodecUI32(codecApi_.Get(), CODECAPI_AVEncCommonQualityVsSpeed, 33,
                 "AVEncCommonQualityVsSpeed(speed)");
    // Keep the encoder from buffering more than one frame internally.
    SetCodecUI32(codecApi_.Get(), CODECAPI_AVEncNumWorkerThreads, 1, "AVEncNumWorkerThreads");
    return true;
}

bool Encoder::ConfigureTypes(int width, int height, int fps, uint32_t bitrateBps) {
    ComPtr<IMFMediaType> outType;
    HRESULT hr = MFCreateMediaType(&outType);
    if (FAILED(hr)) return false;

    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    outType->SetUINT32(MF_MT_AVG_BITRATE, bitrateBps);
    outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    outType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
    outType->SetUINT32(MF_MT_MPEG2_LEVEL, static_cast<UINT32>(-1));  // let the MFT pick
    outType->SetUINT32(MF_MT_MAX_KEYFRAME_SPACING, static_cast<UINT32>(fps * 2));
    outType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, FALSE);
    outType->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    outType->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    MFSetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(width),
                       static_cast<UINT32>(height));
    MFSetAttributeRatio(outType.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(fps), 1);
    MFSetAttributeRatio(outType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    hr = transform_->SetOutputType(0, outType.Get(), 0);
    if (FAILED(hr)) {
        // Retry with Baseline: some encoders reject High at unusual resolutions.
        outType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);
        const HRESULT hr2 = transform_->SetOutputType(0, outType.Get(), 0);
        if (FAILED(hr2)) {
            LOGE("SetOutputType(H264 %dx%d @%dfps %u bps) failed: %s (baseline retry: %s)", width,
                 height, fps, bitrateBps, HrString(hr).c_str(), HrString(hr2).c_str());
            return false;
        }
        LOGW("High profile rejected, using Baseline");
    }

    ComPtr<IMFMediaType> inType;
    hr = MFCreateMediaType(&inType);
    if (FAILED(hr)) return false;
    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    inType->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    inType->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(width),
                       static_cast<UINT32>(height));
    MFSetAttributeRatio(inType.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(fps), 1);
    MFSetAttributeRatio(inType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    hr = transform_->SetInputType(0, inType.Get(), 0);
    if (FAILED(hr)) {
        LOGE("SetInputType(NV12 %dx%d) failed: %s", width, height, HrString(hr).c_str());
        return false;
    }

    MFT_OUTPUT_STREAM_INFO osi{};
    if (SUCCEEDED(transform_->GetOutputStreamInfo(0, &osi))) {
        outputProvidesSamples_ =
            (osi.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                            MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        outputBufferSize_ = osi.cbSize ? osi.cbSize : (1u << 20);
    } else {
        outputBufferSize_ = 1u << 20;
    }
    return true;
}

void Encoder::CacheSequenceHeader() {
    ComPtr<IMFMediaType> cur;
    if (FAILED(transform_->GetOutputCurrentType(0, &cur)) || !cur) return;

    UINT32 blobSize = 0;
    if (FAILED(cur->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &blobSize)) || blobSize == 0) return;

    std::vector<uint8_t> blob(blobSize);
    if (FAILED(cur->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, blob.data(), blobSize, &blobSize))) return;

    const AnnexBScan scan = ScanAnnexB(blob.data(), blob.size(), &sps_, &pps_);
    if (scan.hasSps && scan.hasPps) {
        LOGI("cached SPS(%zu B)/PPS(%zu B) from MF_MT_MPEG_SEQUENCE_HEADER", sps_.size(),
             pps_.size());
    }
}

bool Encoder::Initialize(ID3D11Device* device, int width, int height, int fps,
                         uint32_t bitrateBps) {
    device_ = device;
    width_ = width;
    height_ = height;
    fps_ = fps;

    HRESULT hr = MFCreateDXGIDeviceManager(&deviceManagerToken_, &deviceManager_);
    if (FAILED(hr)) {
        LOGE("MFCreateDXGIDeviceManager failed: %s", HrString(hr).c_str());
        return false;
    }
    hr = deviceManager_->ResetDevice(device_.Get(), deviceManagerToken_);
    if (FAILED(hr)) {
        LOGE("IMFDXGIDeviceManager::ResetDevice failed: %s", HrString(hr).c_str());
        return false;
    }

    if (!ActivateTransform(width, height)) return false;

    ConfigureCodecApi(/*afterTypes=*/false);
    if (!ConfigureTypes(width, height, fps, bitrateBps)) {
        if (isHardware_) {
            LOGE("hardware encoder '%s' could not be configured; retrying with software",
                 encoderName_.c_str());
            transform_.Reset();
            codecApi_.Reset();
            isHardware_ = false;
            // Re-run enumeration but skip the hardware attempt this time by
            // clearing the device manager requirement.
            MFT_REGISTER_TYPE_INFO inInfo{MFMediaType_Video, MFVideoFormat_NV12};
            MFT_REGISTER_TYPE_INFO outInfo{MFMediaType_Video, MFVideoFormat_H264};
            IMFActivate** activates = nullptr;
            UINT32 count = 0;
            if (SUCCEEDED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                                    MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT |
                                        MFT_ENUM_FLAG_SORTANDFILTER,
                                    &inInfo, &outInfo, &activates, &count)) &&
                count > 0) {
                if (SUCCEEDED(activates[0]->ActivateObject(IID_PPV_ARGS(&transform_)))) {
                    wchar_t* nameW = nullptr;
                    UINT32 nameLen = 0;
                    activates[0]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &nameW, &nameLen);
                    encoderName_ = nameW ? WideToUtf8(nameW) : "(software)";
                    if (nameW) CoTaskMemFree(nameW);
                    isAsync_ = false;
                    transform_.As(&codecApi_);
                    LOGW("using SOFTWARE encoder '%s'", encoderName_.c_str());
                }
                for (UINT32 j = 0; j < count; ++j) activates[j]->Release();
                CoTaskMemFree(activates);
            }
            if (!transform_) return false;
            ConfigureCodecApi(false);
            if (!ConfigureTypes(width, height, fps, bitrateBps)) return false;
        } else {
            return false;
        }
    }

    // Bitrate/GOP settings often only stick once the output type is known.
    ConfigureCodecApi(/*afterTypes=*/true);
    SetCodecUI32(codecApi_.Get(), CODECAPI_AVEncCommonMeanBitRate, bitrateBps,
                 "AVEncCommonMeanBitRate");
    SetCodecUI32(codecApi_.Get(), CODECAPI_AVEncCommonMaxBitRate, bitrateBps, "AVEncCommonMaxBitRate");
    SetCodecUI32(codecApi_.Get(), CODECAPI_AVEncCommonBufferSize, bitrateBps / 2,
                 "AVEncCommonBufferSize");

    CacheSequenceHeader();

    running_.store(true);

    if (isAsync_) {
        hr = transform_.As(&eventGen_);
        if (FAILED(hr)) {
            LOGE("IMFMediaEventGenerator query failed on async MFT: %s", HrString(hr).c_str());
            return false;
        }
    }

    transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    if (isAsync_ && eventGen_) eventGen_->BeginGetEvent(this, nullptr);

    LOGI("encoder ready: %dx%d @%dfps, %.1f Mbps CBR, GOP=%d, B-frames=0", width, height, fps,
         bitrateBps / 1e6, fps * 2);
    return true;
}

// ---------------------------------------------------------------------------
// Frame submission
// ---------------------------------------------------------------------------

bool Encoder::SubmitFrame(ID3D11Texture2D* nv12, int64_t sampleTime100ns, int64_t captureQpc) {
    if (!running_.load() || !transform_ || !nv12) return false;

    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), nv12, 0, FALSE, &buffer);
    if (FAILED(hr)) {
        LOGE("MFCreateDXGISurfaceBuffer failed: %s", HrString(hr).c_str());
        return false;
    }
    ComPtr<IMF2DBuffer> buffer2d;
    if (SUCCEEDED(buffer.As(&buffer2d))) {
        DWORD length = 0;
        if (SUCCEEDED(buffer2d->GetContiguousLength(&length))) buffer->SetCurrentLength(length);
    }

    ComPtr<IMFSample> sample;
    hr = MFCreateSample(&sample);
    if (FAILED(hr)) return false;
    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(sampleTime100ns);
    sample->SetSampleDuration(10000000LL / std::max(1, fps_));

    {
        std::lock_guard<std::mutex> lock(captureMapMutex_);
        captureTimes_[sampleTime100ns] = captureQpc;
        // The map should never grow: it is drained on every output. Guard anyway.
        while (captureTimes_.size() > 32) captureTimes_.erase(captureTimes_.begin());
    }

    if (isAsync_) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_) {
            // Queue depth 1: newest frame wins, older one is dropped.
            ++encoderDrops_;
        }
        pending_ = sample;
        TryFeedLocked();
        return true;
    }

    hr = transform_->ProcessInput(0, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        DrainSyncOutputs();
        hr = transform_->ProcessInput(0, sample.Get(), 0);
    }
    if (FAILED(hr)) {
        LOGE("ProcessInput failed: %s", HrString(hr).c_str());
        return false;
    }
    DrainSyncOutputs();
    return true;
}

void Encoder::TryFeedLocked() {
    while (needInput_ > 0 && pending_) {
        ComPtr<IMFSample> sample = pending_;
        pending_.Reset();
        --needInput_;
        const HRESULT hr = transform_->ProcessInput(0, sample.Get(), 0);
        if (FAILED(hr)) {
            LOGE("async ProcessInput failed: %s", HrString(hr).c_str());
            ++needInput_;  // put the request back; the sample is lost
            return;
        }
    }
}

void Encoder::DrainSyncOutputs() {
    for (;;) {
        MFT_OUTPUT_DATA_BUFFER out{};
        DWORD status = 0;

        ComPtr<IMFSample> outSample;
        ComPtr<IMFMediaBuffer> outBuffer;
        if (!outputProvidesSamples_) {
            if (FAILED(MFCreateSample(&outSample))) return;
            if (FAILED(MFCreateMemoryBuffer(outputBufferSize_, &outBuffer))) return;
            outSample->AddBuffer(outBuffer.Get());
            out.pSample = outSample.Get();
        }

        const HRESULT hr = transform_->ProcessOutput(0, 1, &out, &status);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            if (out.pEvents) out.pEvents->Release();
            return;
        }
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (out.pEvents) out.pEvents->Release();
            ComPtr<IMFMediaType> newType;
            if (SUCCEEDED(transform_->GetOutputAvailableType(0, 0, &newType))) {
                transform_->SetOutputType(0, newType.Get(), 0);
                CacheSequenceHeader();
            }
            continue;
        }
        if (FAILED(hr)) {
            LOGE("ProcessOutput failed: %s", HrString(hr).c_str());
            if (out.pEvents) out.pEvents->Release();
            return;
        }

        IMFSample* produced = out.pSample;
        if (produced) {
            LONGLONG sampleTime = 0;
            produced->GetSampleTime(&sampleTime);
            ComPtr<IMFMediaBuffer> buf;
            if (SUCCEEDED(produced->ConvertToContiguousBuffer(&buf))) {
                BYTE* data = nullptr;
                DWORD maxLen = 0, curLen = 0;
                if (SUCCEEDED(buf->Lock(&data, &maxLen, &curLen))) {
                    DeliverBitstream(data, curLen, sampleTime);
                    buf->Unlock();
                }
            }
            if (outputProvidesSamples_) produced->Release();
        }
        if (out.pEvents) out.pEvents->Release();
    }
}

void Encoder::PullOutput() {
    MFT_OUTPUT_DATA_BUFFER out{};
    DWORD status = 0;

    ComPtr<IMFSample> outSample;
    ComPtr<IMFMediaBuffer> outBuffer;
    if (!outputProvidesSamples_) {
        if (FAILED(MFCreateSample(&outSample))) return;
        if (FAILED(MFCreateMemoryBuffer(outputBufferSize_, &outBuffer))) return;
        outSample->AddBuffer(outBuffer.Get());
        out.pSample = outSample.Get();
    }

    const HRESULT hr = transform_->ProcessOutput(0, 1, &out, &status);
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        if (out.pEvents) out.pEvents->Release();
        ComPtr<IMFMediaType> newType;
        if (SUCCEEDED(transform_->GetOutputAvailableType(0, 0, &newType))) {
            transform_->SetOutputType(0, newType.Get(), 0);
            CacheSequenceHeader();
        }
        return;
    }
    if (FAILED(hr)) {
        if (hr != MF_E_TRANSFORM_NEED_MORE_INPUT) {
            LOGE("async ProcessOutput failed: %s", HrString(hr).c_str());
        }
        if (out.pEvents) out.pEvents->Release();
        return;
    }

    IMFSample* produced = out.pSample;
    if (produced) {
        LONGLONG sampleTime = 0;
        produced->GetSampleTime(&sampleTime);
        ComPtr<IMFMediaBuffer> buf;
        if (SUCCEEDED(produced->ConvertToContiguousBuffer(&buf))) {
            BYTE* data = nullptr;
            DWORD maxLen = 0, curLen = 0;
            if (SUCCEEDED(buf->Lock(&data, &maxLen, &curLen))) {
                DeliverBitstream(data, curLen, sampleTime);
                buf->Unlock();
            }
        }
        if (outputProvidesSamples_) produced->Release();
    }
    if (out.pEvents) out.pEvents->Release();
}

void Encoder::DeliverBitstream(const uint8_t* data, size_t size, int64_t sampleTime100ns) {
    if (!data || size == 0 || !callback_) return;

    const uint8_t* au = data;
    size_t auSize = size;

    // MF H.264 encoders emit Annex-B, but guard against a length-prefixed MFT.
    if (!LooksLikeAnnexB(data, size)) {
        if (AvccToAnnexB(data, size, &scratch_)) {
            au = scratch_.data();
            auSize = scratch_.size();
            static bool warned = false;
            if (!warned) {
                warned = true;
                LOGW("encoder produced length-prefixed output; converting to Annex-B");
            }
        } else {
            LOGW("encoder output is neither Annex-B nor AVCC (%zu bytes), dropping", size);
            return;
        }
    }

    const AnnexBScan scan = ScanAnnexB(au, auSize, &sps_, &pps_);
    if (!scan.valid) return;

    EncodedFrame frame;
    frame.isIdr = scan.hasIdr;
    frame.encodeDoneQpc = QpcNow();

    {
        std::lock_guard<std::mutex> lock(captureMapMutex_);
        auto it = captureTimes_.find(sampleTime100ns);
        if (it != captureTimes_.end()) {
            frame.captureQpc = it->second;
            captureTimes_.erase(it);
        } else {
            frame.captureQpc = frame.encodeDoneQpc;
        }
    }

    const bool haveParamSets = !sps_.empty() && !pps_.empty();
    if (frame.isIdr) {
        frame.flags = kFlagIdr;
        if (scan.hasSps && scan.hasPps) {
            // Encoder already inlined the parameter sets - do not duplicate.
            frame.flags |= kFlagSpsPps;
            frame.payload.assign(au, au + auSize);
        } else if (haveParamSets) {
            frame.flags |= kFlagSpsPps;
            frame.payload.reserve(sps_.size() + pps_.size() + auSize);
            frame.payload.insert(frame.payload.end(), sps_.begin(), sps_.end());
            frame.payload.insert(frame.payload.end(), pps_.begin(), pps_.end());
            frame.payload.insert(frame.payload.end(), au, au + auSize);
        } else {
            LOGW("IDR without cached SPS/PPS - receiver may not be able to start");
            frame.payload.assign(au, au + auSize);
        }
    } else {
        frame.payload.assign(au, au + auSize);
    }

    if (frame.payload.size() > kMaxPayload) {
        LOGW("access unit %zu bytes exceeds protocol max, dropping", frame.payload.size());
        return;
    }
    callback_(std::move(frame));
}

void Encoder::ForceKeyFrame() {
    SetCodecUI32(codecApi_.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1, "AVEncVideoForceKeyFrame");
}

void Encoder::Shutdown() {
    if (!running_.exchange(false)) {
        transform_.Reset();
        codecApi_.Reset();
        eventGen_.Reset();
        deviceManager_.Reset();
        device_.Reset();
        return;
    }

    callback_ = nullptr;

    if (transform_) {
        transform_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        if (isHardware_) {
            transform_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
        }
    }

    // Give any in-flight Invoke() a moment to observe running_ == false.
    Sleep(50);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.Reset();
    }

    eventGen_.Reset();
    codecApi_.Reset();
    transform_.Reset();
    deviceManager_.Reset();
    device_.Reset();
}

}  // namespace a2m
