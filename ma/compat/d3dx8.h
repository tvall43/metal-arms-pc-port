// compat/d3dx8.h
//
// The tree was built against the DirectX 8 SDK's d3dx8/d3dx8tex. mingw-w64
// ships D3DX9 instead, but it is not a drop-in replacement here:
//
//   * D3DXLoadSurfaceFromMemory in D3DX9 takes a D3DXIMAGE_FILEFORMAT and
//     decodes an encoded image (DDS/JPG/...). The call sites in fdx8tex.cpp and
//     fdx8sh.cpp pass a D3DFORMAT -- they blit already-decoded pixels of that
//     surface format, not an encoded image. Forwarding to D3DX9 would
//     reinterpret the bytes and produce garbage.
//   * D3DXCreateCubeTexture gained parameters in D3DX9.
//   * d3dx9math.h includes d3dx9.h, which includes d3d9.h. Pulling that in
//     alongside the d3d8.h the renderer needs produces a wall of
//     "redefinition of struct _D3DSURFACE_DESC" and friends.
//
// So the math half is declared here directly. The engine only uses three D3DX
// types (D3DXVECTOR3/4, D3DXMATRIX) and four functions, all of which are
// simple enough to inline; that removes both the d3d9 clash and the runtime
// dependency on d3dx9.dll. The two texture functions are implemented over
// plain d3d8 in d3dx8_compat.cpp.

#pragma once

#include <d3d8.h>
#include <string.h>
#include <math.h>

// ---------------------------------------------------------------------------
// D3DX constants used by the engine
// ---------------------------------------------------------------------------
#ifndef D3DX_FILTER_NONE
#define D3DX_FILTER_NONE       0x1
#endif
#ifndef D3DX_FILTER_POINT
#define D3DX_FILTER_POINT      0x4
#endif
#ifndef D3DX_DEFAULT
#define D3DX_DEFAULT           0xffffffff
#endif

// ---------------------------------------------------------------------------
// D3DX math types (same definitions as the D3DX8/9 SDK)
// ---------------------------------------------------------------------------
struct D3DXVECTOR2 { FLOAT x, y; };
struct D3DXVECTOR3 { FLOAT x, y, z; };
struct D3DXVECTOR4 { FLOAT x, y, z, w; };

// Mirrors d3dx8.h's C++ definition: derives from D3DMATRIX, so _11.._44 and
// m[4][4] come from the base, and a D3DXMATRIX* upcasts to D3DMATRIX*. The
// engine needs all three: it writes _44 on matrices, builds them from 16
// floats, and passes them where a D3DMATRIX is expected.
struct D3DXMATRIX : public D3DMATRIX
{
	D3DXMATRIX() {}
	D3DXMATRIX(const FLOAT *pf) { memcpy(m, pf, sizeof(FLOAT) * 16); }
	D3DXMATRIX(const D3DMATRIX &mat) { memcpy(m, mat.m, sizeof(FLOAT) * 16); }
	D3DXMATRIX(FLOAT f11, FLOAT f12, FLOAT f13, FLOAT f14,
	           FLOAT f21, FLOAT f22, FLOAT f23, FLOAT f24,
	           FLOAT f31, FLOAT f32, FLOAT f33, FLOAT f34,
	           FLOAT f41, FLOAT f42, FLOAT f43, FLOAT f44)
	{
		m[0][0]=f11; m[0][1]=f12; m[0][2]=f13; m[0][3]=f14;
		m[1][0]=f21; m[1][1]=f22; m[1][2]=f23; m[1][3]=f24;
		m[2][0]=f31; m[2][1]=f32; m[2][2]=f33; m[2][3]=f34;
		m[3][0]=f41; m[3][1]=f42; m[3][2]=f43; m[3][3]=f44;
	}
	FLOAT& operator () (UINT row, UINT col) { return m[row][col]; }
	FLOAT  operator () (UINT row, UINT col) const { return m[row][col]; }
};

// ---------------------------------------------------------------------------
// D3DX math functions used by the engine.
//
// Layout and convention match the SDK: m[row][col], row-vector convention, so
// D3DXMatrixMultiply(pOut, pA, pB) computes pA * pB with pOut->m[i][j] =
// sum_k pA->m[i][k] * pB->m[k][j].
// ---------------------------------------------------------------------------

// pOut = pA * pB
static inline void D3DXMatrixMultiply(D3DXMATRIX* pOut, CONST D3DXMATRIX* pA, CONST D3DXMATRIX* pB)
{
    D3DXMATRIX tmp;
    for (int i = 0; i < 4; ++i)
    {
        for (int j = 0; j < 4; ++j)
        {
            FLOAT s = 0.0f;
            for (int k = 0; k < 4; ++k)
                s += pA->m[i][k] * pB->m[k][j];
            tmp.m[i][j] = s;
        }
    }
    *pOut = tmp;
}

// pOut = transpose(pM)
static inline void D3DXMatrixTranspose(D3DXMATRIX* pOut, CONST D3DXMATRIX* pM)
{
    D3DXMATRIX tmp;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            tmp.m[i][j] = pM->m[j][i];
    *pOut = tmp;
}

// pOut = inverse(pM), via Gauss-Jordan with partial pivoting. Returns NULL if
// pM is singular. The SDK's own routine is a cofactor expansion; both compute
// the same mathematical inverse.
static inline D3DXMATRIX* D3DXMatrixInverse(D3DXMATRIX* pOut, CONST D3DXMATRIX* pM, FLOAT* pDeterminant)
{
    FLOAT a[4][8];
    for (int i = 0; i < 4; ++i)
    {
        for (int j = 0; j < 4; ++j)
            a[i][j] = pM->m[i][j];
        for (int j = 0; j < 4; ++j)
            a[i][4 + j] = (i == j) ? 1.0f : 0.0f;
    }

    // determinant via elimination; the pivot product gives det up to sign
    FLOAT det = 1.0f;
    for (int col = 0; col < 4; ++col)
    {
        int piv = col;
        for (int r = col + 1; r < 4; ++r)
            if (fabsf(a[r][col]) > fabsf(a[piv][col]))
                piv = r;
        if (a[piv][col] == 0.0f)
        {
            if (pDeterminant) *pDeterminant = 0.0f;
            return NULL;
        }
        if (piv != col)
        {
            for (int j = 0; j < 8; ++j) { FLOAT t = a[col][j]; a[col][j] = a[piv][j]; a[piv][j] = t; }
            det = -det;
        }
        det *= a[col][col];
        const FLOAT inv = 1.0f / a[col][col];
        for (int j = 0; j < 8; ++j) a[col][j] *= inv;
        for (int r = 0; r < 4; ++r)
        {
            if (r == col) continue;
            const FLOAT f = a[r][col];
            if (f == 0.0f) continue;
            for (int j = 0; j < 8; ++j) a[r][j] -= f * a[col][j];
        }
    }

    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            pOut->m[i][j] = a[i][4 + j];

    if (pDeterminant) *pDeterminant = det;
    return pOut;
}

// pOut = pV / |pV|
static inline D3DXVECTOR3* D3DXVec3Normalize(D3DXVECTOR3* pOut, CONST D3DXVECTOR3* pV)
{
    const FLOAT d = sqrtf(pV->x * pV->x + pV->y * pV->y + pV->z * pV->z);
    if (d == 0.0f)
    {
        *pOut = *pV;
        return NULL;
    }
    const FLOAT inv = 1.0f / d;
    pOut->x = pV->x * inv;
    pOut->y = pV->y * inv;
    pOut->z = pV->z * inv;
    return pOut;
}

// ---------------------------------------------------------------------------
// D3DX8 texture functions, reimplemented over plain d3d8.
// See d3dx8_compat.cpp for the D3DX8 argument order, reconstructed from the
// call sites.
// ---------------------------------------------------------------------------
#ifdef __cplusplus
extern "C" {
#endif

// fdx8tex.cpp:4125  (pSurface, NULL, NULL, pnSrcImage, nSrcD3DFormat,
//                   nBytesPerColumn * nSrcWidth, NULL, &SrcRect,
//                   D3DX_DEFAULT, (D3DCOLOR)0)
// fdx8sh.cpp:2983   (pSurface, NULL, &dstRect, pImageData_L, (D3DFORMAT)nD3DFormat,
//                   nWidth*4, NULL, &srcRect, D3DX_FILTER_POINT, 0)
HRESULT D3DXLoadSurfaceFromMemory(
    LPDIRECT3DSURFACE8     pDestSurface,
    const RECT*           pDestRect,
    const RECT*           pAltDestRect,
    LPCVOID               pSrcData,
    UINT                  SrcFormat,
    UINT                  SrcPitch,
    const PALETTEENTRY*   pPalette,
    const RECT*           pSrcRect,
    DWORD                 Filter,
    DWORD                 dwDstStartMipLevel);

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
