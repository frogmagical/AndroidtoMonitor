#include "cursor.h"

#include <cstring>

namespace a2m {
namespace {

inline void SetPixel(std::vector<uint8_t>& buf, int width, int x, int y, uint8_t b, uint8_t g,
                     uint8_t r, uint8_t a) {
    uint8_t* p = buf.data() + (static_cast<size_t>(y) * width + x) * 4;
    p[0] = b;
    p[1] = g;
    p[2] = r;
    p[3] = a;
}

// 1bpp mask bit, MSB first within each byte.
inline bool MaskBit(const uint8_t* row, int x) { return ((row[x / 8] >> (7 - (x % 8))) & 1) != 0; }

bool DecodeMonochrome(const DXGI_OUTDUPL_POINTER_SHAPE_INFO& info, const uint8_t* data,
                      size_t dataSize, DecodedCursor* out) {
    // Monochrome shapes stack an AND mask on top of an XOR mask, so the reported
    // height is twice the visible height.
    const int width = static_cast<int>(info.Width);
    const int height = static_cast<int>(info.Height) / 2;
    const size_t pitch = info.Pitch;
    if (width <= 0 || height <= 0 || pitch == 0) return false;
    if (dataSize < pitch * static_cast<size_t>(height) * 2) return false;

    out->width = width;
    out->height = height;
    out->colorBgra.assign(static_cast<size_t>(width) * height * 4, 0);
    out->invertBgra.assign(static_cast<size_t>(width) * height * 4, 0);

    for (int y = 0; y < height; ++y) {
        const uint8_t* andRow = data + pitch * static_cast<size_t>(y);
        const uint8_t* xorRow = data + pitch * static_cast<size_t>(height + y);
        for (int x = 0; x < width; ++x) {
            const bool a = MaskBit(andRow, x);
            const bool b = MaskBit(xorRow, x);
            if (!a && !b) {
                SetPixel(out->colorBgra, width, x, y, 0, 0, 0, 255);  // opaque black
                out->hasColor = true;
            } else if (!a && b) {
                SetPixel(out->colorBgra, width, x, y, 255, 255, 255, 255);  // opaque white
                out->hasColor = true;
            } else if (a && b) {
                // AND=1, XOR=1 -> invert the destination (classic I-beam).
                SetPixel(out->invertBgra, width, x, y, 255, 255, 255, 255);
                out->hasInvert = true;
            }
            // a && !b -> fully transparent, leave both layers at zero
        }
    }
    return true;
}

bool DecodeColor(const DXGI_OUTDUPL_POINTER_SHAPE_INFO& info, const uint8_t* data, size_t dataSize,
                 DecodedCursor* out) {
    const int width = static_cast<int>(info.Width);
    const int height = static_cast<int>(info.Height);
    const size_t pitch = info.Pitch;
    if (width <= 0 || height <= 0 || pitch < static_cast<size_t>(width) * 4) return false;
    if (dataSize < pitch * static_cast<size_t>(height)) return false;

    out->width = width;
    out->height = height;
    out->colorBgra.assign(static_cast<size_t>(width) * height * 4, 0);
    out->invertBgra.clear();
    out->hasColor = true;
    out->hasInvert = false;

    for (int y = 0; y < height; ++y) {
        std::memcpy(out->colorBgra.data() + static_cast<size_t>(y) * width * 4,
                    data + pitch * static_cast<size_t>(y), static_cast<size_t>(width) * 4);
    }
    return true;
}

bool DecodeMaskedColor(const DXGI_OUTDUPL_POINTER_SHAPE_INFO& info, const uint8_t* data,
                       size_t dataSize, DecodedCursor* out) {
    const int width = static_cast<int>(info.Width);
    const int height = static_cast<int>(info.Height);
    const size_t pitch = info.Pitch;
    if (width <= 0 || height <= 0 || pitch < static_cast<size_t>(width) * 4) return false;
    if (dataSize < pitch * static_cast<size_t>(height)) return false;

    out->width = width;
    out->height = height;
    out->colorBgra.assign(static_cast<size_t>(width) * height * 4, 0);
    out->invertBgra.assign(static_cast<size_t>(width) * height * 4, 0);

    for (int y = 0; y < height; ++y) {
        const uint8_t* row = data + pitch * static_cast<size_t>(y);
        for (int x = 0; x < width; ++x) {
            const uint8_t* px = row + static_cast<size_t>(x) * 4;
            const uint8_t mask = px[3];
            if (mask == 0) {
                // Mask 0 => the colour replaces the screen pixel.
                SetPixel(out->colorBgra, width, x, y, px[0], px[1], px[2], 255);
                out->hasColor = true;
            } else {
                // Mask 0xFF => XOR the colour with the screen. Real cursors only
                // ever use 0x000000 (no-op => transparent) or 0xFFFFFF (invert);
                // any other value is approximated as a plain inversion.
                if (px[0] || px[1] || px[2]) {
                    SetPixel(out->invertBgra, width, x, y, 255, 255, 255, 255);
                    out->hasInvert = true;
                }
            }
        }
    }
    return true;
}

}  // namespace

const char* PointerShapeTypeName(UINT type) {
    switch (type) {
        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME:
            return "MONOCHROME";
        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR:
            return "COLOR";
        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR:
            return "MASKED_COLOR";
        default:
            return "UNKNOWN";
    }
}

bool DecodePointerShape(const DXGI_OUTDUPL_POINTER_SHAPE_INFO& info, const uint8_t* data,
                        size_t dataSize, DecodedCursor* out) {
    if (!data || !out) return false;
    out->hasColor = false;
    out->hasInvert = false;

    switch (info.Type) {
        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME:
            return DecodeMonochrome(info, data, dataSize, out);
        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR:
            return DecodeColor(info, data, dataSize, out);
        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR:
            return DecodeMaskedColor(info, data, dataSize, out);
        default:
            return false;
    }
}

}  // namespace a2m
