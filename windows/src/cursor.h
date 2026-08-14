// DXGI pointer-shape decoding (REQUIREMENTS OQ-3: the cursor is composited on the
// PC side, since DDA never bakes it into the desktop image).
//
// Splits a shape into two BGRA layers because classic Windows cursors are not a
// simple alpha blend: monochrome and masked-colour shapes contain pixels that
// INVERT whatever is underneath so the cursor stays visible on any background.
// Those pixels go in `invertBgra` and are drawn with an inverting blend state;
// everything else goes in `colorBgra` and is drawn with normal alpha blending.
#pragma once

#include <dxgi1_2.h>

#include <cstdint>
#include <cstddef>
#include <vector>

namespace a2m {

struct DecodedCursor {
    int width = 0;
    int height = 0;
    // Both buffers are width*height*4 BGRA. Alpha 0 = "this layer does not draw here".
    std::vector<uint8_t> colorBgra;
    std::vector<uint8_t> invertBgra;
    bool hasColor = false;
    bool hasInvert = false;
};

// Decodes DXGI_OUTDUPL_POINTER_SHAPE_TYPE_{MONOCHROME,COLOR,MASKED_COLOR}.
// Returns false for unknown/!supported types or a malformed buffer.
bool DecodePointerShape(const DXGI_OUTDUPL_POINTER_SHAPE_INFO& info, const uint8_t* data,
                        size_t dataSize, DecodedCursor* out);

const char* PointerShapeTypeName(UINT type);

}  // namespace a2m
