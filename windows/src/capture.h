// DXGI Desktop Duplication capture of a single output + GPU BGRA->NV12 conversion.
//
// The virtual display is located by scanning every adapter/output for one whose
// desktop rectangle matches the requested size (REQUIREMENTS §2: 1080x2400 portrait).
// Everything stays on the GPU: the duplicated BGRA texture is copied into a
// persistent texture and converted with ID3D11VideoProcessor into a pooled NV12
// texture that is handed straight to the Media Foundation encoder (REQUIREMENTS
// §4.2 "GPU 上でのコピー回数を最小化").
#pragma once

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <string>
#include <vector>

namespace a2m {

struct OutputInfo {
    std::string adapterName;
    std::string deviceName;
    int width = 0;
    int height = 0;
    int adapterIndex = 0;
    int outputIndex = 0;
    UINT rotation = 0;
    bool attached = false;
};

class DesktopCapture {
public:
    DesktopCapture() = default;
    ~DesktopCapture();

    DesktopCapture(const DesktopCapture&) = delete;
    DesktopCapture& operator=(const DesktopCapture&) = delete;

    // Enumerates every adapter/output pair (used for diagnostics and selection).
    static std::vector<OutputInfo> EnumerateOutputs();

    // Picks the output whose desktop rect is exactly wantW x wantH and brings up
    // D3D11 + duplication + the NV12 conversion pipeline on that adapter.
    bool Initialize(int wantW, int wantH);

    // Waits up to timeoutMs for a new desktop frame. Returns true when the
    // internal "latest frame" texture was refreshed. Handles ACCESS_LOST by
    // rebuilding the duplication object.
    bool PumpFrame(int timeoutMs);

    // True once at least one desktop frame has been captured.
    bool HasFrame() const { return haveFrame_; }

    // Converts the latest captured BGRA frame into the next NV12 texture from the
    // pool and returns it (borrowed pointer, owned by the pool). Returns nullptr
    // on failure or when no frame has been captured yet.
    ID3D11Texture2D* ConvertLatestToNv12();

    ID3D11Device* Device() const { return device_.Get(); }
    int Width() const { return width_; }
    int Height() const { return height_; }
    const OutputInfo& SelectedOutput() const { return selected_; }

private:
    bool CreateDuplication();
    void ReleaseDuplication();
    bool EnsureVideoProcessor();
    bool EnsureNv12Pool();

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter_;
    Microsoft::WRL::ComPtr<IDXGIOutput1> output_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> dupl_;

    Microsoft::WRL::ComPtr<ID3D11VideoDevice> videoDevice_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> videoContext_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> vpEnum_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> vp_;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> lastBgra_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> vpInputView_;

    static constexpr int kNv12PoolSize = 4;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> nv12Pool_[kNv12PoolSize];
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> nv12Views_[kNv12PoolSize];
    int nv12Next_ = 0;

    OutputInfo selected_;
    int width_ = 0;
    int height_ = 0;
    bool haveFrame_ = false;
    bool frameHeld_ = false;
};

}  // namespace a2m
