// DXGI Desktop Duplication capture of a single output, GPU cursor compositing,
// and GPU BGRA->NV12 conversion.
//
// The virtual display is located by scanning every adapter/output for one whose
// desktop rectangle matches the requested size (REQUIREMENTS §2: 1080x2400 portrait).
// Everything stays on the GPU: the duplicated BGRA texture is copied into a
// persistent texture, the mouse cursor is blended in with a small render pass, and
// the result is converted with ID3D11VideoProcessor into a pooled NV12 texture that
// is handed straight to the Media Foundation encoder (REQUIREMENTS §4.2 "GPU 上での
// コピー回数を最小化").
#pragma once

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <string>
#include <vector>

#include "cursor.h"

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

// What a single PumpFrame call observed. DDA reports desktop updates and pointer
// updates independently: the pointer can move over a completely static screen, and
// that still has to reach the phone (otherwise the cursor appears frozen).
struct PumpResult {
    bool desktopUpdated = false;
    bool pointerUpdated = false;
    bool Any() const { return desktopUpdated || pointerUpdated; }
};

struct CursorStats {
    uint64_t shapeUpdates = 0;
    uint64_t unsupportedShapes = 0;
    // Per DXGI_OUTDUPL_POINTER_SHAPE_TYPE: how many shapes of each format were
    // decoded, so the report can state real coverage instead of assuming it.
    uint64_t monochromeShapes = 0;
    uint64_t colorShapes = 0;
    uint64_t maskedColorShapes = 0;
    uint64_t invertingShapes = 0;  // shapes that contain XOR/invert pixels
    UINT lastShapeType = 0;
    bool visible = false;
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
    // D3D11 + duplication + cursor compositing + NV12 conversion on that adapter.
    bool Initialize(int wantW, int wantH, bool drawCursor);

    // Waits up to timeoutMs for a new desktop frame and/or pointer update.
    // Handles ACCESS_LOST by rebuilding the duplication object.
    PumpResult PumpFrame(int timeoutMs);

    // True once at least one desktop frame has been captured.
    bool HasFrame() const { return haveFrame_; }

    // Composites the cursor (when visible) over the latest desktop frame and
    // converts the result into the next NV12 texture from the pool. Returns a
    // borrowed pointer owned by the pool, or nullptr on failure.
    ID3D11Texture2D* ConvertLatestToNv12();

    ID3D11Device* Device() const { return device_.Get(); }
    int Width() const { return width_; }
    int Height() const { return height_; }
    const OutputInfo& SelectedOutput() const { return selected_; }
    const CursorStats& Cursor() const { return cursorStats_; }

private:
    bool CreateDuplication();
    void ReleaseDuplication();
    bool EnsureVideoProcessor();
    bool EnsureNv12Pool();
    bool EnsureCursorPipeline();
    bool EnsureComposeTarget();

    void UpdatePointerShape(const DXGI_OUTDUPL_FRAME_INFO& info);
    bool UploadCursorTextures(const DecodedCursor& decoded);
    // Returns the texture the video processor should read this frame.
    ID3D11Texture2D* ComposeCursor();
    void DrawCursorLayer(ID3D11ShaderResourceView* srv, ID3D11BlendState* blend);

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter_;
    Microsoft::WRL::ComPtr<IDXGIOutput1> output_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> dupl_;

    Microsoft::WRL::ComPtr<ID3D11VideoDevice> videoDevice_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> videoContext_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> vpEnum_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> vp_;

    // Clean desktop image (never has the cursor drawn into it, so the previous
    // cursor position can always be restored by copying from here).
    Microsoft::WRL::ComPtr<ID3D11Texture2D> lastBgra_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> lastBgraView_;
    // Desktop + cursor, used as the encoder source whenever the cursor is visible.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> composedBgra_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> composedView_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> composedRtv_;

    // Cursor render pass
    bool drawCursor_ = true;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> cb_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendAlpha_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendInvert_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> cursorColorTex_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> cursorColorSrv_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> cursorInvertTex_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> cursorInvertSrv_;

    DecodedCursor cursorShape_;
    bool cursorVisible_ = false;
    int cursorX_ = 0;
    int cursorY_ = 0;
    CursorStats cursorStats_;
    std::vector<uint8_t> shapeBuffer_;

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
