// compat/xgraphics.h
//
// Stand-in for the Xbox SDK header of the same name. The Xbox SDK is long gone
// and the original Windows build linked xgraphics.lib, so this supplies the
// handful of things the engine actually references.
//
// Most of XGraphics has no counterpart on PC and nothing in the tree calls it,
// so this header exists mainly for XGUnswizzleRect (fdx8tex.cpp).
//
// XGUnswizzleRect exists because Xbox textures are stored *swizzled* in memory:
// a Z-order (Morton) interleave of the x and y texel bits that suits the GCN's
// texture cache and cannot be addressed linearly. Fang marks such textures with
// FTEX_FLAG_LOAD_IN_PLACE (0x40), so the archive holds the GPU layout directly
// and the loader has to undo it. PC D3D8 surfaces are stored linearly.
//
// History, because this file was wrong twice:
//   * v1 passed the pixel data through with a plain row-by-row copy, on the
//     theory that the archive was already linear. It is not -- reading a sky
//     texture that way yields diagonal streaks where the moon should be.
//   * v1 also took DestinationPitch literally. fdx8tex.cpp's 32-bit path uses
//     0 to mean "tightly packed" (its 16-bit path passes nWidth * 2), so every
//     row landed on the same address and only the last row survived. That is
//     what made the mine wires invisible: their 8x8 A8R8G8B8 texture came
//     through as one valid row plus seven zero rows, and the cutout row's alpha
//     test then rejected every fragment.
// Both are fixed here: a real Z-order unswizzle, and 0 meaning tight packing.
//
// Note DXT textures do NOT take this path (fdx8tex.cpp copies their blocks
// straight through) because the exporter leaves compressed data linear; only
// the uncompressed 16/32-bit formats are pre-swizzled.

#pragma once

#include <d3d8.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// Spreads the bits of nValue into the bit positions set in nPattern.
// e.g. nPattern = 0b0101, nValue = 0b11 -> 0b0101.
static inline unsigned int _XGCompatFillPattern(unsigned int nPattern, unsigned int nValue)
{
    unsigned int nResult = 0;
    unsigned int nBit = 1;

    while (nValue > 0)
    {
        if (nPattern & nBit)
        {
            if (nValue & 1)
            {
                nResult |= nBit;
            }
            nValue >>= 1;
        }
        nBit <<= 1;
    }

    return nResult;
}

// Xbox SDK shape: copies a rectangle from pSource into pDestination, which has
// DestinationPitch bytes per row. pRect may be NULL for the whole image.
// DestinationPitch 0 means "tightly packed". Returns S_OK.
static inline HRESULT XGUnswizzleRect(const VOID*  pSource,
                                      UINT         Width,
                                      UINT         Height,
                                      const RECT*  pRect,
                                      VOID*        pDestination,
                                      UINT         DestinationPitch,
                                      DWORD        Flags,
                                      DWORD        BytesPerPixel)
{
    (void)Flags;

    if (pSource == NULL || pDestination == NULL || Width == 0 || Height == 0 || BytesPerPixel == 0)
        return E_INVALIDARG;

    const unsigned char *pSrc = (const unsigned char *)pSource;
    unsigned char *pDst = (unsigned char *)pDestination;
    const size_t nDstPitch = (DestinationPitch != 0)
                           ? (size_t)DestinationPitch
                           : (size_t)Width * (size_t)BytesPerPixel;

    if (pRect != NULL)
    {
        // No caller needs a sub-rectangle; keep the old linear behaviour for one
        // rather than pretending to unswizzle a windowed region.
        const LONG w = pRect->right - pRect->left;
        const LONG h = pRect->bottom - pRect->top;
        if (w <= 0 || h <= 0)
            return E_INVALIDARG;

        const unsigned char *pSrcOrigin = pSrc
            + (ptrdiff_t)pRect->top * (ptrdiff_t)Width * (ptrdiff_t)BytesPerPixel
            + (ptrdiff_t)pRect->left * (ptrdiff_t)BytesPerPixel;
        const size_t nRowBytes = (size_t)w * (size_t)BytesPerPixel;

        for (LONG y = 0; y < h; ++y)
        {
            memcpy(pDst + (ptrdiff_t)y * (ptrdiff_t)nDstPitch,
                   pSrcOrigin + (ptrdiff_t)y * (ptrdiff_t)Width * (ptrdiff_t)BytesPerPixel,
                   nRowBytes);
        }
        return S_OK;
    }

    // Build the Z-order masks: bit 0 of x goes to bit 0, bit 0 of y to bit 1,
    // bit 1 of x to bit 2, and so on, until both dimensions are exhausted. (A
    // second dimension of 1 contributes nothing, as the size-tuple walk below
    // shows.) This is the NV2A's swizzle for a 2D surface.
    unsigned int anMask[2] = { 0, 0 };
    {
        unsigned int nTexelBit = 1;
        unsigned int nMaskBit = 1;
        BOOL bDone = FALSE;

        while (!bDone)
        {
            bDone = TRUE;
            if (nTexelBit < Width)  { anMask[0] |= nMaskBit; nMaskBit <<= 1; bDone = FALSE; }
            if (nTexelBit < Height) { anMask[1] |= nMaskBit; nMaskBit <<= 1; bDone = FALSE; }
            nTexelBit <<= 1;
        }
    }

    for (unsigned int y = 0; y < Height; ++y)
    {
        const unsigned int nYPart = _XGCompatFillPattern(anMask[1], y);

        for (unsigned int x = 0; x < Width; ++x)
        {
            const unsigned int nSrcTexel = _XGCompatFillPattern(anMask[0], x) | nYPart;

            memcpy(pDst + (size_t)y * nDstPitch + (size_t)x * (size_t)BytesPerPixel,
                   pSrc + (size_t)nSrcTexel * (size_t)BytesPerPixel,
                   (size_t)BytesPerPixel);
        }
    }

    return S_OK;
}

#ifdef __cplusplus
}
#endif
