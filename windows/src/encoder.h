// Media Foundation H.264 encoder MFT wrapper (REQUIREMENTS §4.2).
//
// Prefers a hardware MFT (vendor-agnostic MFTEnumEx with MFT_ENUM_FLAG_HARDWARE)
// and falls back to a software encoder with a warning. Input is a D3D11 NV12
// texture - no CPU copy. Output is one Annex-B access unit per frame; SPS/PPS are
// cached and re-prepended to every IDR so the receiver can join mid-stream
// (PROTOCOL.md type=2 / flags bit0|bit1).
#pragma once

#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <icodecapi.h>
#include <codecapi.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace a2m {

// NVENC's MF wrapper accepts CODECAPI_AVEncCommonRateControlMode=CBR but does not
// behave like CBR (M2-REPORT §6): it overshoots hard around IDRs and, worse, pads
// a completely static screen up to the target rate. Peak-constrained VBR keeps the
// same ceiling while letting idle content cost almost nothing.
enum class RateControlMode {
    Cbr,
    PeakConstrainedVbr,
};

const char* RateControlName(RateControlMode mode);

struct EncodedFrame {
    std::vector<uint8_t> payload;  // Annex-B access unit (SPS+PPS prepended on IDR)
    uint16_t flags = 0;            // kFlagIdr / kFlagSpsPps
    bool isIdr = false;
    int64_t captureQpc = 0;    // when the desktop frame was grabbed
    int64_t encodeDoneQpc = 0; // when the encoder handed the bitstream back
};

class Encoder : public IMFAsyncCallback {
public:
    using FrameCallback = std::function<void(EncodedFrame&&)>;

    Encoder() = default;
    ~Encoder();

    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;

    // `device` must have multithread protection enabled.
    bool Initialize(ID3D11Device* device, int width, int height, int fps, uint32_t bitrateBps,
                    RateControlMode rc);
    void SetFrameCallback(FrameCallback cb) { callback_ = std::move(cb); }

    // Hands an NV12 texture to the encoder. Queue depth is 1: if a frame is still
    // waiting to be consumed it is replaced (and counted as a drop) so latency
    // never accumulates in the encoder (REQUIREMENTS §4.2).
    bool SubmitFrame(ID3D11Texture2D* nv12, int64_t sampleTime100ns, int64_t captureQpc);

    // Requests an IDR on the next submitted frame (used after every reconnect).
    void ForceKeyFrame();

    void Shutdown();

    bool IsHardware() const { return isHardware_; }
    const std::string& EncoderName() const { return encoderName_; }
    uint64_t EncoderDrops() const { return encoderDrops_.load(); }

    // IUnknown / IMFAsyncCallback (async hardware MFT event loop)
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;
    STDMETHODIMP GetParameters(DWORD* flags, DWORD* queue) override;
    STDMETHODIMP Invoke(IMFAsyncResult* result) override;

private:
    bool ActivateTransform(int width, int height);
    bool ConfigureCodecApi(bool afterTypes);
    void ApplyRateControl(uint32_t bitrateBps);
    bool ConfigureTypes(int width, int height, int fps, uint32_t bitrateBps);
    void CacheSequenceHeader();

    void TryFeedLocked();
    void PullOutput();
    void DeliverBitstream(const uint8_t* data, size_t size, int64_t sampleTime100ns);
    void DrainSyncOutputs();

    std::atomic<ULONG> refCount_{1};

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> deviceManager_;
    UINT deviceManagerToken_ = 0;

    Microsoft::WRL::ComPtr<IMFTransform> transform_;
    Microsoft::WRL::ComPtr<ICodecAPI> codecApi_;
    Microsoft::WRL::ComPtr<IMFMediaEventGenerator> eventGen_;

    bool isAsync_ = false;
    bool isHardware_ = false;
    bool outputProvidesSamples_ = false;
    DWORD outputBufferSize_ = 0;
    std::string encoderName_;

    int width_ = 0;
    int height_ = 0;
    int fps_ = 30;
    RateControlMode rc_ = RateControlMode::PeakConstrainedVbr;

    std::mutex mutex_;        // guards the input slot
    std::mutex outputMutex_;  // serialises ProcessOutput / bitstream assembly
    Microsoft::WRL::ComPtr<IMFSample> pending_;  // depth-1 input slot
    int needInput_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> encoderDrops_{0};

    // sample time (100ns) -> capture QPC, so capture->encode latency survives the
    // trip through the MFT.
    std::mutex captureMapMutex_;
    std::map<int64_t, int64_t> captureTimes_;

    std::vector<uint8_t> sps_;
    std::vector<uint8_t> pps_;
    std::vector<uint8_t> scratch_;

    FrameCallback callback_;
};

}  // namespace a2m
