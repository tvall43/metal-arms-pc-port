// compat/xgraphics.h
//
// Stand-in for the Xbox SDK header of the same name. The Xbox SDK is long gone
// and the original Windows build linked xgraphics.lib, so this supplies the
// handful of things the engine actually references.
//
// Most of XGraphics has no counterpart on PC and nothing in the tree calls it,
// so this header exists mainly for XGUnswizzleRect (fdx8tex.cpp).
//
// XGUnswizzleRect exists because Xbox textures are stored *swizzled* in memory
// (a tiled layout that suits the GCN's texture cache, and which cannot be
// addressed linearly). PC D3D8 surfaces are stored linearly, so on this
// platform there is nothing to unswizzle and the correct operation is a plain
// row-by-row copy at the requested destination pitch. Implementing that here
// keeps the call sites untouched and behaves correctly for unswizzled input.
#pragma once

#include <d3d8.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// Xbox SDK shape: copies a rectangle from pSource into pDestination, which has
// DestinationPitch bytes per row. pRect may be NULL for the whole image.
// Returns S_OK.
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

    if (pSource == NULL || pDestination == NULL)
        return E_INVALIDARG;

    RECT r;
    if (pRect != NULL)
    {
        r = *pRect;
    }
    else
    {
        r.left = 0;
        r.top = 0;
        r.right = (LONG)Width;
        r.bottom = (LONG)Height;
    }

    const LONG w = r.right - r.left;
    const LONG h = r.bottom - r.top;
    if (w <= 0 || h <= 0)
        return E_INVALIDARG;

    const BYTE* src = (const BYTE*)pSource
                    + (ptrdiff_t)r.top * (ptrdiff_t)Width * (ptrdiff_t)BytesPerPixel
                    + (ptrdiff_t)r.left * (ptrdiff_t)BytesPerPixel;
    BYTE* dst = (BYTE*)pDestination;

    const size_t rowBytes = (size_t)w * (size_t)BytesPerPixel;

    // The Xbox SDK reads DestinationPitch == 0 as "tightly packed" (the rows are
    // contiguous, pixel width * bytes per pixel apart). fdx8tex.cpp's 32-bit path
    // relies on that and passes 0, while its 16-bit path passes nWidth * 2. Taking
    // 0 literally wrote every row to the SAME destination offset, so only the last
    // source row survived and the rest of the destination stayed at its zero fill:
    // a texture whose whole body is transparent black. That is what made the mine
    // wires invisible (their 8x8 A8R8G8B8 texture came through as one valid row
    // plus seven zero rows, and the cutout row's alpha test then rejected every
    // fragment). Treat 0 as tight packing, as the call site expects.
    const size_t dstPitch = ( DestinationPitch != 0 ) ? (size_t)DestinationPitch : rowBytes;

    for (LONG y = 0; y < h; ++y)
        memcpy(dst + (ptrdiff_t)y * (ptrdiff_t)dstPitch,
               src + (ptrdiff_t)y * (ptrdiff_t)Width * (ptrdiff_t)BytesPerPixel,
               rowBytes);

    return S_OK;
}

#ifdef __cplusplus
}
#endif
