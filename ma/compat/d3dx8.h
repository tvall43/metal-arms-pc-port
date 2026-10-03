// compat/d3dx8.h
//
// The tree was built against the DirectX 8 SDK's d3dx8/d3dx8tex. mingw-w64
// ships D3DX9 instead, and the two are *not* drop-in equivalents for the two
// functions actually used here:
//
//   * D3DXLoadSurfaceFromMemory in D3DX9 takes a D3DXIMAGE_FILEFORMAT and
//     decodes an encoded image (DDS/JPG/...). The call sites in fdx8tex.cpp and
//     fdx8sh.cpp pass a D3DFORMAT -- they are blitting raw pixels of that
//     surface format, not decoding anything. Forwarding to the D3DX9 function
//     would reinterpret the bytes and produce garbage.
//   * D3DXCreateCubeTexture gained several parameters in D3DX9.
//
// So both are implemented from scratch over plain d3d8 in d3dx8_compat.cpp.
// The math half needs no shim: D3DXVECTOR2/3/4, D3DXMATRIX, D3DXCOLOR and the
// D3DXMatrix*/D3DXVec* helpers have identical definitions in d3dx9math.h and
// are header-only, so they are reused as-is.

#pragma once

#include <d3d8.h>
#include <d3dx9math.h>

// d3dx9math.h spells the width/height variants D3DXVECTOR2W etc.; the plain
// D3DX8 spellings already exist, so nothing to alias here. Filter constants
// are defined by d3dx9.h, which we do not want to pull in (it declares a large
// surface of functions we deliberately do not implement). Define the three the
// engine uses.
#ifndef D3DX_FILTER_NONE
#define D3DX_FILTER_NONE       0x1
#endif
#ifndef D3DX_FILTER_POINT
#define D3DX_FILTER_POINT      0x4
#endif
#ifndef D3DX_DEFAULT
#define D3DX_DEFAULT           0xffffffff
#endif

#ifdef __cplusplus
extern "C" {
#endif

// D3DX8 argument order, reconstructed from the call sites:
//
//   fdx8tex.cpp:4125  (pSurface, NULL, NULL, pnSrcImage, nSrcD3DFormat,
//                      nBytesPerColumn * nSrcWidth, NULL, &SrcRect,
//                      D3DX_DEFAULT, (D3DCOLOR)0)
//   fdx8sh.cpp:2983   (pSurface, NULL, &dstRect, pImageData_L, (D3DFORMAT)nD3DFormat,
//                      nWidth*4, NULL, &srcRect, D3DX_FILTER_POINT, 0)
//
// Every call site copies an unscaled region: the src rect always spans the
// whole supplied buffer, so Filter is never actually exercised.
HRESULT D3DXLoadSurfaceFromMemory(
    LPDIRECT3DSURFACE8 pDestSurface,
    const RECT*       pDestRect,
    const RECT*       pUnusedRect,
    LPCVOID           pSrcData,
    UINT              SrcFormat,
    UINT              SrcPitch,
    const PALETTEENTRY* pPalette,
    const RECT*       pSrcRect,
    DWORD             Filter,
    DWORD             dwDstStartMipLevel);

// D3DX8 form: (pDevice, Width, MipLevels, Usage, Format, Pool, ppCubeTexture)
HRESULT D3DXCreateCubeTexture(
    LPDIRECT3DDEVICE8       pDevice,
    UINT                    Width,
    UINT                    MipLevels,
    D3DUSAGE               Usage,
    D3DFORMAT               Format,
    D3DPOOL                 Pool,
    LPDIRECT3DCUBETEXTURE8* ppCubeTexture);

#ifdef __cplusplus
}
#endif