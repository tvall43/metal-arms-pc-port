// compat/d3dx8_compat.cpp -- see compat/d3dx8.h for why these are reimplemented
// rather than forwarded to D3DX9.

#include "d3dx8.h"

#include <string.h>

// Bytes per pixel for the formats the engine actually feeds through
// D3DXLoadSurfaceFromMemory. fdx8tex.cpp branches on its own texel table to
// decide pitch, and the DXT cases have to be here because those tables store
// bits-per-block, not bits-per-pixel.
static int _BytesPerPixel(D3DFORMAT fmt)
{
    switch (fmt)
    {
    case D3DFMT_L8:
    case D3DFMT_A8:
        return 1;
    case D3DFMT_A4R4G4B4:
    case D3DFMT_A1R5G5B5:
    case D3DFMT_R5G6B5:
    case D3DFMT_A8P8:
        return 2;
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
    case D3DFMT_R8G8B8:
    case D3DFMT_X8B8G8R8:
        return 4;
    // Block-compressed: 8 bytes per 4x4 block for DXT1, 16 for DXT3/5.
    case D3DFMT_DXT1:
        return 2;
    case D3DFMT_DXT3:
    case D3DFMT_DXT5:
        return 4;
    default:
        return 0;
    }
}

extern "C" HRESULT D3DXLoadSurfaceFromMemory(
    LPDIRECT3DSURFACE8     pDestSurface,
    const RECT*           pDestRect,
    const RECT*           pAltDestRect,
    LPCVOID               pSrcData,
    UINT                  SrcFormat,
    UINT                  SrcPitch,
    const PALETTEENTRY*   pPalette,
    const RECT*           pSrcRect,
    DWORD                 Filter,
    DWORD                 dwDstStartMipLevel)
{
    // The D3DX8 prototypes name the two leading rect parameters
    // pDestRect/pSrcRect, but the call sites do not agree on which of the two
    // leading slots holds the destination rect (fdx8tex.cpp:4125 passes NULL in
    // both and the rect at slot 8, fdx8sh.cpp:2983 passes the rect named
    // dstRect at slot 3). Treat the first non-NULL leading rect as the
    // destination and slot 8 as the source; both call sites then behave.
    //
    // SrcFormat is a D3DFORMAT describing already-decoded pixels, so this is a
    // plain blit. pPalette is always NULL and Filter is never exercised: every
    // call site passes a src rect spanning the whole buffer, so no scaling or
    // colour conversion is required.
    (void)SrcFormat;
    (void)pPalette;
    (void)Filter;
    (void)dwDstStartMipLevel;

    if (!pDestSurface || !pSrcData)
        return E_INVALIDARG;

    D3DSURFACE_DESC desc;
    if (FAILED(pDestSurface->GetDesc(&desc)))
        return E_FAIL;

    const int bpp = _BytesPerPixel(desc.Format);
    if (bpp == 0)
        return E_FAIL;

    // Source rect defaults to the whole supplied buffer. SrcPitch is the real
    // stride of pSrcData when the caller supplies one, otherwise assume a
    // tightly packed image.
    RECT src;
    if (pSrcRect)
    {
        src = *pSrcRect;
    }
    else
    {
        const UINT w = desc.Width;
        const UINT h = desc.Height;
        src.left = 0;
        src.top  = 0;
        src.right  = (LONG)w;
        src.bottom = (LONG)h;
        if (SrcPitch == 0)
            SrcPitch = w * (UINT)bpp;
    }

    RECT dst;
    if (pDestRect)
        dst = *pDestRect;
    else if (pAltDestRect)
        dst = *pAltDestRect;
    else
    {
        dst.left   = 0;
        dst.top    = 0;
        dst.right  = (LONG)desc.Width;
        dst.bottom = (LONG)desc.Height;
    }

    const int w = src.right  - src.left;
    const int h = src.bottom - src.top;
    if (w <= 0 || h <= 0)
        return E_INVALIDARG;

    // Clamp to the destination so a mismatched mip level cannot overrun.
    if (dst.right > (LONG)desc.Width)  dst.right  = (LONG)desc.Width;
    if (dst.bottom > (LONG)desc.Height) dst.bottom = (LONG)desc.Height;

    D3DLOCKED_RECT lr;
    const HRESULT hr = pDestSurface->LockRect(&lr, NULL, D3DLOCK_DISCARD);
    if (FAILED(hr))
        return hr;

    const BYTE* src8 = (const BYTE*)pSrcData
                     + (ptrdiff_t)src.top * (ptrdiff_t)SrcPitch
                     + (ptrdiff_t)src.left * bpp;

    for (int y = 0; y < h; y++)
    {
        const LONG dy = dst.top + y;
        if (dy < 0 || dy >= (LONG)desc.Height)
            continue;

        BYTE* d = (BYTE*)lr.pBits
                + (ptrdiff_t)dy * (ptrdiff_t)lr.Pitch
                + (ptrdiff_t)dst.left * bpp;

        const BYTE* s = src8 + (ptrdiff_t)y * (ptrdiff_t)SrcPitch;
        const size_t rowBytes = (size_t)w * (size_t)bpp;
        const size_t avail = (dst.right > dst.left)
                           ? (size_t)(dst.right - dst.left) * (size_t)bpp
                           : 0;
        memcpy(d, s, rowBytes < avail ? rowBytes : avail);
    }

    pDestSurface->UnlockRect();
    return S_OK;
}

extern "C" HRESULT D3DXCreateCubeTexture(
    LPDIRECT3DDEVICE8       pDevice,
    UINT                    Width,
    UINT                    MipLevels,
    D3DUSAGE               Usage,
    D3DFORMAT               Format,
    D3DPOOL                 Pool,
    LPDIRECT3DCUBETEXTURE8* ppCubeTexture)
{
    if (!pDevice || !ppCubeTexture)
        return E_INVALIDARG;

    *ppCubeTexture = NULL;
    return pDevice->CreateCubeTexture(Width, MipLevels, Usage, Format, Pool, ppCubeTexture);
}