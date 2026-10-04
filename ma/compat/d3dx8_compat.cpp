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

// ---------------------------------------------------------------------------
// IDirect3DCubeTexture8 emulation.
//
// D3D8 has no cube textures -- the hardware feature arrived with D3D9. D3DX8
// therefore provided D3DXCreateCubeTexture by keeping six surfaces and handing
// back an object that dispatches to them. The engine uses just four calls:
// GetLevelDesc, LockRect(face,...), UnlockRect(face,...) and Release, but the
// whole interface has to be implemented to satisfy the vtable.
// ---------------------------------------------------------------------------
class CD3DXCubeTexture : public IDirect3DCubeTexture8
{
public:
	CD3DXCubeTexture( LPDIRECT3DSURFACE8 *pFaces, UINT nEdge, D3DFORMAT fmt, D3DPOOL pool )
	{
		memcpy( m_apFaces, pFaces, sizeof(m_apFaces) );
		m_nEdge = nEdge;
		m_fmt  = fmt;
		m_pool = pool;
		m_nLOD = 0;
		m_nRef = 1;
	}
	~CD3DXCubeTexture() {}

	// --- IUnknown ---
	HRESULT STDMETHODCALLTYPE QueryInterface( REFIID riid, void **ppvObject )
	{
		if ( !ppvObject ) return E_POINTER;
		if ( riid == IID_IUnknown || riid == IID_IDirect3DResource8 ||
		     riid == IID_IDirect3DBaseTexture8 || riid == IID_IDirect3DCubeTexture8 )
		{
			*ppvObject = this;
			AddRef();
			return S_OK;
		}
		*ppvObject = NULL;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() { return ++m_nRef; }
	ULONG STDMETHODCALLTYPE Release()
	{
		const ULONG n = --m_nRef;
		if ( n == 0 ) delete this;
		return n;
	}

	// --- IDirect3DResource8 ---
	HRESULT STDMETHODCALLTYPE GetDevice( IDirect3DDevice8 **ppDevice )
	{
		if ( !ppDevice ) return E_POINTER;
		return m_apFaces[0] ? m_apFaces[0]->GetDevice( ppDevice ) : E_FAIL;
	}
	HRESULT STDMETHODCALLTYPE SetPrivateData( REFGUID, const void *, DWORD, DWORD ) { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE GetPrivateData( REFGUID, void *, DWORD * )           { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE FreePrivateData( REFGUID )                            { return E_NOTIMPL; }
	DWORD  STDMETHODCALLTYPE SetPriority( DWORD )      { return 0; }
	DWORD  STDMETHODCALLTYPE GetPriority()              { return 0; }
	void   STDMETHODCALLTYPE PreLoad()                  {}
	D3DRESOURCETYPE STDMETHODCALLTYPE GetType()        { return D3DRTYPE_CUBETEXTURE; }

	// --- IDirect3DBaseTexture8 ---
	DWORD STDMETHODCALLTYPE SetLOD( DWORD LODNew ) { const DWORD o = m_nLOD; m_nLOD = LODNew; return o; }
	DWORD STDMETHODCALLTYPE GetLOD()                { return m_nLOD; }
	DWORD STDMETHODCALLTYPE GetLevelCount()         { return 1; }

	// --- IDirect3DCubeTexture8 ---
	HRESULT STDMETHODCALLTYPE GetLevelDesc( UINT Level, D3DSURFACE_DESC *pDesc )
	{
		if ( !pDesc || Level != 0 || !m_apFaces[0] ) return E_INVALIDARG;
		return m_apFaces[0]->GetDesc( pDesc );
	}
	HRESULT STDMETHODCALLTYPE GetCubeMapSurface( D3DCUBEMAP_FACES FaceType, UINT Level,
	                                            IDirect3DSurface8 **ppCubeMapSurface )
	{
		if ( !ppCubeMapSurface || Level != 0 ) return E_INVALIDARG;
		if ( (unsigned)FaceType >= 6 || !m_apFaces[FaceType] ) return E_INVALIDARG;
		*ppCubeMapSurface = m_apFaces[FaceType];
		m_apFaces[FaceType]->AddRef();
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE LockRect( D3DCUBEMAP_FACES face, UINT level,
	                                    D3DLOCKED_RECT *pRect, const RECT *p, DWORD flags )
	{
		if ( level != 0 || (unsigned)face >= 6 || !m_apFaces[face] ) return E_INVALIDARG;
		return m_apFaces[face]->LockRect( pRect, p, flags );
	}
	HRESULT STDMETHODCALLTYPE UnlockRect( D3DCUBEMAP_FACES face, UINT level )
	{
		if ( level != 0 || (unsigned)face >= 6 || !m_apFaces[face] ) return E_INVALIDARG;
		return m_apFaces[face]->UnlockRect();
	}
	HRESULT STDMETHODCALLTYPE AddDirtyRect( D3DCUBEMAP_FACES face, const RECT * )
	{
		if ( (unsigned)face >= 6 ) return E_INVALIDARG;
		return S_OK;   // managed surfaces never need it
	}

	// --- conveniences the engine may use ---
	HRESULT STDMETHODCALLTYPE GetSurfaceLevel( UINT Level, IDirect3DSurface8 **pp )
	{
		return GetCubeMapSurface( D3DCUBEMAP_FACE_POSITIVE_X, Level, pp );
	}

private:
	IDirect3DSurface8 *m_apFaces[6];
	UINT    m_nEdge;
	DWORD   m_nLOD;
	D3DFORMAT m_fmt;
	D3DPOOL m_pool;
	ULONG   m_nRef;
};

extern "C" HRESULT D3DXCreateCubeTexture(
    LPDIRECT3DDEVICE8       pDevice,
    UINT                    Width,
    UINT                    MipLevels,
    DWORD                   Usage,
    D3DFORMAT               Format,
    D3DPOOL                 Pool,
    LPDIRECT3DCUBETEXTURE8* ppCubeTexture)
{
	if ( !pDevice || !ppCubeTexture || Width == 0 ) return E_INVALIDARG;

	*ppCubeTexture = NULL;

	// D3DX8 only ever supported a single mip level for cube maps here, and the
	// engine asks for one.
	if ( MipLevels != 1 ) return E_INVALIDARG;

	// Managed textures cannot back an image surface, and a cube map is not
	// expressible as one anyway, so keep six image surfaces. Their format is
	// forced to something lockable, which is all the engine's use requires
	// (it writes a normalisation ramp into each face and samples the result).
	D3DSURFACE_DESC desc;
	memset( &desc, 0, sizeof(desc) );
	desc.Format = (Format == D3DFMT_UNKNOWN) ? D3DFMT_A8R8G8B8 : Format;

	IDirect3DSurface8 *apFaces[6] = { NULL, NULL, NULL, NULL, NULL, NULL };
	for ( int i = 0; i < 6; i++ )
	{
		HRESULT hr = pDevice->CreateImageSurface( Width, Width, desc.Format, &apFaces[i] );
		if ( FAILED(hr) )
		{
			for ( int j = 0; j < i; j++ ) { if ( apFaces[j] ) apFaces[j]->Release(); }
			return hr;
		}
	}

	CD3DXCubeTexture *pCube = new CD3DXCubeTexture( apFaces, Width, desc.Format, Pool );
	for ( int i = 0; i < 6; i++ ) { if ( apFaces[i] ) apFaces[i]->Release(); }
	if ( !pCube ) return E_OUTOFMEMORY;

	// Prove every face is usable before handing the object over. The engine
	// writes a ramp into each face via LockRect and trusts the array without
	// re-checking, so an object containing a face that cannot be locked or
	// described faults inside the shim rather than failing cleanly. Better to
	// report failure here: the engine already handles a null cube map, losing
	// per-pixel normalisation is a performance and appearance change only.
	for ( int i = 0; i < 6; i++ )
	{
		D3DSURFACE_DESC d;
		HRESULT hr = pCube->GetLevelDesc( 0, &d );
		D3DLOCKED_RECT lr;
		if ( SUCCEEDED( hr ) ) hr = pCube->LockRect( (D3DCUBEMAP_FACES)i, 0, &lr, NULL, D3DLOCK_READONLY );
		if ( SUCCEEDED( hr ) ) pCube->UnlockRect( (D3DCUBEMAP_FACES)i, 0 );
		if ( FAILED( hr ) )
		{
			pCube->Release();
			return hr;
		}
	}

	*ppCubeTexture = pCube;
	return S_OK;
}
