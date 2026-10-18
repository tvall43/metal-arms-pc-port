//////////////////////////////////////////////////////////////////////////////////////
// fdx8mesh.cpp - Fang mesh module (DX8 version).
//
// Author: Steve Ranck     
//////////////////////////////////////////////////////////////////////////////////////
// THIS CODE IS PROPRIETARY PROPERTY OF SWINGIN' APE STUDIOS, INC.
// Copyright (c) 2000
//
// The contents of this file may not be disclosed to third
// parties, copied or duplicated in any form, in whole or in part,
// without the prior written permission of Swingin' Ape Studios, Inc.
//////////////////////////////////////////////////////////////////////////////////////
// Modification History:
//
// Date     Who         Description
// -------- ----------  --------------------------------------------------------------
// 10/16/00 Ranck       Created.
//////////////////////////////////////////////////////////////////////////////////////

#include "fang.h"
#include "fresload.h"
#include "flight.h"
#include "frenderer.h"
#include "fshaders.h"
#include "fworld.h"
#include "fshadow.h"
#include "fDataStreaming.h"

// Counts actual GPU draw submissions. Nothing in the engine reports these, so
// there is no way to tell 'the renderer produced no geometry' from 'geometry was
// submitted and came out black' -- the window looks identical either way.

#include "fdx8.h"
#include "fdx8shadow.h"
#include "fdx8mesh.h"
#include "fdx8load.h"
#include "fdx8vb.h"
#include "fdx8sh.h"
#include "fdx8vid.h"
#include "fdx8viewport.h"
#include "fdx8xfm.h"
#include "fdx8tex.h"

#include "fdx8vshader_const.h"



//////////////////////////////////////////////////////////////////////////////////////
// External dependencies:
//////////////////////////////////////////////////////////////////////////////////////

extern f32 FShadow_ShadowDist;
extern f32 FShader_ShadowRecieveDist;


//////////////////////////////////////////////////////////////////////////////////////
// Global variables:
//////////////////////////////////////////////////////////////////////////////////////

u32 FMesh_nLastSurfaceMaterialCRC;


//////////////////////////////////////////////////////////////////////////////////////
// Local defines:
//////////////////////////////////////////////////////////////////////////////////////

#define _D3D_VTX_BLEND_FLAG_COUNT	5
#define _SUPPORTING_PARTS_DRAWING	FALSE

//////////////////////////////////////////////////////////////////////////////////////
// Local classes and structures:
//////////////////////////////////////////////////////////////////////////////////////

typedef struct 
{
	u32 nD3DValue;
	u32 nFangValue;
} _MapTable_t;


static _MapTable_t _aMap_CullDir_D3DCULL[] = 
{
	D3DCULL_CW,						FMESH_CULLDIR_CW,
	D3DCULL_CCW,					FMESH_CULLDIR_CCW,
	D3DCULL_NONE,					FMESH_CULLDIR_NONE,
};


//////////////////////////////////////////////////////////////////////////////////////
// Static variables:
//////////////////////////////////////////////////////////////////////////////////////

static BOOL8 _bModuleInitialized;
static BOOL8 _bWindowCreated;
static BOOL8 _bStream3Active;

static FResLoadReg_t _ResLoadRegistration;

static FMeshCullDir_e _nCurrentCullDir;

static const D3DVERTEXBLENDFLAGS _aD3DVertexBlendFlags[_D3D_VTX_BLEND_FLAG_COUNT] = 
{
	D3DVBF_DISABLE,		// 0 segment matrices
	D3DVBF_DISABLE,		// 1 segment matrix
	D3DVBF_1WEIGHTS,	// 2 segment matrices
	D3DVBF_2WEIGHTS,	// 3 segment matrices
	D3DVBF_3WEIGHTS		// 4 segment matrices
};


//////////////////////////////////////////////////////////////////////////////////////
// Static function prototypes:
//////////////////////////////////////////////////////////////////////////////////////

static BOOL _WindowCreatedCallback( FDX8VidEvent_e nEvent );
static BOOL _ResLoadCreate( FResHandle_t hRes, void *pLoadedBase, u32 nLoadedBytes, cchar *pszResName );
static void _ResLoadDestroy( void *pResMem );
static FINLINE void _SetCullDir_P( FMeshCullDir_e nCullDir );


//////////////////////////////////////////////////////////////////////////////////////
// Implementation:
//////////////////////////////////////////////////////////////////////////////////////

//
//
//
BOOL fdx8mesh_ModuleStartup( void ) 
{
	FASSERT( !_bModuleInitialized );

	_bWindowCreated = FALSE;

	fdx8vid_RegisterWindowCallbackFunction( _WindowCreatedCallback );

	fres_CopyType( _ResLoadRegistration.sResType, FMESH_RESTYPE );
	_ResLoadRegistration.pszFileExtension = "ape";
	_ResLoadRegistration.nMemType = FRESLOAD_MEMTYPE_TEMP;
	_ResLoadRegistration.nAlignment = 16;
	_ResLoadRegistration.pFcnCreate = _ResLoadCreate;
	_ResLoadRegistration.pFcnDestroy = _ResLoadDestroy;

	_nCurrentCullDir = FMESH_CULLDIR_NONE;

	if ( !fresload_RegisterHandler( &_ResLoadRegistration ) ) 
	{
		// Registration failed...
		DEVPRINTF( "fdx8mesh_ModuleStartup(): Could not register resource.\n" );
		return FALSE;
	}

	FMesh_nMaxMeshLights = 6;
	
	_bModuleInitialized = TRUE;

	_bStream3Active = FALSE;

	return TRUE;
}


//
//
//
void fdx8mesh_ModuleShutdown( void ) 
{
	FASSERT( _bModuleInitialized );

	fdx8vid_UnregisterWindowCallbackFunction( _WindowCreatedCallback );

	_bModuleInitialized = FALSE;
}

BOOL _bFlipCullDir=FALSE;


//
//
//
static FINLINE void _SetCullDir_P( FMeshCullDir_e nCullDir )
{
	FASSERT( _bModuleInitialized );
	FASSERT( frenderer_GetActive() == FRENDERER_MESH );
	FASSERT( nCullDir >=0 && nCullDir < FMESH_CULLDIR_COUNT );

//	if ( _nCurrentCullDir != nCullDir ) 
	{
		if (_bFlipCullDir)
		{
			if (nCullDir == FMESH_CULLDIR_CW) { nCullDir = FMESH_CULLDIR_CCW; }
			else if (nCullDir == FMESH_CULLDIR_CCW) { nCullDir = FMESH_CULLDIR_CW; }
		}
		_nCurrentCullDir = nCullDir;
		fdx8_SetRenderState_CULLMODE( _aMap_CullDir_D3DCULL[nCullDir].nD3DValue );
	}
}


//
//
//
void fmesh_SetCullDir( FMeshCullDir_e nCullDir ) 
{
	_SetCullDir_P( nCullDir );
}


//
//
//
FMeshCullDir_e fmesh_GetCullDir( void )
{
	return _nCurrentCullDir;
}


//
//
//
void fmesh_FlipCullDir( BOOL bFlip )
{
	_bFlipCullDir = bFlip;
}


//
//
//
BOOL fmesh_FlushDrawPrep( void )
{
	return TRUE;
}


//
//
//
CFMeshInst::~CFMeshInst( void )
{
	u32 i;
	for ( i = 0; i < m_nColorStreamCount; i++ )
	{
		if ( m_papColorStreams && ((IDirect3DVertexBuffer8 **)m_papColorStreams)[i] ) 
		{
			((IDirect3DVertexBuffer8 **)m_papColorStreams)[i]->Unlock();
			FDX8_SAFE_RELEASE( ((IDirect3DVertexBuffer8 **)m_papColorStreams)[i] );
			((IDirect3DVertexBuffer8 **)m_papColorStreams)[i] = NULL;
		}
	}

	m_nColorStreamCount = 0;
	m_papColorStreams = NULL;
}


//
//
//
void CFMeshInst::SetColorStreams( u32 nStreamCount, ColorStream_t *paStreams ) 
{
	u32 i;

	FASSERT( m_pMesh && m_pMesh->pMeshIS );

	if ( nStreamCount != m_pMesh->pMeshIS->nVBCount )
	{
		DEVPRINTF( "CFMeshInst::SetColorStreams() - Mesh %s : Color stream data assumes %d VB's, mesh data has %d.  Color streams ignored.\n", m_pMesh->szName, nStreamCount, m_pMesh->pMeshIS->nVBCount );
		return;
	}

	FResFrame_t Frame = fres_GetFrame();

	m_papColorStreams = fres_Alloc( sizeof( IDirect3DVertexBuffer8 *) * nStreamCount );
	if ( !m_papColorStreams )
	{
		DEVPRINTF( "CFMeshInst::SetColorStreams() - Mesh %s : Unable to allocate heap memory for color streams.  Color streams ignored.\n", m_pMesh->szName );
		return;
	}

	// Make sure that the vertex radiosity flag is set
	m_nFlags |= FMESHINST_FLAG_VERT_RADIOSITY;

	m_nColorStreamCount = nStreamCount;

	for ( i = 0; i < nStreamCount; i++ )
	{
		// Build a DX vertex buffer
		if ( paStreams[i].nColorCount != m_pMesh->pMeshIS->aVB[i].nVtxCount )
		{
			DEVPRINTF( "CFMeshInst::SetColorStreams() - Mesh %s : Color stream data assumes %d vertices for VB %d, actual mesh has %d.  Color streams ignored.\n", m_pMesh->szName, paStreams[i].nColorCount, i, m_pMesh->pMeshIS->aVB[i].nVtxCount );
			goto EXITWITHOUTSTREAMS;
		}

		if ( paStreams[i].nColorCount == 0 )
		{
			FASSERT( !paStreams[i].paVertexColors );
			((IDirect3DVertexBuffer8 **)m_papColorStreams)[i] = NULL;
		}

		u32 nBytes = sizeof( DWORD ) * paStreams[i].nColorCount;
		HRESULT hResult = FDX8_pDev->CreateVertexBuffer( nBytes, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &((IDirect3DVertexBuffer8 **)m_papColorStreams)[i] );
		if ( hResult != D3D_OK )
		{
			// Failed to create VB...
			DEVPRINTF( "CFMeshInst::SetColorStreams() - Mesh %s : Unable to create DX VB for color streams.  Color streams ignored.\n", m_pMesh->szName );
			goto EXITWITHOUTSTREAMS;
		}

		u8 *pData;
		((IDirect3DVertexBuffer8 **)m_papColorStreams)[i]->Lock( 0, 0, &pData, 0 );
		if ( !pData )
		{
			DEVPRINTF( "CFMeshInst::SetColorStreams() - Mesh %s : Unable to lock DX VB for color streams.  Color streams ignored.\n", m_pMesh->szName );
			goto EXITWITHOUTSTREAMS;
		}
		fang_MemCopy( pData, paStreams[i].paVertexColors, sizeof( DWORD ) * paStreams[i].nColorCount );

		((IDirect3DVertexBuffer8 **)m_papColorStreams)[i]->Unlock();
	}

	return;

EXITWITHOUTSTREAMS:
	// Remove static lighting
	m_nFlags &= ~(FMESHINST_FLAG_NOLIGHT_AMBIENT|FMESHINST_FLAG_NOLIGHT_DYNAMIC|FMESHINST_FLAG_LM|FMESHINST_FLAG_VERT_RADIOSITY);
	
	m_nColorStreamCount = 0;
	m_papColorStreams = NULL;
}


//
//
//
void CFMeshInst::CacheMeshData( u32 nPriority ) 
{
#if FANG_PLATFORM_XB	
	u32 i, ii;

	if ( nPriority < 2 )
	{
		// Bring streaming textures into the cache
		u32 nLightMapCount;
		if ( m_panLightRegisterLMOverride )
		{
			nLightMapCount = m_panLightRegisterLMOverride[0];

			for ( ii = 0; ii < nLightMapCount; ii++ )
			{
				FShTexInst_t *pTexInst = (FShTexInst_t *)m_panLightRegisterLMOverride[1 + (ii * 3)];
				FASSERT( pTexInst );

				if ( pTexInst->TexInst.GetTexDef() && (pTexInst->TexInst.GetTexDef()->TexInfo.nFlags & FTEX_FLAG_STREAMING) )
				{
					FTexData_t *pTexData = pTexInst->TexInst.GetTexDef()->pTexData;
					if ( pTexData->nFlags & FDX8TEXFLAGS_NOT_IN_CACHE )
					{
						u8 *pCachedData = (u8 *)FDS_StreamMgr.AccessData( pTexData->pStreamingHandle );
						if ( pCachedData )
						{
							// Streamed texture just entered the cache, so register it with the texture header
							pTexData->pD3DTexture->BlockUntilNotBusy();
							pTexData->pD3DTexture->Lock = 0;
							pTexData->nFlags &= ~FDX8TEXFLAGS_NOT_IN_CACHE;
							pTexData->pD3DTexture->Register( pCachedData + 128 );
						}
					}
					else
					{
						void *pCachedData = FDS_StreamMgr.AccessData( pTexData->pStreamingHandle );
						if ( !pCachedData )
						{
							// Streamed texture is no longer in the local streaming cache
							pTexData->nFlags |= FDX8TEXFLAGS_NOT_IN_CACHE;
							pTexData->pD3DTexture->Data = NULL;
						}
					}
				}
			}
		}
		else if ( m_nFlags & FMESHINST_FLAG_WORLD_GEO )
		{
			FMeshMaterial_t *pMaterial = m_pMesh->aMtl;
			for ( i = 0; i < m_pMesh->nMaterialCount; i++, pMaterial++ )
			{
				nLightMapCount = pMaterial->pnShLightRegisters[FSHADERS_LIGHT_REG_LMCOUNT];

				for ( ii = 0; ii < nLightMapCount; ii++ )
				{
					FShTexInst_t *pTexInst = (FShTexInst_t *)pMaterial->pnShLightRegisters[FSHADERS_LIGHT_REG_LM + (ii * 3)];
					FASSERT( pTexInst );
					if ( pTexInst->TexInst.GetTexDef() && (pTexInst->TexInst.GetTexDef()->TexInfo.nFlags & FTEX_FLAG_STREAMING) )
					{
						FTexData_t *pTexData = pTexInst->TexInst.GetTexDef()->pTexData;
						if ( pTexData->nFlags & FDX8TEXFLAGS_NOT_IN_CACHE )
						{
							u8 *pCachedData = (u8 *)FDS_StreamMgr.AccessData( pTexData->pStreamingHandle );
							if ( pCachedData )
							{
								// Streamed texture just entered the cache, so register it with the texture header
								pTexData->pD3DTexture->BlockUntilNotBusy();
								pTexData->pD3DTexture->Lock = 0;
								pTexData->nFlags &= ~FDX8TEXFLAGS_NOT_IN_CACHE;
								pTexData->pD3DTexture->Register( pCachedData + 128 );
							}
						}
						else
						{
							void *pCachedData = FDS_StreamMgr.AccessData( pTexData->pStreamingHandle );
							if ( !pCachedData )
							{
								// Streamed texture is no longer in the local streaming cache
								pTexData->nFlags |= FDX8TEXFLAGS_NOT_IN_CACHE;
								pTexData->pD3DTexture->Data = NULL;
							}
						}
					}
				}
			}
		}
	}
#endif // FANG_PLATFORM_XB
}


//
//
//
BOOL CFMeshInst::DrawPrep_P( BOOL bFlushImmediate ) 
{
	if ( m_nFlags & (FMESHINST_FLAG_POSTER_Y|FMESHINST_FLAG_POSTER_X|FMESHINST_FLAG_POSTER_Z) )
	{
		u8 nNumPops = PushXfm();

		// Store the concatenated model matrix and scale for later use
		m_PosMtx.Set( FXfm_pModel->m_MtxF );
		m_fMatrixScale = FXfm_pModel->m_fScaleF;

		CFXfm::PopModel( nNumPops );
	}
	else
	{
		// Store the concatenated model matrix and scale for later use
		m_PosMtx.Set( m_Xfm.m_MtxF );
		m_fMatrixScale = m_Xfm.m_fScaleF;
	}

	return TRUE;
}



//
//
//
// Round-45d: 192 slots silently swallowed all later meshes — grow + reset on overflow.
static void *_s_apTEMPMDKey[1024];
static char _s_acTEMPMDBranch[1024];
static u32 _s_anTEMPMDFrame[1024];

//R49: invisible-class light-pass force gate. The oBASE surface class (elevator
//car godm_nwelv, cables, supports, doors, dead bodies, chip switch) submits
//lightD+surfaced draws with sane states and sane light constants yet renders
//invisible. Forge a decisive split: force this class's LIGHT pass to solid
//magenta (pixel shader off, fixed-function TFACTOR select). Visible = the pass
//rasterizes fine (content math is the bug). Invisible = pixels never land
//(depth/viewport/decl). Name-gated, skipped during shadow renders.
static const char *_s_apTEMPFFName[] =
{
	"godm_nwelv", "godm_cabl", "go_1cable01",
	"godm_dead02", "godm_dead03", "godm_sprt01", "godm_sprt02",
	"godmdoor", "chipswitch", "godm_m1eld",
};

//R49/R50: device-texture identity resolver (defined in fdx8tex.cpp).
extern const char *fdx8tex_TEMPFindDXTex( const void *pDXTex, u32 *pW, u32 *pH, u32 *pFmt, u32 *pFlags );

//R51: stash of the stage-0 texture captured INSIDE the surface force right
//before SetTexture(0,NULL) -- the device readback after the draw used to be
//self-nulled by the force (t0name dev=NULL was our own doing).
static void *_s_pTEMPSurfDXTex = NULL;

//R52: the surface-white force must restore what it bypasses. The engine's
//shader-state cache keeps "last set" values, and our direct device calls
//made it skip re-applying real states to the draws AFTER each gated one
//(R50/R51 rides: elevators/NPCs/other props turned white -- contamination,
//not their light content).
static const DWORD _s_anTEMPTSS[4] = { D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1 };
static DWORD _s_nTEMPSavedPS = 0;
static DWORD _s_anTEMPSavedTSS[4] = { 0, 0, 0, 0 };
static DWORD _s_nTEMPSavedTF = 0;
static LPDIRECT3DBASETEXTURE8 _s_pTEMPSavedTex0 = NULL;
static LPDIRECT3DBASETEXTURE8 _s_pTEMPSavedTex1 = NULL;
//R53: the loud force also overrides raster states.
static DWORD _s_nTEMPSavedCull = 0;
static DWORD _s_nTEMPSavedZfunc = 0;
static DWORD _s_nTEMPSavedZwrite = 0;
static DWORD _s_nTEMPSavedAbEnable = 0;
static DWORD _s_nTEMPSavedSrcBlend = 0;
static DWORD _s_nTEMPSavedDstBlend = 0;
static DWORD _s_nTEMPSavedAtEnable = 0;
static BOOL _s_bTEMPSurfForced = FALSE;

static BOOL _TEMPFF( const char *pName )
{
	u32 i;
	if ( !pName ) { return FALSE; }
	for ( i = 0; i < sizeof(_s_apTEMPFFName) / sizeof(_s_apTEMPFFName[0]); i++ )
	{
		if ( strncmp( pName, _s_apTEMPFFName[i], strlen( _s_apTEMPFFName[i] ) ) == 0 ) { return TRUE; }
	}
	return FALSE;
}


static BOOL _TEMPMDSeen( const void *pKey, char cBranch )
{
	u32 i;
	for ( i = 0; i < 1024; i++ )
	{
		if ( _s_apTEMPMDKey[i] == pKey && _s_acTEMPMDBranch[i] == cBranch )
		{
			// TEMP round-14: expire so we get a time-series, not just first frame ever
			//R50: expiry 600->1800f (600f cadence produced a 226MB log and
			//perceptible drag on the user's machine).
			if ( FVid_nFrameCounter - _s_anTEMPMDFrame[i] < 1800 ) { return TRUE; }
			_s_anTEMPMDFrame[i] = FVid_nFrameCounter;
			return FALSE;
		}
	}
	for ( i = 0; i < 1024; i++ )
	{
		if ( _s_apTEMPMDKey[i] == NULL ) { _s_apTEMPMDKey[i] = pKey; _s_acTEMPMDBranch[i] = cBranch; _s_anTEMPMDFrame[i] = FVid_nFrameCounter; return FALSE; }
	}
	memset( _s_apTEMPMDKey, 0, sizeof(_s_apTEMPMDKey) );
	memset( _s_acTEMPMDBranch, 0, sizeof(_s_acTEMPMDBranch) );
	memset( _s_anTEMPMDFrame, 0, sizeof(_s_anTEMPMDFrame) );
	_s_apTEMPMDKey[0] = pKey;
	_s_acTEMPMDBranch[0] = cBranch;
	_s_anTEMPMDFrame[0] = FVid_nFrameCounter;
	return FALSE;
}

//R55: the name gate can only ever confirm a guess. Two of the user's three
//fixtures (the fence, the platform floor) have never turned magenta under ANY
//surface state force (R53 cull-off, R54 z-ALWAYS) -- and ride 9's log shows
//every mesh the engine SUBMITS is also DRAWN (78/78 frs names appear in the
//mdraw stream, zero addfails), so that geometry is simply not in our gate
//list. Force by GEOMETRY PROXIMITY instead: if the instance's transformed
//bounding sphere reaches one of the pinned spots, force its surface pass in
//that pin's colour and log its name + the ambient the render sort set for it.
//Whatever owns the invisible geometry then shows up, in colour, and in the log.
static const f32 _afTEMPFFPin[3][3] =
{
	{ 133.f, 4.f, -330.f },		// fix 0: platform in front of the elevator
	{ 1544.f, -592.f, 1020.f },	// fix 1: fence
	{ 404.f, 49.f, 248.f },		// fix 2: wires in the next level
};
//magenta / cyan / yellow. Name-gate hits stay WHITE so the old control group
//is still distinguishable from the newly discovered ones.
static const DWORD _anTEMPFFPinCol[3] = { 0xFFFF00FF, 0xFF00FFFF, 0xFFFFFF00 };
#define TEMPFF_PIN_RADIUS_CAP 500.f
//last hit's bounding radius, surfaced in the ffpin log line
static f32 _fTEMPFfPinRadius = 0.f;

//-1 = not near any pin; else the pin index whose spot this mesh's bounds reach.
//Meshes too big to force without painting half the level are skipped, but said
//so once (TEMP fftoobig) -- that line is itself an answer if the invisible
//geometry turns out to live inside a huge world mesh.
static s32 _TEMPFFPin( CFMeshInst *pInst )
{
	u32 i;
	if ( pInst == NULL ) { return -1; }
	for ( i = 0; i < 3; i++ )
	{
		CFSphere spTest;
		f32 dx, dy, dz, fReach;
		pInst->m_Xfm.TransformSphereF( spTest, pInst->m_BoundSphere_MS );
		dx = spTest.m_Pos.x - _afTEMPFFPin[i][0];
		dy = spTest.m_Pos.y - _afTEMPFFPin[i][1];
		dz = spTest.m_Pos.z - _afTEMPFFPin[i][2];
		fReach = spTest.m_fRadius + 12.f;
		if ( dx*dx + dy*dy + dz*dz <= fReach*fReach )
		{
			if ( spTest.m_fRadius > TEMPFF_PIN_RADIUS_CAP )
			{
				if ( !_TEMPMDSeen( (const void *)( (size_t)pInst->m_pMesh->szName ^ (size_t)(i + 0x100) ), 'B' ) )
				{
					DEVPRINTF( "[ma] TEMP fftoobig f%u fix=%u %s radius=%g pos=(%g,%g,%g)\r\n",
						(unsigned)FVid_nFrameCounter, i, pInst->m_pMesh->szName, (float)spTest.m_fRadius,
						(float)spTest.m_Pos.x, (float)spTest.m_Pos.y, (float)spTest.m_Pos.z );
				}
				continue;
			}
			_fTEMPFfPinRadius = spTest.m_fRadius;
			return (s32)i;
		}
	}
	return -1;
}

//R46b: DrawIndexedPrimitive failure probe. All 12 draw sites swallow DIP
//failures behind the retail no-op fang_DevPrintf() -- an invisible mesh whose
//transform/state/shaders all decode healthy could simply be failing its draw
//call. Log every failure (one per (vb,start,tris) key per 600 frames) with the
//VB's vertex count for range validation.
static void _TEMPDipFail( const char *pTag, u32 nMin, u32 nRange, u32 nStart, u32 nTris )
{
	static u32 _s_anKey[16];
	static u32 _s_anAge[16];
	static u32 _s_nTick = 0;
	FDX8VB_t *pVB = fdx8vb_GetSelected();
	u32 i, nKey;
	_s_nTick++;
	nKey = (u32)(size_t)pVB ^ (nStart << 16) ^ nTris;
	for ( i = 0; i < 16; i++ )
	{
		if ( _s_anKey[i] == nKey && (_s_nTick - _s_anAge[i]) < 600 ) return;
	}
	for ( i = 0; i < 16; i++ )
	{
		if ( (_s_nTick - _s_anAge[i]) >= 600 )
		{
			_s_anKey[i] = nKey;
			_s_anAge[i] = _s_nTick;
			DEVPRINTF( "[ma] TEMP dipfail %s min=%d range=%d start=%d tris=%d vbvtx=%d vbinfo=%d fvf=0x%08X vb=%p\n",
				pTag, nMin, nRange, nStart, nTris,
				pVB ? (s32)pVB->nVtxCount : -1,
				pVB ? pVB->nInfoIndex : -1,
				pVB ? (unsigned)pVB->hVertexShader : 0,
				(void *)pVB );
			return;
		}
	}
}

//R47: vertex-content dump for the tracked invisible flat-geometry meshes
//(elevator floor slats/cables/door fence) plus one visible world control.
//State, shaders and the DIP call itself all decode healthy for these meshes,
//so the remaining suspects are the vertex contents (positions/normals/colors/
//UVs) and index winding. Dumps the first-referenced vertices (read-only lock,
//once per mesh per 600f) plus the first two triangles' indices.
static void _TEMPVData( FMesh_t *pMesh, FDX8MeshCluster_t *pCluster, FDX8Mesh_t *pMeshIS, u32 nMatIdx )
{
	static const char *_asWatch[12] = { "godm_slat", "go_1cable01", "godm_cabl", "godm_dor01", "wedmmines01",
		"godm_m1eld", "godm_nwelv", "godmdoor", "chipswitch", "godm_dead02", "godm_dead03", "godm_sprt01" };
	u32 i, nV, nb, nMin;
	FDX8VB_t *pVB;
	const FDX8VB_Info_t *pInfo;
	u8 *pData;
	for ( i = 0; i < 12; i++ )
	{
		if ( strncmp( pMesh->szName, _asWatch[i], strlen(_asWatch[i]) ) == 0 ) { break; }
	}
	if ( i == 12 ) { return; }
	//R51: key folds in the material index so each material of the mesh logs its
	//own vertex content (m1eld mat0 = invisible floor, mat1 = fine deck).
	if ( _TEMPMDSeen( (const void *)( (size_t)pMesh ^ nMatIdx ), 'v' ) ) { return; }
	pVB = fdx8vb_GetSelected();
	if ( pVB == NULL || pVB->nInfoIndex < 0 || pVB->nInfoIndex >= FDX8VB_TYPE_COUNT )
	{
		DEVPRINTF( "[ma] TEMP vdata %s skip info=%d\r\n", pMesh->szName, pVB ? pVB->nInfoIndex : -1 );
		return;
	}
	pInfo = &FDX8VB_InfoTable[ pVB->nInfoIndex ];
	nMin = pCluster->TriList.nVtxIndexMin;
	nV = pVB->nVtxCount;
	nb = pVB->nBytesPerVertex;
	{
		static char _s_szTEMPVData[640];
		int nOff = sprintf( _s_szTEMPVData, "[ma] TEMP vdata %s nvtx=%d stride=%d info=%d fvf=0x%08X nrm=%d col=%d tc=%d min=%d idx=",
			pMesh->szName, pVB->nVtxCount, pVB->nBytesPerVertex, pVB->nInfoIndex,
			(unsigned)pInfo->nFVF, pInfo->nNormalCount, pInfo->nColorCount, pInfo->nTCCount, nMin );
		//First two triangles of the TriList (winding check).
		if ( pMeshIS && pCluster->nIBIndex < 64 )
		{
			IDirect3DIndexBuffer8 *pIB = (IDirect3DIndexBuffer8 *)pMeshIS->apDXIB[ pCluster->nIBIndex ];
			if ( pIB )
			{
				u16 *pIdx = NULL;
				if ( SUCCEEDED( pIB->Lock( pCluster->TriList.nStartVindex * sizeof(u16), 6 * sizeof(u16), (u8 **)&pIdx, D3DLOCK_READONLY ) ) )
				{
					if ( pIdx )
					{
						for ( i = 0; i < 6; i++ ) { nOff += sprintf( _s_szTEMPVData + nOff, "%u%s", pIdx[i], (i==5)?"":"," ); }
					}
					pIB->Unlock();
				}
			}
		}
		pData = (u8 *)fdx8vb_Lock( pVB, 0, nV );
		if ( pData )
		{
			for ( i = 0; i < 3; i++ )
			{
				u32 v = nMin + i;
				u8 *pv;
				f32 *pf, fN0 = 0.f, fN1 = 0.f, fN2 = 0.f, fU = 0.f, fT = 0.f;
				u32 nCol = 0;
				if ( v >= nV ) { break; }
				pv = pData + v * nb;
				pf = (f32 *)( pv + pInfo->nOffsetPos );
				if ( pInfo->nNormalCount ) { f32 *pn = (f32 *)( pv + pInfo->nOffsetNorm ); fN0 = pn[0]; fN1 = pn[1]; fN2 = pn[2]; }
				if ( pInfo->nColorCount ) { nCol = *(u32 *)( pv + pInfo->nOffsetColor ); }
				if ( pInfo->nTCCount ) { f32 *pt = (f32 *)( pv + pInfo->nOffsetTC ); fU = pt[0]; fT = pt[1]; }
				nOff += sprintf( _s_szTEMPVData + nOff, " v%u=(%g,%g,%g) n=(%g,%g,%g) c=0x%08X uv=(%g,%g)",
					i, (float)pf[0], (float)pf[1], (float)pf[2], (float)fN0, (float)fN1, (float)fN2, nCol, (float)fU, (float)fT );
			}
			//R51: brightest color over the first 8 verts of the cluster range.
			//Radiosity check: bright V_COLOR => light content should be visible.
			{
				u32 nMaxCol = 0;
				for ( i = 0; i < 8; i++ )
				{
					u32 v = nMin + i;
					if ( v < nV && pInfo->nColorCount )
					{
						u32 nC = *(u32 *)( pData + v * nb + pInfo->nOffsetColor );
						if ( (nC & 0x00FFFFFF) > (nMaxCol & 0x00FFFFFF) ) { nMaxCol = nC; }
					}
				}
				nOff += sprintf( _s_szTEMPVData + nOff, " cmax=0x%08X", nMaxCol );
			}
			fdx8vb_Unlock( pVB );
		}
		else
		{
			nOff += sprintf( _s_szTEMPVData + nOff, " (lockfail)" );
		}
		DEVPRINTF( "%s\r\n", _s_szTEMPVData );
	}
}


static const CFMtx43A *_TEMPGetPal( u32 nBoneCount, u32 nInstFlags, const CFMtx43A &PosMtx, CFMtx43A *const *ppPalette, const FMeshSeg_t *pSeg )
{
	if ( nBoneCount == 0 ) { return &PosMtx; }
	if ( pSeg == NULL ) { return NULL; }
	if ( pSeg->nBoneMtxCount == 0 ) { return &PosMtx; }
	if ( ppPalette == NULL ) { return NULL; }
	if ( nInstFlags & FMESHINST_FLAG_NOBONES ) { return NULL; }
	return ppPalette[ pSeg->anBoneMtxIndex[0] ];
}

//R54: clip-space probe for the invisible flat-geometry class. R53 proved the
//loud force reaches the surface pass (PS off, cull NONE, z=LESSEQUAL+write,
//magenta TFACTOR) yet some gated meshes (fence/floor) still produce no pixels.
//"No fragments" (clipped/degenerate/empty VB) and "drawn but z-rejected" look
//identical from the outside. This reads the vertex buffer AND index buffer the
//DRAW will actually consume (device state, not the mesh's own segment data),
//transforms the first vertex of the draw by the same CV_WORLDVIEWPROJ rows the
//vertex shader dp4's against, and prints the clip position. w<=0, |x|>w, |y|>w
//or z outside [0,w] => the hardware clips it before any pixel is made. The
//stride/vb/start also let us cross-check the mdraw line's bound stream.
static void _TEMPMClip( const char *pPass, const char *pName, u32 nStartIndex, u32 nTriCount )
{
	IDirect3DVertexBuffer8 *pVB = NULL;
	IDirect3DIndexBuffer8 *pIB = NULL;
	UINT nStride = 0, nBase = 0;
	u16 *pIdx = NULL;
	u8 *pData = NULL;
	float aWvp[16];

	if ( !_TEMPFF( pName ) ) { return; }
	FDX8_pDev->GetStreamSource( 0, &pVB, &nStride );
	FDX8_pDev->GetIndices( &pIB, &nBase );
	if ( pVB && pIB && nStride >= 12 && (nStride & 3) == 0
		&& !_TEMPMDSeen( (const void *)( (size_t)pVB ^ nStartIndex ^ (size_t)pPass[0] ), 'c' ) )
	{
		memset( aWvp, 0, sizeof(aWvp) );
		FDX8_pDev->GetVertexShaderConstant( CV_WORLDVIEWPROJ_0, aWvp, 4 );
		if ( SUCCEEDED( pIB->Lock( nStartIndex * sizeof(u16), sizeof(u16), (u8 **)&pIdx, D3DLOCK_READONLY ) ) )
		{
			u32 nIdx = (u32)(*pIdx) + nBase;
			pIB->Unlock();
			if ( SUCCEEDED( pVB->Lock( nIdx * nStride, nStride, &pData, D3DLOCK_READONLY ) ) )
			{
				const float *pP = (const float *)pData;
				float x = pP[0], y = pP[1], z = pP[2];
				float cx = x*aWvp[0] + y*aWvp[1] + z*aWvp[2]  + aWvp[3];
				float cy = x*aWvp[4] + y*aWvp[5] + z*aWvp[6]  + aWvp[7];
				float cz = x*aWvp[8] + y*aWvp[9] + z*aWvp[10] + aWvp[11];
				float cw = x*aWvp[12]+ y*aWvp[13]+ z*aWvp[14] + aWvp[15];
				DEVPRINTF( "[ma] TEMP mclip f%u %s %s start=%u tris=%u stride=%u base=%u idx=%u p=(%g,%g,%g) clip=(%g,%g,%g,%g) ndc=(%g,%g) in=%d\r\n",
					(unsigned)FVid_nFrameCounter, pPass, pName, nStartIndex, nTriCount, nStride, nBase, nIdx,
					(float)x, (float)y, (float)z, (float)cx, (float)cy, (float)cz, (float)cw,
					(cw != 0.f) ? cx/cw : 0.f, (cw != 0.f) ? cy/cw : 0.f,
					(cw > 0.000001f && cx >= -cw && cx <= cw && cy >= -cw && cy <= cw && cz >= 0.f && cz <= cw) ? 1 : 0 );
				pVB->Unlock();
			}
		}
	}
	if ( pVB ) { pVB->Release(); }
	if ( pIB ) { pIB->Release(); }
}


static void _TEMPMDLog( const char *pPass, char cBranch, const void *pKey, const char *pName,
	const void *pInst, u32 nClusters, u32 nPartSkip, u32 nLodSkip, u32 nDrawn, u32 nSeg0, u32 nSeg1, u32 nSegSkin,
	u32 nBoneCount, u32 nPartsMask, u32 nLODIndex, f32 fScale, f32 fM0, f32 fM1, f32 fM2,
	int nVol, u32 nInstFlags, const CFMtx43A *pPal, s32 nMatIdx )
{
	DWORD dwShader = 0xFFFFFFFF;
	DWORD dwBlend = 0xFFFFFFFF;
	DWORD dwTEMPps = 0xffffffff, dwTEMPcull = 0xffffffff, dwTEMPzfunc = 0xffffffff, dwTEMPzwrite = 0xffffffff;
	DWORD dwTEMPab = 0xffffffff, dwTEMPsrc = 0xffffffff, dwTEMPdst = 0xffffffff, dwTEMPat = 0xffffffff;
	DWORD dwTEMPafunc = 0xffffffff, dwTEMParef = 0xffffffff;
	DWORD dwTEMPclip = 0xffffffff, dwTEMPclippe = 0xffffffff;
	IDirect3DBaseTexture8 *pTEMPt0 = NULL, *pTEMPt1 = NULL;
	IDirect3DVertexBuffer8 *pTEMPvb = NULL;
	IDirect3DIndexBuffer8 *pTEMPib = NULL;
	UINT nTEMPstride = 0;
	D3DMATRIX Wm;
	//R49: per-material dedupe key so every material of a mesh samples the
	//pass (the old key collapsed multiple materials into one 600f sample).
	if ( _TEMPMDSeen( (const void *)( (u32)(UINT_PTR)pKey ^ (u32)nMatIdx ), cBranch ) ) { return; }

	//R50: fixture-coords lockon -- the user pinned three fixtures with the
	//debug cam: platform missing its floor in front of the elevator
	//(133,4,-330), a fence (1544,-592,1020), and the wires in the next
	//level (404,49,248). Identify the mesh/material at each pin so the
	//next gate list is grounded in data, not guesses.
	{
		static const f32 _aTEMPFx[3][3] = { {133.f, 4.f, -330.f}, {1544.f, -592.f, 1020.f}, {404.f, 49.f, 248.f} };
		u32 nTEMPf;
		f32 fTEMPdx, fTEMPdy, fTEMPdz;
		for ( nTEMPf = 0; nTEMPf < 3; nTEMPf++ )
		{
			fTEMPdx = fM0 - _aTEMPFx[nTEMPf][0];
			fTEMPdy = fM1 - _aTEMPFx[nTEMPf][1];
			fTEMPdz = fM2 - _aTEMPFx[nTEMPf][2];
			if ( fTEMPdx*fTEMPdx + fTEMPdy*fTEMPdy + fTEMPdz*fTEMPdz < 14400.f ) //R51: 5->120 units (mesh pivots sit far from visual fixtures)
			{
				DEVPRINTF( "[ma] TEMP lockon f%u fix=%u %s pos=(%g,%g,%g) pass=%s mat=%d\r\n",
					(unsigned)FVid_nFrameCounter, nTEMPf, pName,
					(float)fM0, (float)fM1, (float)fM2, pPass, nMatIdx );
				break;
			}
		}
	}
	FDX8_pDev->GetVertexShader( &dwShader );
	FDX8_pDev->GetRenderState( D3DRS_VERTEXBLEND, &dwBlend );
	FDX8_pDev->GetPixelShader( &dwTEMPps );
	FDX8_pDev->GetRenderState( D3DRS_CULLMODE, &dwTEMPcull );
	FDX8_pDev->GetRenderState( D3DRS_ZFUNC, &dwTEMPzfunc );
	FDX8_pDev->GetRenderState( D3DRS_ZWRITEENABLE, &dwTEMPzwrite );
	FDX8_pDev->GetRenderState( D3DRS_ALPHABLENDENABLE, &dwTEMPab );
	FDX8_pDev->GetRenderState( D3DRS_SRCBLEND, &dwTEMPsrc );
	FDX8_pDev->GetRenderState( D3DRS_DESTBLEND, &dwTEMPdst );
	FDX8_pDev->GetRenderState( D3DRS_ALPHATESTENABLE, &dwTEMPat );
	FDX8_pDev->GetRenderState( D3DRS_ALPHAFUNC, &dwTEMPafunc );
	FDX8_pDev->GetRenderState( D3DRS_ALPHAREF, &dwTEMParef );
	FDX8_pDev->GetRenderState( D3DRS_CLIPPING, &dwTEMPclip );
	FDX8_pDev->GetRenderState( D3DRS_CLIPPLANEENABLE, &dwTEMPclippe );
	FDX8_pDev->GetTexture( 0, &pTEMPt0 );
	if ( pTEMPt0 ) { pTEMPt0->Release(); }
	FDX8_pDev->GetTexture( 1, &pTEMPt1 );
	if ( pTEMPt1 ) { pTEMPt1->Release(); }
	FDX8_pDev->GetStreamSource( 0, &pTEMPvb, &nTEMPstride );
	if ( pTEMPvb ) { pTEMPvb->Release(); }
	UINT nTEMPbase = 0;
	FDX8_pDev->GetIndices( &pTEMPib, &nTEMPbase );
	if ( pTEMPib ) { pTEMPib->Release(); }
	memset( &Wm, 0, sizeof(Wm) );
	FDX8_pDev->GetTransform( D3DTS_WORLDMATRIX(0), &Wm );
	float aCvw[16];
	memset( aCvw, 0, sizeof(aCvw) );
	FDX8_pDev->GetVertexShaderConstant( CV_WORLD_0, aCvw, 4 );
	//R48: read back the dynamic-light VS constants this draw will actually
	//see (dir light in slot 14/22, point lights 1-2 in 15/16 and 23/24 --
	//_BuildD3DLight writes CV_LIGHT2_*+pointCount for the first point).
	float aLDir[4], aLDif[4], aLP1[4], aLP1c[4], aLP2[4], aLP2c[4];
	memset( aLDir, 0, sizeof(aLDir) );
	memset( aLDif, 0, sizeof(aLDif) );
	memset( aLP1, 0, sizeof(aLP1) );
	memset( aLP1c, 0, sizeof(aLP1c) );
	memset( aLP2, 0, sizeof(aLP2) );
	memset( aLP2c, 0, sizeof(aLP2c) );
	FDX8_pDev->GetVertexShaderConstant( CV_LIGHT1_DIRECTION, aLDir, 1 );
	FDX8_pDev->GetVertexShaderConstant( CV_LIGHT2_POSITION, aLP1, 1 );
	FDX8_pDev->GetVertexShaderConstant( CV_LIGHT3_POSITION, aLP2, 1 );
	FDX8_pDev->GetVertexShaderConstant( CV_LIGHT1_DIFFUSE, aLDif, 1 );
	FDX8_pDev->GetVertexShaderConstant( CV_LIGHT2_DIFFUSE, aLP1c, 1 );
	FDX8_pDev->GetVertexShaderConstant( CV_LIGHT3_DIFFUSE, aLP2c, 1 );
	f32 fP0 = 0.f, fP1 = 0.f, fP2 = 0.f;
	if ( pPal ) { fP0 = pPal->m_vP.x; fP1 = pPal->m_vP.y; fP2 = pPal->m_vP.z; }
	int nNan = ( fM0 != fM0 || fM1 != fM1 || fM2 != fM2 || Wm._41 != Wm._41 || Wm._44 != Wm._44
		|| fP0 != fP0 || fP1 != fP1 ) ? 1 : 0;
	for ( int nTEMPi = 0; nTEMPi < 16; nTEMPi++ ) { if ( aCvw[nTEMPi] != aCvw[nTEMPi] ) { nNan = 1; } }
	DEVPRINTF( "[ma] TEMP mdraw f%u %s%c %s inst=%p clu=%u partskip=%u lodskip=%u drawn=%u seg01sk=%u/%u/%u bone=%u parts=%08x lod=%u scale=%g pos=(%g,%g,%g) pal=(%g,%g,%g)%s cvw=(%g,%g,%g,%g) vw=(%g,%g,%g,%g,%g,%g,%g,%g,%g,%g,%g,%g) pj=(%g,%g,%g,%g,%g,%g) sh=0x%08X blend=%u wm=(%g,%g,%g,%g) vol=%d iflags=%x nan=%d ps=%u cull=%u z=%u zw=%u ab=%u s=%u d=%u at=%u af=%u aref=%u t0=%p clip=%u clippe=%u t1=%p stride=%u vb=%p ib=%p cvw12=(%g,%g,%g,%g,%g,%g,%g,%g,%g,%g,%g,%g) mat=%d\r\n",
		(unsigned)FVid_nFrameCounter, pPass, cBranch, pName, pInst, nClusters, nPartSkip, nLodSkip, nDrawn, nSeg0, nSeg1, nSegSkin,
		nBoneCount, nPartsMask, nLODIndex, fScale, fM0, fM1, fM2,
		fP0, fP1, fP2, pPal ? "" : "(none)",
		(float)aCvw[0], (float)aCvw[1], (float)aCvw[2], (float)aCvw[3],
		(float)FDX8_CurrentViewMtx._11, (float)FDX8_CurrentViewMtx._12, (float)FDX8_CurrentViewMtx._13,
		(float)FDX8_CurrentViewMtx._21, (float)FDX8_CurrentViewMtx._22, (float)FDX8_CurrentViewMtx._23,
		(float)FDX8_CurrentViewMtx._31, (float)FDX8_CurrentViewMtx._32, (float)FDX8_CurrentViewMtx._33,
		(float)FDX8_CurrentViewMtx._41, (float)FDX8_CurrentViewMtx._42, (float)FDX8_CurrentViewMtx._43,
		(float)FDX8_CurrentProjMtx._11, (float)FDX8_CurrentProjMtx._22, (float)FDX8_CurrentProjMtx._33,
		(float)FDX8_CurrentProjMtx._14, (float)FDX8_CurrentProjMtx._34, (float)FDX8_CurrentProjMtx._44,
		(unsigned)dwShader, (unsigned)dwBlend, (float)Wm._41, (float)Wm._42, (float)Wm._43, (float)Wm._44,
		nVol, nInstFlags, nNan,
		(unsigned)dwTEMPps, (unsigned)dwTEMPcull, (unsigned)dwTEMPzfunc, (unsigned)dwTEMPzwrite,
		(unsigned)dwTEMPab, (unsigned)dwTEMPsrc, (unsigned)dwTEMPdst, (unsigned)dwTEMPat,
		(unsigned)dwTEMPafunc, (unsigned)dwTEMParef, (void *)pTEMPt0,
		(unsigned)dwTEMPclip, (unsigned)dwTEMPclippe, (void *)pTEMPt1, nTEMPstride,
		(void *)pTEMPvb, (void *)pTEMPib,
		(float)aCvw[4], (float)aCvw[5], (float)aCvw[6], (float)aCvw[7],
		(float)aCvw[8], (float)aCvw[9], (float)aCvw[10], (float)aCvw[11],
		(float)aCvw[12], (float)aCvw[13], (float)aCvw[14], (float)aCvw[15], nMatIdx );
	DEVPRINTF( "[ma] TEMP mlight f%u %s%c %s ldir=(%g,%g,%g) ldif=(%g,%g,%g) lp1=(%g,%g,%g) lp1c=(%g,%g,%g) lp2=(%g,%g,%g) lp2c=(%g,%g,%g)\r\n",
		(unsigned)FVid_nFrameCounter, pPass, cBranch, pName,
		(float)aLDir[0], (float)aLDir[1], (float)aLDir[2],
		(float)aLDif[0], (float)aLDif[1], (float)aLDif[2],
		(float)aLP1[0], (float)aLP1[1], (float)aLP1[2],
		(float)aLP1c[0], (float)aLP1c[1], (float)aLP1c[2],
		(float)aLP2[0], (float)aLP2[1], (float)aLP2[2],
		(float)aLP2c[0], (float)aLP2c[1], (float)aLP2c[2] );

	//R49: complete the constant picture for the class the review flagged.
	//All six point slots (position c15..c20, diffuse c23..c28, attenuation
	//c38..c43) plus the material constants the VS lighting math multiplies by.
	//Gated to the invisible-class names, light 'D' branch only.
	if ( _TEMPFF( pName ) && cBranch == 'D' )
	{
		float aTEMPmp[24], aTEMPmc[24], aTEMPma[24];
		float aTEMPamb[4], aTEMPmatd[4], aTEMPmatem[4], aTEMPfac[4];
		memset( aTEMPmp, 0, sizeof(aTEMPmp) );
		memset( aTEMPmc, 0, sizeof(aTEMPmc) );
		memset( aTEMPma, 0, sizeof(aTEMPma) );
		memset( aTEMPamb, 0, sizeof(aTEMPamb) );
		memset( aTEMPmatd, 0, sizeof(aTEMPmatd) );
		memset( aTEMPmatem, 0, sizeof(aTEMPmatem) );
		memset( aTEMPfac, 0, sizeof(aTEMPfac) );
		FDX8_pDev->GetVertexShaderConstant( CV_LIGHT2_POSITION, aTEMPmp, 6 );
		FDX8_pDev->GetVertexShaderConstant( CV_LIGHT2_DIFFUSE, aTEMPmc, 6 );
		FDX8_pDev->GetVertexShaderConstant( CV_LIGHT2_ATTENUATION, aTEMPma, 6 );
		FDX8_pDev->GetVertexShaderConstant( CV_MAT_AMBIENT, aTEMPamb, 1 );
		FDX8_pDev->GetVertexShaderConstant( CV_MAT_DIFFUSE, aTEMPmatd, 1 );
		FDX8_pDev->GetVertexShaderConstant( CV_MAT_EMISSIVE, aTEMPmatem, 1 );
		FDX8_pDev->GetVertexShaderConstant( CV_FACTOR, aTEMPfac, 1 );
		DEVPRINTF( "[ma] TEMP mlit f%u %s amb=(%g,%g,%g,%g) mdif=(%g,%g,%g,%g) mem=(%g,%g,%g,%g) fac=(%g,%g,%g,%g)\r\n",
			(unsigned)FVid_nFrameCounter, pName,
			(float)aTEMPamb[0], (float)aTEMPamb[1], (float)aTEMPamb[2], (float)aTEMPamb[3],
			(float)aTEMPmatd[0], (float)aTEMPmatd[1], (float)aTEMPmatd[2], (float)aTEMPmatd[3],
			(float)aTEMPmatem[0], (float)aTEMPmatem[1], (float)aTEMPmatem[2], (float)aTEMPmatem[3],
			(float)aTEMPfac[0], (float)aTEMPfac[1], (float)aTEMPfac[2], (float)aTEMPfac[3] );
		DEVPRINTF( "[ma] TEMP mlitP f%u %s p0=(%g,%g,%g) p1=(%g,%g,%g) p2=(%g,%g,%g) p3=(%g,%g,%g) p4=(%g,%g,%g) p5=(%g,%g,%g)\r\n",
			(unsigned)FVid_nFrameCounter, pName,
			(float)aTEMPmp[0], (float)aTEMPmp[1], (float)aTEMPmp[2],
			(float)aTEMPmp[4], (float)aTEMPmp[5], (float)aTEMPmp[6],
			(float)aTEMPmp[8], (float)aTEMPmp[9], (float)aTEMPmp[10],
			(float)aTEMPmp[12], (float)aTEMPmp[13], (float)aTEMPmp[14],
			(float)aTEMPmp[16], (float)aTEMPmp[17], (float)aTEMPmp[18],
			(float)aTEMPmp[20], (float)aTEMPmp[21], (float)aTEMPmp[22] );
		DEVPRINTF( "[ma] TEMP mlitC f%u %s c0=(%g,%g,%g) c1=(%g,%g,%g) c2=(%g,%g,%g) c3=(%g,%g,%g) c4=(%g,%g,%g) c5=(%g,%g,%g)\r\n",
			(unsigned)FVid_nFrameCounter, pName,
			(float)aTEMPmc[0], (float)aTEMPmc[1], (float)aTEMPmc[2],
			(float)aTEMPmc[4], (float)aTEMPmc[5], (float)aTEMPmc[6],
			(float)aTEMPmc[8], (float)aTEMPmc[9], (float)aTEMPmc[10],
			(float)aTEMPmc[12], (float)aTEMPmc[13], (float)aTEMPmc[14],
			(float)aTEMPmc[16], (float)aTEMPmc[17], (float)aTEMPmc[18],
			(float)aTEMPmc[20], (float)aTEMPmc[21], (float)aTEMPmc[22] );
		DEVPRINTF( "[ma] TEMP mlitA f%u %s a0=(%g,%g,%g) a1=(%g,%g,%g) a2=(%g,%g,%g) a3=(%g,%g,%g) a4=(%g,%g,%g) a5=(%g,%g,%g)\r\n",
			(unsigned)FVid_nFrameCounter, pName,
			(float)aTEMPma[0], (float)aTEMPma[1], (float)aTEMPma[2],
			(float)aTEMPma[4], (float)aTEMPma[5], (float)aTEMPma[6],
			(float)aTEMPma[8], (float)aTEMPma[9], (float)aTEMPma[10],
			(float)aTEMPma[12], (float)aTEMPma[13], (float)aTEMPma[14],
			(float)aTEMPma[16], (float)aTEMPma[17], (float)aTEMPma[18],
			(float)aTEMPma[20], (float)aTEMPma[21], (float)aTEMPma[22] );
	}

	//R49/R50: name-resolve the surface-pass stage-0 device texture for the
	//gated class ('d' = drawn surface draw; the black-wire report). If the
	//bound DX texture is not in the registry, the stage bind is broken/NULL;
	//if the name is a wrong resource, the material layer binding is broken.
	if ( _TEMPFF( pName ) && cBranch == 'd' )
	{
		LPDIRECT3DTEXTURE8 pTEMPDXTex = NULL;
		FDX8_pDev->GetTexture( 0, &pTEMPDXTex );
		if ( pTEMPDXTex == NULL && _s_pTEMPSurfDXTex ) { pTEMPDXTex = (LPDIRECT3DTEXTURE8)_s_pTEMPSurfDXTex; } //R51: force nulled the device bind; use the pre-force stash
		if ( pTEMPDXTex )
		{
			u32 nTEMPW = 0, nTEMPH = 0, nTEMPFmt = 0, nTEMPFlags = 0;
			const char *pTEMPName = fdx8tex_TEMPFindDXTex( pTEMPDXTex, &nTEMPW, &nTEMPH, &nTEMPFmt, &nTEMPFlags );
			DEVPRINTF( "[ma] TEMP t0name f%u %s dev=%p name=%s w=%u h=%u fmt=%d flags=%d\r\n",
				(unsigned)FVid_nFrameCounter, pName, (void*)pTEMPDXTex,
				pTEMPName ? pTEMPName : "<NOT-IN-REGISTRY>", nTEMPW, nTEMPH,
				(int)nTEMPFmt, (int)nTEMPFlags );
		}
		else
		{
			DEVPRINTF( "[ma] TEMP t0name f%u %s dev=NULL\r\n",
				(unsigned)FVid_nFrameCounter, pName );
		}

		//R52: texture-matrix readback. The surface VS computes
		//oT0 = CV_TEXMTX0 * (V_TEX0,1); the wires (godm_cabl) bind the right
		//texture (tewj_wire01) yet sample black texels under the magenta
		//force -- a stale non-identity TEXMTX0 (scroll/project matrix left
		//behind by another shader execute) would sample the wrong region.
		{
			float aTEMPmt[12];
			memset( aTEMPmt, 0, sizeof(aTEMPmt) );
			if ( FDX8_pDev->GetVertexShaderConstant( CV_TEXMTX0_0, aTEMPmt, 3 ) == D3D_OK )
			{
				DEVPRINTF( "[ma] TEMP mtex f%u %s m0=(%g,%g,%g,%g) m1=(%g,%g,%g,%g) m2=(%g,%g,%g,%g)\r\n",
					(unsigned)FVid_nFrameCounter, pName,
					(float)aTEMPmt[0], (float)aTEMPmt[1], (float)aTEMPmt[2], (float)aTEMPmt[3],
					(float)aTEMPmt[4], (float)aTEMPmt[5], (float)aTEMPmt[6], (float)aTEMPmt[7],
					(float)aTEMPmt[8], (float)aTEMPmt[9], (float)aTEMPmt[10], (float)aTEMPmt[11] );
			}
		}
	}
}

BOOL CFMeshInst::DrawMaterialLight_P( FViewportPlanesMask_t nCrossesPlanesMask, FMeshMaterial_t *pMaterial, u32 nLODIndex )
{
	FASSERT( _bWindowCreated );
	FASSERT( nCrossesPlanesMask != -1 );

	CFMtx43A Mtx43A, *pMtx43A;
	u32 nBoneMtxNum, nStrip;
	s32 nShadowID;
	BOOL bFastPass = FALSE;
	u32 nTEMPClusters = 0, nTEMPPartSkip = 0, nTEMPLodSkip = 0, nTEMPDrawn = 0;
	u32 nTEMPSeg0 = 0, nTEMPSeg1 = 0, nTEMPSegSkin = 0;

	#if FPERF_ENABLE
		u32 nPreStripCount = FPerf_nRawStripTriCount;
		u32 nPreListCount = FPerf_nRawListTriCount;
	#endif

	#if _SUPPORTING_PARTS_DRAWING
		if ( !(pMaterial->nPartIDMask & m_nDrawnPartsMask) )
		{
			return FALSE;
		}
	#endif

	#if FANG_PLATFORM_WIN
		// Setup clipping
		fdx8_SetRenderState_CLIPPING( nCrossesPlanesMask != 0 );
	#endif

	fdx8_SetRenderState_FILLMODE( FRenderer_nD3DFillMode );
	_SetCullDir_P( (FMeshCullDir_e)m_nCullDirection );
	FDX8Mesh_t *pMeshIS = m_pMesh->pMeshIS;
	u32 nMeshBoneCount = m_pMesh->nBoneCount;

	if ( !(m_pMesh->nFlags & FMESH_FLAGS_VOLUME_MESH) )
	{
		if ( nMeshBoneCount == 0 ) 
		{
			// There are no bones in the entire mesh. We can just set a single model matrix...
			fdx8xfm_SetCustomDXMatrix( D3DTS_WORLDMATRIX(0), &m_PosMtx, TRUE );
			fdx8_SetRenderState_VERTEXBLEND( D3DVBF_DISABLE );
			fdx8xfm_SetRenormalizeNormalsIfNeeded( m_fMatrixScale );
		}
		nShadowID = 0x01;
	}
	else
	{
		nShadowID = 0x00;
	}

	// Setup color stream management
	s32 nColorStreamVBIndex = -1;
	if ( m_papColorStreams && !FMesh_bRenderShadows )
	{
		fdx8sh_ToggleExternalColorStream( TRUE );
		_bStream3Active = TRUE;
	}
	else if ( _bStream3Active )
	{
		FDX8_pDev->SetStreamSource( 3, NULL, 0 );
		fdx8sh_ToggleExternalColorStream( FALSE );
		_bStream3Active = FALSE;
	}

	if ( !SetupShaderParameters( pMaterial, FSHADERS_PASS_LIGHTING ) )
	{
		_TEMPMDLog( "light", 'F', m_pMesh, m_pMesh->szName, this, nTEMPClusters, nTEMPPartSkip, nTEMPLodSkip, nTEMPDrawn, nTEMPSeg0, nTEMPSeg1, nTEMPSegSkin, m_pMesh->nBoneCount, m_nDrawnPartsMask, nLODIndex, m_fMatrixScale, m_PosMtx.m_vP.x, m_PosMtx.m_vP.y, m_PosMtx.m_vP.z, (m_pMesh->nFlags & FMESH_FLAGS_VOLUME_MESH) ? 1 : 0, m_nFlags, _TEMPGetPal( m_pMesh->nBoneCount, m_nFlags, m_PosMtx, m_apBoneMtxPalette, NULL ), (s32)(pMaterial - m_pMesh->aMtl) );
		return FALSE;
	}

	// Submit the material's clusters once for each pass that is required
#if FANG_DEBUG_BUILD
	FDX8VB_t *pVB = NULL;
#endif
	FMeshSeg_t *pSeg = NULL;
	FMeshSeg_t *aSeg = m_pMesh->aSeg;
	s32 nCurrentIB = -1, nPass;
	s32 nTotalPasses = fsh_GetCurrentNumPasses( &bFastPass );
#if FMESH_ALLOW_POSTER_BONES
	BOOL bPosterBones = FALSE;
	CFMtx43A PosterMtx;

	if ( (m_nFlags & (FMESHINST_FLAG_POSTER_X | FMESHINST_FLAG_POSTER_Y | FMESHINST_FLAG_POSTER_Z)) && (m_nFlags & FMESHINST_FLAG_POSTER_BONES) )
	{
		if ( m_pMesh->nBoneCount )
		{
			bPosterBones = TRUE;
			PosterMtx.Mul( m_PosMtx, m_Xfm.m_MtxR );
		}
	}
#endif

	for ( nPass = 0; nPass < nTotalPasses; nPass++ )
	{
		fsh_SetPassIdx( nPass );
		fsh_ExecuteCurrent( FMesh_bRenderShadows, nShadowID ); 

		//R49 magenta light-pass force retired: ride 3 (log_e) proved raster,
		//oPos/depth fine for the gate list. R50 moves the test to the surface
		//side (see DrawMaterialSurface_P) to expose the REAL light result.

		// Progress through all the clusters for this material, submitting for render the relevant ones
		FDX8MeshCluster_t *pCluster = ((FDX8MeshMaterial_t *)pMaterial->pPlatformData)->aCluster;
		FDX8MeshCluster_t *pEndCluster = &pCluster[((FDX8MeshMaterial_t *)pMaterial->pPlatformData)->nClusterCount];
		for ( ;pCluster < pEndCluster; pCluster++ )
		{
			nTEMPClusters++;
			if ( !(m_nDrawnPartsMask & (1 << pCluster->nPartID)) )
			{
				nTEMPPartSkip++;
				continue;
			}

			if ( pCluster->nLODID != nLODIndex )
			{
				nTEMPLodSkip++;
				continue;
			}

			nTEMPDrawn++;
			// Select the appropriate vertex buffer for this cluster
			BOOL bUseLightmaps = (FSh_shaderType == SHADERTYPE_DIFFUSE || FSh_shaderType == SHADERTYPE_TRANSLUCENCY);

			fdx8vb_Select( &pMeshIS->aVB[ pCluster->nVBIndex ], FALSE, bUseLightmaps, TRUE );
			fsh_CheckVB();

			// Activate the color stream for this cluster, if there is one
			if ( m_papColorStreams && nColorStreamVBIndex != pCluster->nVBIndex )
			{
				FDX8_pDev->SetStreamSource( 3, ((IDirect3DVertexBuffer8 **)m_papColorStreams)[pCluster->nVBIndex], sizeof(DWORD) );
				nColorStreamVBIndex = pCluster->nVBIndex;
				_bStream3Active = TRUE;
			}

			// Set the index buffer
			if ( nCurrentIB != pCluster->nIBIndex )
			{
				nCurrentIB = pCluster->nIBIndex;
				FDX8_pDev->SetIndices( (IDirect3DIndexBuffer8 *)pMeshIS->apDXIB[nCurrentIB], 0 );
			}

			D3DXMATRIX boneMtx;

			if ( nMeshBoneCount ) 
			{
				FASSERT( pCluster->nSegmentIdx < m_pMesh->nSegCount );

				if ( pSeg != &aSeg[pCluster->nSegmentIdx] )
				{
					pSeg = &aSeg[pCluster->nSegmentIdx];
						if ( pSeg->nBoneMtxCount == 0 ) { nTEMPSeg0++; }
						else if ( pSeg->nBoneMtxCount == 1 ) { nTEMPSeg1++; }
						else { nTEMPSegSkin++; }

					if ( pSeg->nBoneMtxCount == 0 )
					{
						// No bones for this segment, so just set this mesh instance's xfm
						// as the model matrix...
						fdx8_SetRenderState_VERTEXBLEND( D3DVBF_DISABLE );
						fdx8xfm_SetCustomDXMatrix( D3DTS_WORLDMATRIX(0), &m_PosMtx, TRUE );
						fdx8xfm_SetRenormalizeNormalsIfNeeded( m_fMatrixScale );
					} 
					else if ( pSeg->nBoneMtxCount == 1 ) 
					{
						// This is a non-skinned segment...

						fdx8_SetRenderState_VERTEXBLEND( D3DVBF_DISABLE );

						if ( !(m_nFlags & FMESHINST_FLAG_NOBONES) ) 
						{
							#if FMESH_ALLOW_POSTER_BONES
							if ( !bPosterBones )
							{
								fdx8xfm_SetCustomDXMatrix( D3DTS_WORLDMATRIX(0), m_apBoneMtxPalette[ pSeg->anBoneMtxIndex[0] ], TRUE );
							} 
							else
							{
								Mtx43A.Mul33( PosterMtx, *m_apBoneMtxPalette[ pSeg->anBoneMtxIndex[0] ] );
								Mtx43A.m_vPos = m_apBoneMtxPalette[ pSeg->anBoneMtxIndex[0] ]->m_vPos;
								fdx8xfm_SetCustomDXMatrix( D3DTS_WORLDMATRIX(0), &Mtx43A, TRUE );
							}
							#else
							fdx8xfm_SetCustomDXMatrix( D3DTS_WORLDMATRIX(0), m_apBoneMtxPalette[ pSeg->anBoneMtxIndex[0] ], TRUE );
							#endif

							fdx8xfm_SetRenormalizeNormalsIfNeeded( m_apBoneMtxPalette[ pSeg->anBoneMtxIndex[0] ] );
						} 
						else 
						{
							// The FMESHINST_FLAG_NOBONES flag is set and this is a non-skinned segment.
							// This means that the vertices have already been transformed into bone space,
							// and we need to transform them back into at-rest model space...
//							Mtx43A.Mul( m_Xfm.m_MtxF, m_pMesh->pBoneArray[ pSeg->anBoneMtxIndex[0] ].AtRestBoneToModelMtx );
							Mtx43A.Mul( m_PosMtx, m_pMesh->pBoneArray[ pSeg->anBoneMtxIndex[0] ].AtRestBoneToModelMtx );
							fdx8xfm_SetCustomDXMatrix( D3DTS_WORLDMATRIX(0), &Mtx43A, TRUE );
							fdx8xfm_SetRenormalizeNormalsIfNeeded( &Mtx43A );
						}
					} 
					else 
					{
						// This is a skinned segment...
						if ( !(m_nFlags & FMESHINST_FLAG_NOBONES) ) 
						{
							fdx8_SetRenderState_VERTEXBLEND( _aD3DVertexBlendFlags[pSeg->nBoneMtxCount] );

							for( nBoneMtxNum=0; nBoneMtxNum<pSeg->nBoneMtxCount; nBoneMtxNum++ ) 
							{
								pMtx43A = m_apBoneMtxPalette[ pSeg->anBoneMtxIndex[nBoneMtxNum] ];
								D3DXMatrixTranspose(&boneMtx, (const D3DXMATRIX *)pMtx43A);
								FDX8_pDev->SetVertexShaderConstant(CV_BONEMTX0_0 + 3*nBoneMtxNum, &boneMtx(0, 0), 3);
							}
						} 
						else 
						{
							fdx8_SetRenderState_VERTEXBLEND( D3DVBF_DISABLE );
							fdx8xfm_SetCustomDXMatrix( D3DTS_WORLDMATRIX(0), &m_PosMtx, TRUE );
							fdx8xfm_SetRenormalizeNormalsIfNeeded( m_fMatrixScale );
						}
					}
				}
			}

			// Render the tri-list...
			if ( pCluster->TriList.nTriCount )
			{
				_TEMPVData( m_pMesh, pCluster, pMeshIS, (u32)( pMaterial - m_pMesh->aMtl ) );
				_TEMPMClip( "light", m_pMesh->szName, pCluster->TriList.nStartVindex, pCluster->TriList.nTriCount );

				if (FAILED(FDX8_pDev->DrawIndexedPrimitive(
					D3DPT_TRIANGLELIST,
					pCluster->TriList.nVtxIndexMin,
					pCluster->TriList.nVtxIndexRange,
					pCluster->TriList.nStartVindex,
					pCluster->TriList.nTriCount
				)))
				{
					_TEMPDipFail("tri", pCluster->TriList.nVtxIndexMin, pCluster->TriList.nVtxIndexRange, pCluster->TriList.nStartVindex, pCluster->TriList.nTriCount);
				}

				#if FPERF_ENABLE
					FPerf_nRawTriListCount++;
					FPerf_nRawListTriCount += pCluster->TriList.nTriCount;
					FPerf_nRawVertexCount += (pCluster->TriList.nTriCount<<1) + pCluster->TriList.nTriCount;
				#endif
			}

			// Render the strip-list...
			FDX8MeshStrip_t *pStrip;
			for ( nStrip=0, pStrip = pCluster->paStripBuffer; nStrip < pCluster->nStripCount; nStrip++, pStrip++ ) 
			{
				if (FAILED(FDX8_pDev->DrawIndexedPrimitive(
					D3DPT_TRIANGLESTRIP,
					pStrip->nVtxIndexMin,
					pStrip->nVtxIndexRange,
					pStrip->nStartVindex,
					pStrip->nTriCount
				)))
				{
					_TEMPDipFail("strip", pStrip->nVtxIndexMin, pStrip->nVtxIndexRange, pStrip->nStartVindex, pStrip->nTriCount);
				}

				#if FPERF_ENABLE
					FPerf_nRawTriStripCount++;
					FPerf_nRawStripTriCount += pStrip->nTriCount;
					FPerf_nRawVertexCount += pStrip->nTriCount + 2;
				#endif
			}
		}
	}

	fsh_EndExecute();

	if ( m_pMesh->nTexLayerIDCount ) 
	{
		// Reset the texture matrices if there were texture layer effects...
		fsh_TexCoordMtx_ResetList();
		fsh_TexOverride_ResetList();
	}

#if FPERF_ENABLE
	u32 nTrisRendered = (FPerf_nRawStripTriCount - nPreStripCount) + (FPerf_nRawListTriCount - nPreListCount);
	FPerf_nTotalTrisLighting += nTrisRendered;
	FPerf_nMeshTris += nTrisRendered / nTotalPasses;
#endif

	_TEMPMDLog( "light", nTEMPDrawn ? 'D' : 'Z', m_pMesh, m_pMesh->szName, this, nTEMPClusters, nTEMPPartSkip, nTEMPLodSkip, nTEMPDrawn, nTEMPSeg0, nTEMPSeg1, nTEMPSegSkin, m_pMesh->nBoneCount, m_nDrawnPartsMask, nLODIndex, m_fMatrixScale, m_PosMtx.m_vP.x, m_PosMtx.m_vP.y, m_PosMtx.m_vP.z, (m_pMesh->nFlags & FMESH_FLAGS_VOLUME_MESH) ? 1 : 0, m_nFlags, _TEMPGetPal( m_pMesh->nBoneCount, m_nFlags, m_PosMtx, m_apBoneMtxPalette, pSeg ), (s32)(pMaterial - m_pMesh->aMtl) );

	return bFastPass;
}


//
//
//
void CFMeshInst::DrawMaterialSurface_P( FViewportPlanesMask_t nCrossesPlanesMask, FMeshMaterial_t *pMaterial, u32 nLODIndex )
{
	FASSERT( _bWindowCreated );
	FASSERT( nCrossesPlanesMask != -1 );

	CFMtx43A Mtx43A, *pMtx43A;
	u32 nBoneMtxNum, nStrip;
	u32 nTEMPClusters = 0, nTEMPPartSkip = 0, nTEMPLodSkip = 0, nTEMPDrawn = 0;
	u32 nTEMPSeg0 = 0, nTEMPSeg1 = 0, nTEMPSegSkin = 0;

	#if FPERF_ENABLE
		u32 nPreStripCount = FPerf_nRawStripTriCount;
		u32 nPreListCount = FPerf_nRawListTriCount;
	#endif

	#if _SUPPORTING_PARTS_DRAWING
		if ( !(pMaterial->nPartIDMask & m_nDrawnPartsMask) )
		{
			return;
		}
	#endif

	#if FANG_PLATFORM_WIN
		fdx8_SetRenderState_CLIPPING( nCrossesPlanesMask != 0 );
	#endif

//	fdx8_SetRenderState_FILLMODE( FRenderer_nD3DFillMode );
	_SetCullDir_P( (FMeshCullDir_e)m_nCullDirection );
	FDX8Mesh_t *pMeshIS = m_pMesh->pMeshIS;
	u32 nMeshBoneCount = m_pMesh->nBoneCount;

	if ( !(m_pMesh->nFlags & FMESH_FLAGS_VOLUME_MESH) )
	{
		if ( nMeshBoneCount == 0 ) 
		{
			// There are no bones in the entire mesh. We can just set a single model matrix...
			fdx8xfm_SetCustomDXMatrix( D3DTS_WORLDMATRIX(0), &m_PosMtx, TRUE );
			fdx8_SetRenderState_VERTEXBLEND( D3DVBF_DISABLE );
			fdx8xfm_SetRenormalizeNormalsIfNeeded( m_fMatrixScale );
		} 
	}

	if ( !SetupShaderParameters( pMaterial, FSHADERS_PASS_SURFACE ) )
	{
		_TEMPMDLog( "surface", 'f', m_pMesh, m_pMesh->szName, this, nTEMPClusters, nTEMPPartSkip, nTEMPLodSkip, nTEMPDrawn, nTEMPSeg0, nTEMPSeg1, nTEMPSegSkin, m_pMesh->nBoneCount, m_nDrawnPartsMask, nLODIndex, m_fMatrixScale, m_PosMtx.m_vP.x, m_PosMtx.m_vP.y, m_PosMtx.m_vP.z, (m_pMesh->nFlags & FMESH_FLAGS_VOLUME_MESH) ? 1 : 0, m_nFlags, _TEMPGetPal( m_pMesh->nBoneCount, m_nFlags, m_PosMtx, m_apBoneMtxPalette, NULL ), (s32)(pMaterial - m_pMesh->aMtl) );
		return;
	}

	// Pass in procedural data
	fsh_SetProcedural( m_pProcedural );

	// Submit the material's clusters once for each pass that is required
#if FANG_DEBUG_BUILD
	FDX8VB_t *pVB = NULL;
#endif
	FMeshSeg_t *pSeg = NULL;
	FMeshSeg_t *aSeg = m_pMesh->aSeg;
	s32 nCurrentIB = -1;

	fsh_SetSurfaceOnly( !!(m_nFlags&FMESHINST_FLAG_SURFACEONLY) );

	if ( FMesh_nLastSurfaceMaterialCRC == pMaterial->nDLHashKey )
	{
		fsh_FastSurfaceExecute();
#if FPERF_ENABLE
		FPerf_nFastSurfaceShaderCount++;
#endif
	}
	else
	{
		fsh_ExecuteCurrent( FALSE, 0 );
		FMesh_nLastSurfaceMaterialCRC = pMaterial->nDLHashKey;
#if FPERF_ENABLE
		FPerf_nFullSurfaceShaderCount++;
#endif
	}

	//R50: surface-side force for the gate list. Replaces the surface pixel
	//shader with a fixed-function white select so the multiplicative blend
	//(DESTCOLOR/SRCCOLOR under FANG_LIGHTING_2X) yields dst*2 -- what the
	//user then sees IS the unmodified light-pass result, brightened 2x.
	//Visible-but-black wires/deadwalls under this force => light-pass
	//content is zero (vertex-lighting math). Missing => the surface pass
	//lands selectively (depth/decl by material). Cutout meshes will show
	//as solid quads (alpha test bypassed with the PS off) -- expected.
	{
		u32 nTEMPStage;
		s32 nTEMPFfPin = _TEMPFFPin( this );
		if ( _TEMPFF( m_pMesh->szName ) || nTEMPFfPin >= 0 )
		{
			//R52: capture everything we are about to bypass (restored at the
			//function tail so the state cache's model stays truthful).
			FDX8_pDev->GetPixelShader( &_s_nTEMPSavedPS );
			FDX8_pDev->GetTexture( 0, &_s_pTEMPSavedTex0 );
			FDX8_pDev->GetTexture( 1, &_s_pTEMPSavedTex1 );
			for ( nTEMPStage = 0; nTEMPStage < 4; nTEMPStage++ )
			{
				FDX8_pDev->GetTextureStageState( 0, _s_anTEMPTSS[nTEMPStage], &_s_anTEMPSavedTSS[nTEMPStage] );
			}
			FDX8_pDev->GetRenderState( D3DRS_TEXTUREFACTOR, &_s_nTEMPSavedTF );
			FDX8_pDev->GetRenderState( D3DRS_CULLMODE, &_s_nTEMPSavedCull );
			FDX8_pDev->GetRenderState( D3DRS_ZFUNC, &_s_nTEMPSavedZfunc );
			FDX8_pDev->GetRenderState( D3DRS_ZWRITEENABLE, &_s_nTEMPSavedZwrite );
			FDX8_pDev->GetRenderState( D3DRS_ALPHABLENDENABLE, &_s_nTEMPSavedAbEnable );
			FDX8_pDev->GetRenderState( D3DRS_SRCBLEND, &_s_nTEMPSavedSrcBlend );
			FDX8_pDev->GetRenderState( D3DRS_DESTBLEND, &_s_nTEMPSavedDstBlend );
			FDX8_pDev->GetRenderState( D3DRS_ALPHATESTENABLE, &_s_nTEMPSavedAtEnable );
			_s_bTEMPSurfForced = TRUE;

			FDX8_pDev->SetPixelShader( NULL );
			{
				//R51: read the bound t0 BEFORE nulling it (t0name needs it).
				LPDIRECT3DTEXTURE8 pTEMPStashTex = NULL;
				FDX8_pDev->GetTexture( 0, &pTEMPStashTex );
				_s_pTEMPSurfDXTex = (void *)pTEMPStashTex;
			}
			FDX8_pDev->SetTexture( 0, NULL );
			FDX8_pDev->SetTexture( 1, NULL );
			for ( nTEMPStage = 0; nTEMPStage < 4; nTEMPStage++ )
			{
				FDX8_pDev->SetTextureStageState( 0, _s_anTEMPTSS[nTEMPStage], (nTEMPStage & 1) ? D3DTA_TFACTOR : D3DTOP_SELECTARG1 );
			}
			//R53: LOUD -- opaque magenta, not blended by dst, never culled,
			//equal-or-nearer z with write. Nothing about the light-pass
			//content can hide this; if it is not magenta on screen, the
			//geometry produced no fragments at all.
			//R55: colour now encodes WHY it was forced: white = old name
			//gate (control), magenta/cyan/yellow = geometry within reach of
			//fixture 0/1/2. ZFUNC is back to LESSEQUAL (R54's ALWAYS only
			//made far forced meshes draw through walls, which told us
			//nothing).
			//R55: log the discovery + the ambient the render sort actually
			//set for this entry (fmesh_Ambient_Set runs just before this
			//call), which tests the "class ambient is zero" theory directly.
			{
				u32 nTEMPMatIdx = (u32)( pMaterial - m_pMesh->aMtl );
				//Once per (name, mat, fix) -- otherwise this repeats for every
				//material of every pinned mesh on every frame.
				if ( !_TEMPMDSeen( (const void *)( (size_t)m_pMesh->szName ^ (size_t)( (nTEMPFfPin + 2) * 8192 + (s32)nTEMPMatIdx ) ), 'N' ) )
				{
					float aTEMPAmb[4];
					memset( aTEMPAmb, 0, sizeof(aTEMPAmb) );
					FDX8_pDev->GetVertexShaderConstant( CV_MAT_AMBIENT, aTEMPAmb, 1 );
					DEVPRINTF( "[ma] TEMP ffpin f%u gate=%d fix=%d rad=%g %s mat=%d pos=(%g,%g,%g) scale=%g cvamb=(%g,%g,%g) final=(%g,%g,%g) motif=(%g,%g,%g)\r\n",
						(unsigned)FVid_nFrameCounter, _TEMPFF( m_pMesh->szName ) ? 1 : 0, nTEMPFfPin, (float)_fTEMPFfPinRadius, m_pMesh->szName, (int)nTEMPMatIdx,
						(float)m_PosMtx.m_vP.x, (float)m_PosMtx.m_vP.y, (float)m_PosMtx.m_vP.z, (float)m_fMatrixScale,
						(float)aTEMPAmb[0], (float)aTEMPAmb[1], (float)aTEMPAmb[2],
						(float)m_FinalAmbientRGB.fRed, (float)m_FinalAmbientRGB.fGreen, (float)m_FinalAmbientRGB.fBlue,
						(float)FWorld_AmbientMotif.ColorRGB.fRed, (float)FWorld_AmbientMotif.ColorRGB.fGreen, (float)FWorld_AmbientMotif.ColorRGB.fBlue );
				}
			}
			{
				DWORD nTEMPFfCol = 0xFFFFFFFF;	//white = name gate
				if ( nTEMPFfPin >= 0 ) { nTEMPFfCol = _anTEMPFFPinCol[ nTEMPFfPin ]; }
				FDX8_pDev->SetRenderState( D3DRS_TEXTUREFACTOR, nTEMPFfCol );
			}
			FDX8_pDev->SetRenderState( D3DRS_ALPHABLENDENABLE, FALSE );
			FDX8_pDev->SetRenderState( D3DRS_ALPHATESTENABLE, FALSE );
			FDX8_pDev->SetRenderState( D3DRS_CULLMODE, D3DCULL_NONE );
			FDX8_pDev->SetRenderState( D3DRS_ZFUNC, D3DCMP_LESSEQUAL );
			FDX8_pDev->SetRenderState( D3DRS_ZWRITEENABLE, TRUE );
		}
	}
	
	// Progress through all the clusters for this material, submitting for render the relevant ones
	FDX8MeshCluster_t *pCluster = ((FDX8MeshMaterial_t *)pMaterial->pPlatformData)->aCluster;
	FDX8MeshCluster_t *pEndCluster = &pCluster[((FDX8MeshMaterial_t *)pMaterial->pPlatformData)->nClusterCount];
#if FMESH_ALLOW_POSTER_BONES
	BOOL bPosterBones = FALSE;
	CFMtx43A PosterMtx;

	if ( (m_nFlags & (FMESHINST_FLAG_POSTER_X | FMESHINST_FLAG_POSTER_Y | FMESHINST_FLAG_POSTER_Z)) && (m_nFlags & FMESHINST_FLAG_POSTER_BONES) )
	{
		if ( m_pMesh->nBoneCount )
		{
			bPosterBones = TRUE;
			PosterMtx.Mul( m_PosMtx, m_Xfm.m_MtxR );
		}
	}
#endif

	for ( ;pCluster < pEndCluster; pCluster++ )
	{
		nTEMPClusters++;
		if ( !(m_nDrawnPartsMask & (1 << pCluster->nPartID)) )
		{
			nTEMPPartSkip++;
			continue;
		}

		if ( pCluster->nLODID != nLODIndex )
		{
			nTEMPLodSkip++;
			continue;
		}
		nTEMPDrawn++;

		// Select the appropriate vertex buffer for this cluster
		fdx8vb_Select( &pMeshIS->aVB[ pCluster->nVBIndex ], FALSE, FALSE, TRUE );
		fsh_CheckVB();

		// Set the index buffer
		if ( nCurrentIB != pCluster->nIBIndex )
		{
			nCurrentIB = pCluster->nIBIndex;
			FDX8_pDev->SetIndices( (IDirect3DIndexBuffer8 *)pMeshIS->apDXIB[nCurrentIB], 0 );
		}

		D3DXMATRIX boneMtx;

		if ( nMeshBoneCount ) 
		{
			FASSERT( pCluster->nSegmentIdx < m_pMesh->nSegCount );

			if ( pSeg != &aSeg[pCluster->nSegmentIdx] )
			{
				pSeg = &aSeg[pCluster->nSegmentIdx];
					if ( pSeg->nBoneMtxCount == 0 ) { nTEMPSeg0++; }
					else if ( pSeg->nBoneMtxCount == 1 ) { nTEMPSeg1++; }
					else { nTEMPSegSkin++; }

				if ( pSeg->nBoneMtxCount == 0 )
				{
					// No bones for this segment, so just set this mesh instance's xfm
					// as the model matrix...
					fdx8_SetRenderState_VERTEXBLEND( D3DVBF_DISABLE );
					fdx8xfm_SetCustomDXMatrix( D3DTS_WORLDMATRIX(0), &m_PosMtx, TRUE );
					fdx8xfm_SetRenormalizeNormalsIfNeeded( m_fMatrixScale );
				} 
				else if ( pSeg->nBoneMtxCount == 1 ) 
				{
					// This is a non-skinned segment...

					fdx8_SetRenderState_VERTEXBLEND( D3DVBF_DISABLE );

					if ( !(m_nFlags & FMESHINST_FLAG_NOBONES) ) 
					{
						#if FMESH_ALLOW_POSTER_BONES
						if ( !bPosterBones )
						{
							fdx8xfm_SetCustomDXMatrix( D3DTS_WORLDMATRIX(0), m_apBoneMtxPalette[ pSeg->anBoneMtxIndex[0] ], TRUE );
						} 
						else
						{
							Mtx43A.Mul33( PosterMtx, *m_apBoneMtxPalette[ pSeg->anBoneMtxIndex[0] ] );
							Mtx43A.m_vPos = m_apBoneMtxPalette[ pSeg->anBoneMtxIndex[0] ]->m_vPos;
							fdx8xfm_SetCustomDXMatrix( D3DTS_WORLDMATRIX(0), &Mtx43A, TRUE );
						}
						#else
						fdx8xfm_SetCustomDXMatrix( D3DTS_WORLDMATRIX(0), m_apBoneMtxPalette[ pSeg->anBoneMtxIndex[0] ], TRUE );
						#endif

						fdx8xfm_SetRenormalizeNormalsIfNeeded( m_apBoneMtxPalette[ pSeg->anBoneMtxIndex[0] ] );
					} 
					else 
					{
						// The FMESHINST_FLAG_NOBONES flag is set and this is a non-skinned segment.
						// This means that the vertices have already been transformed into bone space,
						// and we need to transform them back into at-rest model space...
//						Mtx43A.Mul( m_Xfm.m_MtxF, m_pMesh->pBoneArray[ pSeg->anBoneMtxIndex[0] ].AtRestBoneToModelMtx );
						Mtx43A.Mul( m_PosMtx, m_pMesh->pBoneArray[ pSeg->anBoneMtxIndex[0] ].AtRestBoneToModelMtx );
						fdx8xfm_SetCustomDXMatrix( D3DTS_WORLDMATRIX(0), &Mtx43A, TRUE );
						fdx8xfm_SetRenormalizeNormalsIfNeeded( &Mtx43A );
					}
				} 
				else 
				{
					// This is a skinned segment...

					if ( !(m_nFlags & FMESHINST_FLAG_NOBONES) ) 
					{
						fdx8_SetRenderState_VERTEXBLEND( _aD3DVertexBlendFlags[pSeg->nBoneMtxCount] );

						for( nBoneMtxNum=0; nBoneMtxNum<pSeg->nBoneMtxCount; nBoneMtxNum++ ) 
						{
							pMtx43A = m_apBoneMtxPalette[ pSeg->anBoneMtxIndex[nBoneMtxNum] ];
							D3DXMatrixTranspose(&boneMtx, (const D3DXMATRIX *)pMtx43A);
							FDX8_pDev->SetVertexShaderConstant(CV_BONEMTX0_0 + 3*nBoneMtxNum, &boneMtx(0, 0), 3);
						}
					} 
					else 
					{
						fdx8_SetRenderState_VERTEXBLEND( D3DVBF_DISABLE );
						fdx8xfm_SetCustomDXMatrix( D3DTS_WORLDMATRIX(0), &m_PosMtx, TRUE );
						fdx8xfm_SetRenormalizeNormalsIfNeeded( m_fMatrixScale );
					}
				}
			}
		}

		// Render the tri-list...
		if ( pCluster->TriList.nTriCount )
		{
				_TEMPMClip( __FUNCTION__, m_pMesh->szName, pCluster->TriList.nStartVindex, pCluster->TriList.nTriCount );
				if (FAILED(FDX8_pDev->DrawIndexedPrimitive(
				D3DPT_TRIANGLELIST,
				pCluster->TriList.nVtxIndexMin,
				pCluster->TriList.nVtxIndexRange,
				pCluster->TriList.nStartVindex,
				pCluster->TriList.nTriCount
			)))
			{
				_TEMPDipFail("tri", pCluster->TriList.nVtxIndexMin, pCluster->TriList.nVtxIndexRange, pCluster->TriList.nStartVindex, pCluster->TriList.nTriCount);
			}

			#if FPERF_ENABLE
				FPerf_nRawTriListCount++;
				FPerf_nRawListTriCount += pCluster->TriList.nTriCount;
				FPerf_nRawVertexCount += (pCluster->TriList.nTriCount<<1) + pCluster->TriList.nTriCount;
			#endif
		}

		// Render the strip-list...
		FDX8MeshStrip_t *pStrip;
		for ( nStrip=0, pStrip = pCluster->paStripBuffer; nStrip < pCluster->nStripCount; nStrip++, pStrip++ ) 
		{
				if (FAILED(FDX8_pDev->DrawIndexedPrimitive(
				D3DPT_TRIANGLESTRIP,
				pStrip->nVtxIndexMin,
				pStrip->nVtxIndexRange,
				pStrip->nStartVindex,
				pStrip->nTriCount
			)))
			{
				_TEMPDipFail("strip", pStrip->nVtxIndexMin, pStrip->nVtxIndexRange, pStrip->nStartVindex, pStrip->nTriCount);
			}

			#if FPERF_ENABLE
				FPerf_nRawTriStripCount++;
				FPerf_nRawStripTriCount += pStrip->nTriCount;
				FPerf_nRawVertexCount += pStrip->nTriCount + 2;
			#endif
		}
	}

	fsh_EndExecute();

	if ( m_pMesh->nTexLayerIDCount ) 
	{
		// Reset the lists if there were texture layer effects...
		fsh_TexCoordMtx_ResetList();
		fsh_TexOverride_ResetList();
	}

	_TEMPMDLog( "surface", nTEMPDrawn ? 'd' : 'z', m_pMesh, m_pMesh->szName, this, nTEMPClusters, nTEMPPartSkip, nTEMPLodSkip, nTEMPDrawn, nTEMPSeg0, nTEMPSeg1, nTEMPSegSkin, m_pMesh->nBoneCount, m_nDrawnPartsMask, nLODIndex, m_fMatrixScale, m_PosMtx.m_vP.x, m_PosMtx.m_vP.y, m_PosMtx.m_vP.z, (m_pMesh->nFlags & FMESH_FLAGS_VOLUME_MESH) ? 1 : 0, m_nFlags, _TEMPGetPal( m_pMesh->nBoneCount, m_nFlags, m_PosMtx, m_apBoneMtxPalette, pSeg ), (s32)(pMaterial - m_pMesh->aMtl) );

	#if FPERF_ENABLE
		FPerf_nTotalTrisSurface += (FPerf_nRawStripTriCount - nPreStripCount) + (FPerf_nRawListTriCount - nPreListCount);
	#endif

	//R52: restore the bypassed state (see the force block at the top of this
	//function). DrawMaterialSurface_P has no early returns after the force,
	//so this tail is the only restore point needed.
	if ( _s_bTEMPSurfForced )
	{
		u32 nTEMPStageR;
		FDX8_pDev->SetPixelShader( _s_nTEMPSavedPS );
		FDX8_pDev->SetTexture( 0, _s_pTEMPSavedTex0 );
		FDX8_pDev->SetTexture( 1, _s_pTEMPSavedTex1 );
		for ( nTEMPStageR = 0; nTEMPStageR < 4; nTEMPStageR++ )
		{
			FDX8_pDev->SetTextureStageState( 0, _s_anTEMPTSS[nTEMPStageR], _s_anTEMPSavedTSS[nTEMPStageR] );
		}
		FDX8_pDev->SetRenderState( D3DRS_TEXTUREFACTOR, _s_nTEMPSavedTF );
		FDX8_pDev->SetRenderState( D3DRS_CULLMODE, _s_nTEMPSavedCull );
		FDX8_pDev->SetRenderState( D3DRS_ZFUNC, _s_nTEMPSavedZfunc );
		FDX8_pDev->SetRenderState( D3DRS_ZWRITEENABLE, _s_nTEMPSavedZwrite );
		FDX8_pDev->SetRenderState( D3DRS_ALPHABLENDENABLE, _s_nTEMPSavedAbEnable );
		FDX8_pDev->SetRenderState( D3DRS_SRCBLEND, _s_nTEMPSavedSrcBlend );
		FDX8_pDev->SetRenderState( D3DRS_DESTBLEND, _s_nTEMPSavedDstBlend );
		FDX8_pDev->SetRenderState( D3DRS_ALPHATESTENABLE, _s_nTEMPSavedAtEnable );
		_s_bTEMPSurfForced = FALSE;
	}
}


//
//
//
void CFMeshInst::DrawMaterialSpecular_P( FViewportPlanesMask_t nCrossesPlanesMask, FMeshMaterial_t *pMaterial, u32 nLODIndex )
{
	FASSERT( _bWindowCreated );
	FASSERT( nCrossesPlanesMask != -1 );

	CFMtx43A Mtx43A, *pMtx43A;
	u32 nBoneMtxNum, nStrip;

	#if FPERF_ENABLE
		u32 nPreStripCount = FPerf_nRawStripTriCount;
		u32 nPreListCount = FPerf_nRawListTriCount;
	#endif

	#if _SUPPORTING_PARTS_DRAWING
		if ( !(pMaterial->nPartIDMask & m_nDrawnPartsMask) )
		{
			return;
		}
	#endif

	#if FANG_PLATFORM_WIN
		fdx8_SetRenderState_CLIPPING( nCrossesPlanesMask != 0 );
	#endif

	fdx8_SetRenderState_FILLMODE( FRenderer_nD3DFillMode );
	_SetCullDir_P( (FMeshCullDir_e)m_nCullDirection );
	FDX8Mesh_t *pMeshIS = m_pMesh->pMeshIS;
	u32 nMeshBoneCount = m_pMesh->nBoneCount;

	if ( !(m_pMesh->nFlags & FMESH_FLAGS_VOLUME_MESH) )
	{
		if ( nMeshBoneCount == 0 ) 
		{
			// There are no bones in the entire mesh. We can just set a single model matrix...
			fdx8xfm_SetCustomDXMatrix( D3DTS_WORLDMATRIX(0), &m_PosMtx, TRUE );
			fdx8_SetRenderState_VERTEXBLEND( D3DVBF_DISABLE );
			fdx8xfm_SetRenormalizeNormalsIfNeeded( m_fMatrixScale );
		} 
	}

	if ( !SetupShaderParameters( pMaterial, FSHADERS_PASS_SPECULAR ) )
	{
		return;
	}

	// Submit the material's clusters once for each pass that is required
#if FANG_DEBUG_BUILD
	FDX8VB_t *pVB = NULL;
#endif
	FMeshSeg_t *pSeg = NULL;
	FMeshSeg_t *aSeg = m_pMesh->aSeg;
	s32 nCurrentIB = -1;

	fsh_ExecuteCurrent( FALSE, 0 );

	// Progress through all the clusters for this material, submitting for render the relevant ones
	FDX8MeshCluster_t *pCluster = ((FDX8MeshMaterial_t *)pMaterial->pPlatformData)->aCluster;
	FDX8MeshCluster_t *pEndCluster = &pCluster[((FDX8MeshMaterial_t *)pMaterial->pPlatformData)->nClusterCount];
#if FMESH_ALLOW_POSTER_BONES
	BOOL bPosterBones = FALSE;
	CFMtx43A PosterMtx;

	if ( (m_nFlags & (FMESHINST_FLAG_POSTER_X | FMESHINST_FLAG_POSTER_Y | FMESHINST_FLAG_POSTER_Z)) && (m_nFlags & FMESHINST_FLAG_POSTER_BONES) )
	{
		if ( m_pMesh->nBoneCount )
		{
			bPosterBones = TRUE;
			PosterMtx.Mul( m_PosMtx, m_Xfm.m_MtxR );
		}
	}
#endif

	for ( ;pCluster < pEndCluster; pCluster++ )
	{
		if ( !(m_nDrawnPartsMask & (1 << pCluster->nPartID)) )
		{
			continue;
		}

		if ( pCluster->nLODID != nLODIndex )
		{
			continue;
		}

		// Select the appropriate vertex buffer for this cluster
		fdx8vb_Select( &pMeshIS->aVB[ pCluster->nVBIndex ], FALSE, FALSE, TRUE );
		fsh_CheckVB();

		// Set the index buffer
		if ( nCurrentIB != pCluster->nIBIndex )
		{
			nCurrentIB = pCluster->nIBIndex;
			FDX8_pDev->SetIndices( (IDirect3DIndexBuffer8 *)pMeshIS->apDXIB[nCurrentIB], 0 );
		}

		D3DXMATRIX boneMtx;

		if ( nMeshBoneCount ) 
		{
			FASSERT( pCluster->nSegmentIdx < m_pMesh->nSegCount );

			if ( pSeg != &aSeg[pCluster->nSegmentIdx] )
			{
				pSeg = &aSeg[pCluster->nSegmentIdx];

				if ( pSeg->nBoneMtxCount == 0 )
				{
					// No bones for this segment, so just set this mesh instance's xfm
					// as the model matrix...
					fdx8_SetRenderState_VERTEXBLEND( D3DVBF_DISABLE );
					fdx8xfm_SetCustomDXMatrix( D3DTS_WORLDMATRIX(0), &m_PosMtx, TRUE );
					fdx8xfm_SetRenormalizeNormalsIfNeeded( m_fMatrixScale );
				} 
				else if ( pSeg->nBoneMtxCount == 1 ) 
				{
					// This is a non-skinned segment...

					fdx8_SetRenderState_VERTEXBLEND( D3DVBF_DISABLE );

					if ( !(m_nFlags & FMESHINST_FLAG_NOBONES) ) 
					{
						#if FMESH_ALLOW_POSTER_BONES
						if ( !bPosterBones )
						{
							fdx8xfm_SetCustomDXMatrix( D3DTS_WORLDMATRIX(0), m_apBoneMtxPalette[ pSeg->anBoneMtxIndex[0] ], TRUE );
						} 
						else
						{
							Mtx43A.Mul33( PosterMtx, *m_apBoneMtxPalette[ pSeg->anBoneMtxIndex[0] ] );
							Mtx43A.m_vPos = m_apBoneMtxPalette[ pSeg->anBoneMtxIndex[0] ]->m_vPos;
							fdx8xfm_SetCustomDXMatrix( D3DTS_WORLDMATRIX(0), &Mtx43A, TRUE );
						}
						#else
						fdx8xfm_SetCustomDXMatrix( D3DTS_WORLDMATRIX(0), m_apBoneMtxPalette[ pSeg->anBoneMtxIndex[0] ], TRUE );
						#endif
						fdx8xfm_SetRenormalizeNormalsIfNeeded( m_apBoneMtxPalette[ pSeg->anBoneMtxIndex[0] ] );
					} 
					else 
					{
						// The FMESHINST_FLAG_NOBONES flag is set and this is a non-skinned segment.
						// This means that the vertices have already been transformed into bone space,
						// and we need to transform them back into at-rest model space...
//						Mtx43A.Mul( m_Xfm.m_MtxF, m_pMesh->pBoneArray[ pSeg->anBoneMtxIndex[0] ].AtRestBoneToModelMtx );
						Mtx43A.Mul( m_PosMtx, m_pMesh->pBoneArray[ pSeg->anBoneMtxIndex[0] ].AtRestBoneToModelMtx );
						fdx8xfm_SetCustomDXMatrix( D3DTS_WORLDMATRIX(0), &Mtx43A, TRUE );
						fdx8xfm_SetRenormalizeNormalsIfNeeded( &Mtx43A );
					}
				} 
				else 
				{
					// This is a skinned segment...

					if ( !(m_nFlags & FMESHINST_FLAG_NOBONES) ) 
					{
						fdx8_SetRenderState_VERTEXBLEND( _aD3DVertexBlendFlags[pSeg->nBoneMtxCount] );

						for( nBoneMtxNum=0; nBoneMtxNum<pSeg->nBoneMtxCount; nBoneMtxNum++ ) 
						{
							pMtx43A = m_apBoneMtxPalette[ pSeg->anBoneMtxIndex[nBoneMtxNum] ];
							D3DXMatrixTranspose(&boneMtx, (const D3DXMATRIX *)pMtx43A);
							FDX8_pDev->SetVertexShaderConstant(CV_BONEMTX0_0 + 3*nBoneMtxNum, &boneMtx(0, 0), 3);
						}
					} 
					else 
					{
						fdx8_SetRenderState_VERTEXBLEND( D3DVBF_DISABLE );
						fdx8xfm_SetCustomDXMatrix( D3DTS_WORLDMATRIX(0), &m_PosMtx, TRUE );
						fdx8xfm_SetRenormalizeNormalsIfNeeded( m_fMatrixScale );
					}
				}
			}
		}

		// Render the tri-list...
		if ( pCluster->TriList.nTriCount )
		{
				_TEMPMClip( __FUNCTION__, m_pMesh->szName, pCluster->TriList.nStartVindex, pCluster->TriList.nTriCount );
				if (FAILED(FDX8_pDev->DrawIndexedPrimitive(
				D3DPT_TRIANGLELIST,
				pCluster->TriList.nVtxIndexMin,
				pCluster->TriList.nVtxIndexRange,
				pCluster->TriList.nStartVindex,
				pCluster->TriList.nTriCount
			)))
			{
				_TEMPDipFail("tri", pCluster->TriList.nVtxIndexMin, pCluster->TriList.nVtxIndexRange, pCluster->TriList.nStartVindex, pCluster->TriList.nTriCount);
			}

			#if FPERF_ENABLE
				FPerf_nRawTriListCount++;
				FPerf_nRawListTriCount += pCluster->TriList.nTriCount;
				FPerf_nRawVertexCount += (pCluster->TriList.nTriCount<<1) + pCluster->TriList.nTriCount;
			#endif
		}

		// Render the strip-list...
		FDX8MeshStrip_t *pStrip;
		for ( nStrip=0, pStrip = pCluster->paStripBuffer; nStrip < pCluster->nStripCount; nStrip++, pStrip++ ) 
		{
				if (FAILED(FDX8_pDev->DrawIndexedPrimitive(
				D3DPT_TRIANGLESTRIP,
				pStrip->nVtxIndexMin,
				pStrip->nVtxIndexRange,
				pStrip->nStartVindex,
				pStrip->nTriCount
			)))
			{
				_TEMPDipFail("strip", pStrip->nVtxIndexMin, pStrip->nVtxIndexRange, pStrip->nStartVindex, pStrip->nTriCount);
			}

			#if FPERF_ENABLE
				FPerf_nRawTriStripCount++;
				FPerf_nRawStripTriCount += pStrip->nTriCount;
				FPerf_nRawVertexCount += pStrip->nTriCount + 2;
			#endif
		}
	}

	fsh_EndExecute();

#if FPERF_ENABLE
	FPerf_nTotalTrisSpecular += (FPerf_nRawStripTriCount - nPreStripCount) + (FPerf_nRawListTriCount - nPreListCount);
#endif
}


//
//
//
void CFMeshInst::DrawMaterialTranslucency_P( FViewportPlanesMask_t nCrossesPlanesMask, FMeshMaterial_t *pMaterial, u32 nLODIndex )
{
	FASSERT( _bWindowCreated );
	FASSERT( nCrossesPlanesMask != -1 );

	CFMtx43A Mtx43A, *pMtx43A;
	u32 nBoneMtxNum, nStrip;

	#if FPERF_ENABLE
		u32 nPreStripCount = FPerf_nRawStripTriCount;
		u32 nPreListCount = FPerf_nRawListTriCount;
	#endif

	#if _SUPPORTING_PARTS_DRAWING
		if ( !(pMaterial->nPartIDMask & m_nDrawnPartsMask) )
		{
			return;
		}
	#endif

	#if FANG_PLATFORM_WIN
		fdx8_SetRenderState_CLIPPING( nCrossesPlanesMask != 0 );
	#endif

	fdx8_SetRenderState_FILLMODE( FRenderer_nD3DFillMode );
	_SetCullDir_P( (FMeshCullDir_e)m_nCullDirection );
	FDX8Mesh_t *pMeshIS = m_pMesh->pMeshIS;
	u32 nMeshBoneCount = m_pMesh->nBoneCount;

	if ( !(m_pMesh->nFlags & FMESH_FLAGS_VOLUME_MESH) )
	{
		if ( nMeshBoneCount == 0 ) 
		{
			// There are no bones in the entire mesh. We can just set a single model matrix...
			fdx8xfm_SetViewDXMatrix( TRUE );
			fdx8xfm_SetCustomDXMatrix( D3DTS_WORLDMATRIX(0), &m_PosMtx, TRUE );
			fdx8_SetRenderState_VERTEXBLEND( D3DVBF_DISABLE );
			fdx8xfm_SetRenormalizeNormalsIfNeeded( m_fMatrixScale );
		} 
		else 
		{
			// This mesh has bones. Set only the view matrix because
			// the model matrix/matrices must be computed for each segment...
			fdx8xfm_SetViewDXMatrix( TRUE );
		}
	}

	if ( !SetupShaderParameters( pMaterial, FSHADERS_PASS_ALL ) )
	{
		return;
	}

	// Setup color stream management
	s32 nColorStreamVBIndex = -1;
	if ( m_papColorStreams && !FMesh_bRenderShadows )
	{
		FDX8_pDev->SetStreamSource( 3, ((IDirect3DVertexBuffer8 **)m_papColorStreams)[0], sizeof(DWORD) );
		fdx8sh_ToggleExternalColorStream( TRUE );
		_bStream3Active = TRUE;
	}
	else if ( _bStream3Active )
	{
		FDX8_pDev->SetStreamSource( 3, NULL, 0 );
		fdx8sh_ToggleExternalColorStream( FALSE );
		_bStream3Active = FALSE;
	}

	// Pass in procedural data
	fsh_SetProcedural( m_pProcedural );

	// Submit the material's clusters once for each pass that is required
#if FANG_DEBUG_BUILD
	FDX8VB_t *pVB = NULL;
#endif
	FMeshSeg_t *pSeg = NULL;
	FMeshSeg_t *aSeg = m_pMesh->aSeg;
	s32 nCurrentIB = -1, nPass;
	
	s32 nTotalPasses = fsh_GetCurrentNumPasses();
#if FMESH_ALLOW_POSTER_BONES
	BOOL bPosterBones = FALSE;
	CFMtx43A PosterMtx;

	if ( (m_nFlags & (FMESHINST_FLAG_POSTER_X | FMESHINST_FLAG_POSTER_Y | FMESHINST_FLAG_POSTER_Z)) && (m_nFlags & FMESHINST_FLAG_POSTER_BONES) )
	{
		if ( m_pMesh->nBoneCount )
		{
			bPosterBones = TRUE;
			PosterMtx.Mul( m_PosMtx, m_Xfm.m_MtxR );
		}
	}
#endif

	for ( nPass = 0; nPass < nTotalPasses; nPass++ )
	{
		fsh_SetPassIdx( nPass );
		fsh_ExecuteCurrent( FALSE, 0 );

		// Progress through all the clusters for this material, submitting for render the relevant ones
		FDX8MeshCluster_t *pCluster = ((FDX8MeshMaterial_t *)pMaterial->pPlatformData)->aCluster;
		FDX8MeshCluster_t *pEndCluster = &pCluster[((FDX8MeshMaterial_t *)pMaterial->pPlatformData)->nClusterCount];
		for ( ;pCluster < pEndCluster; pCluster++ )
		{
			if ( !(m_nDrawnPartsMask & (1 << pCluster->nPartID)) )
			{
				continue;
			}

			if ( pCluster->nLODID != nLODIndex )
			{
				continue;
			}

			// Select the appropriate vertex buffer for this cluster
			fdx8vb_Select( &pMeshIS->aVB[ pCluster->nVBIndex ], FALSE, TRUE, TRUE );
			fsh_CheckVB();

			// Activate the color stream for this cluster, if there is one
			if ( m_papColorStreams && nColorStreamVBIndex != pCluster->nVBIndex )
			{
				FDX8_pDev->SetStreamSource( 3, ((IDirect3DVertexBuffer8 **)m_papColorStreams)[pCluster->nVBIndex], sizeof(DWORD) );
				nColorStreamVBIndex = pCluster->nVBIndex;
			}

			// Set the index buffer
			if ( nCurrentIB != pCluster->nIBIndex )
			{
				nCurrentIB = pCluster->nIBIndex;
				FDX8_pDev->SetIndices( (IDirect3DIndexBuffer8 *)pMeshIS->apDXIB[nCurrentIB], 0 );
			}

			D3DXMATRIX boneMtx;

			if ( nMeshBoneCount ) 
			{
				FASSERT( pCluster->nSegmentIdx < m_pMesh->nSegCount );

				if ( pSeg != &aSeg[pCluster->nSegmentIdx] )
				{
					pSeg = &aSeg[pCluster->nSegmentIdx];

					if ( pSeg->nBoneMtxCount == 0 )
					{
						// No bones for this segment, so just set this mesh instance's xfm
						// as the model matrix...
						fdx8_SetRenderState_VERTEXBLEND( D3DVBF_DISABLE );
						fdx8xfm_SetCustomDXMatrix( D3DTS_WORLDMATRIX(0), &m_PosMtx, TRUE );
						fdx8xfm_SetRenormalizeNormalsIfNeeded( m_fMatrixScale );
					} 
					else if ( pSeg->nBoneMtxCount == 1 ) 
					{
						// This is a non-skinned segment...

						fdx8_SetRenderState_VERTEXBLEND( D3DVBF_DISABLE );

						if ( !(m_nFlags & FMESHINST_FLAG_NOBONES) ) 
						{
							#if FMESH_ALLOW_POSTER_BONES
							if ( !bPosterBones )
							{
								fdx8xfm_SetCustomDXMatrix( D3DTS_WORLDMATRIX(0), m_apBoneMtxPalette[ pSeg->anBoneMtxIndex[0] ], TRUE );
							} 
							else
							{
								Mtx43A.Mul33( PosterMtx, *m_apBoneMtxPalette[ pSeg->anBoneMtxIndex[0] ] );
								Mtx43A.m_vPos = m_apBoneMtxPalette[ pSeg->anBoneMtxIndex[0] ]->m_vPos;
								fdx8xfm_SetCustomDXMatrix( D3DTS_WORLDMATRIX(0), &Mtx43A, TRUE );
							}
							#else
							fdx8xfm_SetCustomDXMatrix( D3DTS_WORLDMATRIX(0), m_apBoneMtxPalette[ pSeg->anBoneMtxIndex[0] ], TRUE );
							#endif
							fdx8xfm_SetRenormalizeNormalsIfNeeded( m_apBoneMtxPalette[ pSeg->anBoneMtxIndex[0] ] );
						} 
						else 
						{
							// The FMESHINST_FLAG_NOBONES flag is set and this is a non-skinned segment.
							// This means that the vertices have already been transformed into bone space,
							// and we need to transform them back into at-rest model space...
							Mtx43A.Mul( m_Xfm.m_MtxF, m_pMesh->pBoneArray[ pSeg->anBoneMtxIndex[0] ].AtRestBoneToModelMtx );
							fdx8xfm_SetCustomDXMatrix( D3DTS_WORLDMATRIX(0), &Mtx43A, TRUE );
							fdx8xfm_SetRenormalizeNormalsIfNeeded( &Mtx43A );
						}
					} 
					else 
					{
						// This is a skinned segment...

						if ( !(m_nFlags & FMESHINST_FLAG_NOBONES) ) 
						{
							fdx8_SetRenderState_VERTEXBLEND( _aD3DVertexBlendFlags[pSeg->nBoneMtxCount] );

							for( nBoneMtxNum=0; nBoneMtxNum<pSeg->nBoneMtxCount; nBoneMtxNum++ ) 
							{
								pMtx43A = m_apBoneMtxPalette[ pSeg->anBoneMtxIndex[nBoneMtxNum] ];
								D3DXMatrixTranspose(&boneMtx, (const D3DXMATRIX *)pMtx43A);
								FDX8_pDev->SetVertexShaderConstant(CV_BONEMTX0_0 + 3*nBoneMtxNum, &boneMtx(0, 0), 3);
							}
						} 
						else 
						{
							fdx8_SetRenderState_VERTEXBLEND( D3DVBF_DISABLE );
							fdx8xfm_SetCustomDXMatrix( D3DTS_WORLDMATRIX(0), &m_PosMtx, TRUE );
							fdx8xfm_SetRenormalizeNormalsIfNeeded( m_fMatrixScale );
						}
					}
				}
			}

			// Render the tri-list...
			if ( pCluster->TriList.nTriCount )
			{
				if (FAILED(FDX8_pDev->DrawIndexedPrimitive(
					D3DPT_TRIANGLELIST,
					pCluster->TriList.nVtxIndexMin,
					pCluster->TriList.nVtxIndexRange,
					pCluster->TriList.nStartVindex,
					pCluster->TriList.nTriCount
				)))
				{
					_TEMPDipFail("tri", pCluster->TriList.nVtxIndexMin, pCluster->TriList.nVtxIndexRange, pCluster->TriList.nStartVindex, pCluster->TriList.nTriCount);
				}

				#if FPERF_ENABLE
					FPerf_nRawTriListCount++;
					FPerf_nRawListTriCount += pCluster->TriList.nTriCount;
					FPerf_nRawVertexCount += (pCluster->TriList.nTriCount<<1) + pCluster->TriList.nTriCount;
				#endif
			}

			// Render the strip-list...
			FDX8MeshStrip_t *pStrip;
			for ( nStrip=0, pStrip = pCluster->paStripBuffer; nStrip < pCluster->nStripCount; nStrip++, pStrip++ ) 
			{
				if (FAILED(FDX8_pDev->DrawIndexedPrimitive(
					D3DPT_TRIANGLESTRIP,
					pStrip->nVtxIndexMin,
					pStrip->nVtxIndexRange,
					pStrip->nStartVindex,
					pStrip->nTriCount
				)))
				{
					_TEMPDipFail("strip", pStrip->nVtxIndexMin, pStrip->nVtxIndexRange, pStrip->nStartVindex, pStrip->nTriCount);
				}

				#if FPERF_ENABLE
					FPerf_nRawTriStripCount++;
					FPerf_nRawStripTriCount += pStrip->nTriCount;
					FPerf_nRawVertexCount += pStrip->nTriCount + 2;
				#endif
			}
		}
	}

	fsh_EndExecute();

	if ( m_pMesh->nTexLayerIDCount ) 
	{
		// Reset the lists if there were texture layer effects...
		fsh_TexCoordMtx_ResetList();
		fsh_TexOverride_ResetList();
	}

	//TEMP(R46): translucent path decode - which materials (incl. liquid surf=38-42) draw here, with device state.
	//R46b: keyed per-material (not per-mesh) so the liquid material's translucent
	//draw logs separately from its siblings.
	_TEMPMDLog( "transl", 'T', pMaterial, m_pMesh->szName, this, 0, 0, 0, 0, 0, 0, 0, m_pMesh->nBoneCount, m_nDrawnPartsMask, nLODIndex, m_fMatrixScale, m_PosMtx.m_vP.x, m_PosMtx.m_vP.y, m_PosMtx.m_vP.z, (m_pMesh->nFlags & FMESH_FLAGS_VOLUME_MESH) ? 1 : 0, m_nFlags, _TEMPGetPal( m_pMesh->nBoneCount, m_nFlags, m_PosMtx, m_apBoneMtxPalette, NULL ), (s32)(pMaterial - m_pMesh->aMtl) );

#if FPERF_ENABLE
	u32 nTrisRendered = (FPerf_nRawStripTriCount - nPreStripCount) + (FPerf_nRawListTriCount - nPreListCount);
	FPerf_nTotalTrisTranslucent += nTrisRendered;
	FPerf_nMeshTris += nTrisRendered / nTotalPasses;
#endif
}


//
//
//
void CFMeshInst::DrawAllMaterials_P( FViewportPlanesMask_t nCrossesPlanesMask ) 
{
	FASSERT( _bWindowCreated );
	FASSERT( nCrossesPlanesMask != -1 );

	CFMtx43A Mtx43A, *pMtx43A;
	u32 i, nBoneMtxNum, nPass, nStrip, nPassCount = 0;

	#if FPERF_ENABLE
		u32 nPreStripCount = FPerf_nRawStripTriCount;
		u32 nPreListCount = FPerf_nRawListTriCount;
	#endif

	#if FANG_PLATFORM_WIN
		fdx8_SetRenderState_CLIPPING( nCrossesPlanesMask != 0 );
	#endif

	fdx8_SetRenderState_FILLMODE( FRenderer_nD3DFillMode );

	_SetCullDir_P( (FMeshCullDir_e)m_nCullDirection );

	FDX8Mesh_t *pMeshIS = m_pMesh->pMeshIS;
	u32 nMeshBoneCount = m_pMesh->nBoneCount;

	if ( nMeshBoneCount == 0 ) 
	{
		// There are no bones in the entire mesh. We can just set a single model matrix...
		fdx8xfm_SetViewDXMatrix( TRUE );
		fdx8xfm_SetCustomDXMatrix( D3DTS_WORLDMATRIX(0), &m_PosMtx, TRUE );
		fdx8_SetRenderState_VERTEXBLEND( D3DVBF_DISABLE );
		fdx8xfm_SetRenormalizeNormalsIfNeeded( m_fMatrixScale );
		// There are no bones in the entire mesh. We can just set a single model matrix...
//		fdx8xfm_SetDXMatrices( TRUE );
//		fdx8_SetRenderState_VERTEXBLEND( D3DVBF_DISABLE );
//		fdx8xfm_SetRenormalizeNormalsIfNeeded( FXfm_pModel->m_fScaleF );
	} 
	else 
	{
		// This mesh has bones. Set only the view matrix because
		// the model matrix/matrices must be computed for each segment...
		fdx8xfm_SetViewDXMatrix( TRUE );
	}

	if ( !FMesh_bRenderShadows )
	{
		AddShaderLights();
	}

	// Check if texture layer animations exist on this object...
	if ( !m_pMesh->nTexLayerIDCount ) 
	{
		// No texture layer animation on this object...
		fsh_TexCoordMtx_ResetList();
		fsh_TexOverride_ResetList();
	}

	// Determine the LOD we should be using
//	f32 fDistToCamSq = FXfm_pModel->m_MtxF.m_vPos.Sub( FXfm_pView->m_MtxR.m_vPos ).MagSq();
	f32 fDistToCamSq = m_PosMtx.m_vPos.Sub( FXfm_pView->m_MtxR.m_vPos ).MagSq();
	u8 nLODMask, nLODToUse = 0;
	if ( fDistToCamSq > 2.f && fDistToCamSq < 10000000.f )
	{
		fDistToCamSq = fmath_Sqrt( fDistToCamSq ) - m_pMesh->BoundSphere_MS.m_fRadius;
		for ( i = 1; i < m_pMesh->nLODCount ; i++ )
		{
			if ( fDistToCamSq < m_pMesh->afLODDistance[i] )
			{
				break;
			}
			nLODToUse++;
		}
	}

	if ( FMesh_bRenderShadows )
	{
		nLODToUse += m_pMesh->nShadowLODBias;
		if ( nLODToUse > m_pMesh->nLODCount - 1 )
		{
			nLODToUse = m_pMesh->nLODCount - 1;
		}
	}
	else
	{
		m_nCurrentLOD = nLODToUse;
	}

#if !FANG_PRODUCTION_BUILD
	if ( FMesh_nForceLOD != -1 )
	{
		nLODToUse = FMesh_nForceLOD;
	}
#endif
	nLODMask = 1 << nLODToUse;

	// Setup color stream management
	s32 nColorStreamVBIndex = -1;
	if ( m_papColorStreams )
	{
		FDX8_pDev->SetStreamSource( 3, ((IDirect3DVertexBuffer8 **)m_papColorStreams)[0], sizeof(DWORD) );
		fdx8sh_ToggleExternalColorStream( TRUE );
		_bStream3Active = TRUE;
	}
	else if ( _bStream3Active )
	{
		FDX8_pDev->SetStreamSource( 3, NULL, 0 );
		fdx8sh_ToggleExternalColorStream( FALSE );
		_bStream3Active = FALSE;
	}

	// Advance through all of the materials, drawing the relevant materials and clusters
	s32 nCurrentIB = -1;
#if FANG_DEBUG_BUILD
	FDX8VB_t *pVB = NULL;
#endif
	FMeshSeg_t *pSeg = NULL;
	FMeshSeg_t *aSeg = m_pMesh->aSeg;
	FMeshMaterial_t *pMtl = m_pMesh->aMtl;
	FMeshMaterial_t *pEndMtl = &m_pMesh->aMtl[m_pMesh->nMaterialCount];

#if FMESH_ALLOW_POSTER_BONES
	BOOL bPosterBones = FALSE;
	CFMtx43A PosterMtx;

	if ( (m_nFlags & (FMESHINST_FLAG_POSTER_X | FMESHINST_FLAG_POSTER_Y | FMESHINST_FLAG_POSTER_Z)) && (m_nFlags & FMESHINST_FLAG_POSTER_BONES) )
	{
		if ( m_pMesh->nBoneCount )
		{
			bPosterBones = TRUE;
			PosterMtx.Mul( m_PosMtx, m_Xfm.m_MtxR );
		}
	}
#endif

	for ( ;pMtl < pEndMtl; pMtl++ )
	{
		#if _SUPPORTING_PARTS_DRAWING
			if ( !(pMtl->nPartIDMask & m_nDrawnPartsMask) )
			{
				continue;
			}
		#endif

		if ( !(pMtl->nLODMask & nLODMask) )
		{
			continue;
		}

		if ( !SetupShaderParameters( pMtl, FSHADERS_PASS_LIGHTING ) )
		{
			continue;
		}

		// Change the shader type, if necessary and supported, based on the mesh alpha
		u32 nSurfaceShaderIdx = pMtl->nSurfaceShaderIdx;
		if ( nSurfaceShaderIdx == FSHADERS_oBASE && GetMeshTintAndAlpha()->fAlpha < 0.999f )
		{
			nSurfaceShaderIdx = FSHADERS_otBASE;
		}

		if ( nSurfaceShaderIdx == FSHADERS_cBASE && GetMeshTintAndAlpha()->fAlpha < 0.999f )
		{
			nSurfaceShaderIdx = FSHADERS_tBASE;
		}

		// Determine the number of necessary passes
		u32 nDiffusePasses = fsh_GetNumDiffusePasses( nSurfaceShaderIdx, pMtl->nLightShaderIdx );
		nPassCount = nDiffusePasses + 1 + (pMtl->nSpecularShaderIdx != FSH_INVALID_SPECULAR_SHADER);
		if ( FMesh_bRenderShadows )
		{
			nPassCount = 1;
			nDiffusePasses = 1;
		}

		// Render the material's clusters once per required pass
		for ( nPass = 0; nPass < nPassCount; nPass++ )
		{
			if ( nPass < nDiffusePasses )
			{
				fsh_SetShaderType( SHADERTYPE_DIFFUSE );

				if ( !fsh_SetShader( nSurfaceShaderIdx, pMtl->nLightShaderIdx, pMtl->nSpecularShaderIdx, nPass ) ) 
				{
					continue;
				}
				fsh_SetReflectTex( NULL );
			}
			else if ( nPass == nDiffusePasses )
			{
				fsh_SetShaderType( SHADERTYPE_SURFACE ); //diffuse, surface, specular or translucency

				if ( !SetupShaderParameters( pMtl, FSHADERS_PASS_SURFACE ) )
				{
					continue;
				}

				if ( !fsh_SetShader( nSurfaceShaderIdx, pMtl->nLightShaderIdx, pMtl->nSpecularShaderIdx ) )
				{
					continue;
				}
				fsh_SetReflectTex( m_pReflectTex );
			}
			else
			{
				fsh_SetShaderType( SHADERTYPE_SPECULAR ); //diffuse, surface, specular or translucency

				if ( !SetupShaderParameters( pMtl, FSHADERS_PASS_SURFACE ) )
				{
					SetupShaderParameters( pMtl, FSHADERS_PASS_SPECULAR );
				}

				if ( !fsh_SetShader( nSurfaceShaderIdx, pMtl->nLightShaderIdx, pMtl->nSpecularShaderIdx ) )
				{
					continue;
				}
			}

			// Setup the DX states for this shader in preparation for rendering
			fsh_ExecuteCurrent( FMesh_bRenderShadows, 0x01 );

			// Submit all relevant clusters for rendering
			FDX8MeshCluster_t *pCluster = ((FDX8MeshMaterial_t *)pMtl->pPlatformData)->aCluster;
			FDX8MeshCluster_t *pEndCluster = &pCluster[((FDX8MeshMaterial_t *)pMtl->pPlatformData)->nClusterCount];
			for ( ;pCluster < pEndCluster; pCluster++ )
			{
				if ( !(m_nDrawnPartsMask & (1 << pCluster->nPartID)) )
				{
					continue;
				}

				if ( pCluster->nLODID != nLODToUse )
				{
					continue;
				}

				// Setup the vertex buffers
				BOOL bUseLightmaps = (FSh_shaderType == SHADERTYPE_DIFFUSE || FSh_shaderType == SHADERTYPE_TRANSLUCENCY);
				fdx8vb_Select( &pMeshIS->aVB[ pCluster->nVBIndex ], FALSE, bUseLightmaps, TRUE );
				fsh_CheckVB();

				// Setup the color streams
				if ( m_papColorStreams && nColorStreamVBIndex != pCluster->nVBIndex )
				{
					FDX8_pDev->SetStreamSource( 3, ((IDirect3DVertexBuffer8 **)m_papColorStreams)[pCluster->nVBIndex], sizeof(DWORD) );
					nColorStreamVBIndex = pCluster->nVBIndex;
					_bStream3Active = TRUE;
				}

				// Setup the index buffer
				if ( nCurrentIB != pCluster->nIBIndex )
				{
					nCurrentIB = pCluster->nIBIndex;
					FDX8_pDev->SetIndices( (IDirect3DIndexBuffer8 *)pMeshIS->apDXIB[nCurrentIB], 0 );
				}

				D3DXMATRIX boneMtx;

				if ( nMeshBoneCount ) 
				{
					FASSERT( pCluster->nSegmentIdx < m_pMesh->nSegCount );

					if ( pSeg != &aSeg[pCluster->nSegmentIdx] )
					{
						pSeg = &aSeg[pCluster->nSegmentIdx];

						if ( pSeg->nBoneMtxCount == 0 )
						{
							// No bones for this segment, so just set this mesh instance's xfm
							// as the model matrix...
							fdx8_SetRenderState_VERTEXBLEND( D3DVBF_DISABLE );
							fdx8xfm_SetModelDXMatrix( TRUE );
							fdx8xfm_SetRenormalizeNormalsIfNeeded( m_fMatrixScale );
						} 
						else if ( pSeg->nBoneMtxCount == 1 ) 
						{
							// This is a non-skinned segment...

							fdx8_SetRenderState_VERTEXBLEND( D3DVBF_DISABLE );

							if ( !(m_nFlags & FMESHINST_FLAG_NOBONES) ) 
							{
								#if FMESH_ALLOW_POSTER_BONES
								if ( !bPosterBones )
								{
									fdx8xfm_SetCustomDXMatrix( D3DTS_WORLDMATRIX(0), m_apBoneMtxPalette[ pSeg->anBoneMtxIndex[0] ], TRUE );
								} 
								else
								{
									Mtx43A.Mul33( PosterMtx, *m_apBoneMtxPalette[ pSeg->anBoneMtxIndex[0] ] );
									Mtx43A.m_vPos = m_apBoneMtxPalette[ pSeg->anBoneMtxIndex[0] ]->m_vPos;
									fdx8xfm_SetCustomDXMatrix( D3DTS_WORLDMATRIX(0), &Mtx43A, TRUE );
								}
								#else
								fdx8xfm_SetCustomDXMatrix( D3DTS_WORLDMATRIX(0), m_apBoneMtxPalette[ pSeg->anBoneMtxIndex[0] ], TRUE );
								#endif
								fdx8xfm_SetRenormalizeNormalsIfNeeded( m_apBoneMtxPalette[ pSeg->anBoneMtxIndex[0] ] );
							} 
							else 
							{
								// The FMESHINST_FLAG_NOBONES flag is set and this is a non-skinned segment.
								// This means that the vertices have already been transformed into bone space,
								// and we need to transform them back into at-rest model space...
								Mtx43A.Mul( m_PosMtx, m_pMesh->pBoneArray[ pSeg->anBoneMtxIndex[0] ].AtRestBoneToModelMtx );
								fdx8xfm_SetCustomDXMatrix( D3DTS_WORLDMATRIX(0), &Mtx43A, TRUE );
								fdx8xfm_SetRenormalizeNormalsIfNeeded( &Mtx43A );
							}
						} 
						else 
						{
							// This is a skinned segment...

							if ( !(m_nFlags & FMESHINST_FLAG_NOBONES) ) 
							{
								fdx8_SetRenderState_VERTEXBLEND( _aD3DVertexBlendFlags[pSeg->nBoneMtxCount] );

								for( nBoneMtxNum=0; nBoneMtxNum<pSeg->nBoneMtxCount; nBoneMtxNum++ ) 
								{
									pMtx43A = m_apBoneMtxPalette[ pSeg->anBoneMtxIndex[nBoneMtxNum] ];
									D3DXMatrixTranspose( &boneMtx, (const D3DXMATRIX *)pMtx43A );
									FDX8_pDev->SetVertexShaderConstant( CV_BONEMTX0_0 + 3 * nBoneMtxNum, &boneMtx(0, 0), 3 );
								}
							} 
							else 
							{
								fdx8_SetRenderState_VERTEXBLEND( D3DVBF_DISABLE );
								fdx8xfm_SetModelDXMatrix( TRUE );
								fdx8xfm_SetRenormalizeNormalsIfNeeded( m_fMatrixScale );
							}
						}
					}
				}

				// Render the tri-list...
				if ( pCluster->TriList.nTriCount )
				{
					if ( FAILED(FDX8_pDev->DrawIndexedPrimitive(
						D3DPT_TRIANGLELIST,
						pCluster->TriList.nVtxIndexMin,
						pCluster->TriList.nVtxIndexRange,
						pCluster->TriList.nStartVindex,
						pCluster->TriList.nTriCount
					)) )
					{
						_TEMPDipFail("tri", pCluster->TriList.nVtxIndexMin, pCluster->TriList.nVtxIndexRange, pCluster->TriList.nStartVindex, pCluster->TriList.nTriCount);
					}

					#if FPERF_ENABLE
						FPerf_nRawTriListCount++;
						FPerf_nRawListTriCount += pCluster->TriList.nTriCount;
						FPerf_nRawVertexCount += (pCluster->TriList.nTriCount<<1) + pCluster->TriList.nTriCount;
					#endif
				}

				// Render the strip-list...
				FDX8MeshStrip_t *pStrip;
				for ( nStrip=0, pStrip = pCluster->paStripBuffer; nStrip < pCluster->nStripCount; nStrip++, pStrip++ ) 
				{
					if ( FAILED(FDX8_pDev->DrawIndexedPrimitive(
						D3DPT_TRIANGLESTRIP,
						pStrip->nVtxIndexMin,
						pStrip->nVtxIndexRange,
						pStrip->nStartVindex,
						pStrip->nTriCount
					)) )
					{
						_TEMPDipFail("strip", pStrip->nVtxIndexMin, pStrip->nVtxIndexRange, pStrip->nStartVindex, pStrip->nTriCount);
					}

					#if FPERF_ENABLE
						FPerf_nRawTriStripCount++;
						FPerf_nRawStripTriCount += pStrip->nTriCount;
						FPerf_nRawVertexCount += pStrip->nTriCount + 2;
					#endif
				}
			}
		}
	}

	fsh_EndExecute();

	if ( m_pMesh->nTexLayerIDCount )
	{
		fsh_TexCoordMtx_ResetList();
		fsh_TexOverride_ResetList();
	}

	#if FPERF_ENABLE
		if ( nPassCount )
		{
			u32 nTrisRendered = (FPerf_nRawStripTriCount - nPreStripCount) + (FPerf_nRawListTriCount - nPreListCount);
			if ( FMesh_bRenderShadows )
			{
				FPerf_nMeshShadowTris += nTrisRendered / nPassCount;
			}
			else
			{
				FPerf_nMeshTris += nTrisRendered / nPassCount;
			}
		}
	#endif
}

//
//
//
void CFMeshInst::DrawShadow_P( FViewportPlanesMask_t nCrossesPlanesMask, FMeshMaterial_t *pMaterial, u32 nLODIndex )
{
	FASSERT( _bWindowCreated );
	FASSERT( nCrossesPlanesMask != -1 );

	CFMtx43A Mtx43A, *pMtx43A;
	u32 nBoneMtxNum, nStrip;

	if ( m_fShadowIntensity < 0.01f )
	{
		return;
	}

	#if _SUPPORTING_PARTS_DRAWING
		if ( !(pMaterial->nPartIDMask & m_nDrawnPartsMask) )
		{
			return;
		}
	#endif

	FDX8Mesh_t *pMeshIS = m_pMesh->pMeshIS;
	u32 nMeshBoneCount = m_pMesh->nBoneCount;

	#if FANG_PLATFORM_WIN
		fdx8_SetRenderState_CLIPPING( nCrossesPlanesMask != 0 );
	#endif

	fdx8_SetRenderState_FILLMODE( FRenderer_nD3DFillMode );

	_SetCullDir_P( (FMeshCullDir_e)m_nCullDirection );

	if ( !(m_pMesh->nFlags & FMESH_FLAGS_VOLUME_MESH) )
	{
		if ( nMeshBoneCount == 0 ) 
		{
			// There are no bones in the entire mesh. We can just set a single model matrix...
			fdx8xfm_SetViewDXMatrix( TRUE );
			fdx8xfm_SetCustomDXMatrix( D3DTS_WORLDMATRIX(0), &m_PosMtx, TRUE );
			fdx8_SetRenderState_VERTEXBLEND( D3DVBF_DISABLE );
			fdx8xfm_SetRenormalizeNormalsIfNeeded( m_fMatrixScale );
		} 
		else 
		{
			// This mesh has bones. Set only the view matrix because
			// the model matrix/matrices must be computed for each segment...
			fdx8xfm_SetViewDXMatrix( TRUE );
		}
	}

	// Setup any material specific depth bias
	fsh_SetDepthBias( pMaterial->nDepthBiasLevel );

	s32 nTotalPasses = fshadow_GetNumShadowPasses(m_pRenderLights, m_nRenderLightCount, this, m_pMesh->nFlags&FMESH_FLAGS_VOLUME_MESH);

#if FANG_DEBUG_BUILD
	FDX8VB_t *pVB = NULL;
#endif
	s32 nPass, nCurrentIB = -1;
	FMeshSeg_t *pSeg = NULL;
	FMeshSeg_t *aSeg = m_pMesh->aSeg;
	s32 nColorStreamVBIndex = -1;
	if ( _bStream3Active )
	{
		FDX8_pDev->SetStreamSource( 3, NULL, 0 );
		fdx8sh_ToggleExternalColorStream( FALSE );
	}

	if (nTotalPasses < 1) 
	{ 
		return; 
	}

#if FMESH_ALLOW_POSTER_BONES
	BOOL bPosterBones = FALSE;
	CFMtx43A PosterMtx;

	if ( (m_nFlags & (FMESHINST_FLAG_POSTER_X | FMESHINST_FLAG_POSTER_Y | FMESHINST_FLAG_POSTER_Z)) && (m_nFlags & FMESHINST_FLAG_POSTER_BONES) )
	{
		if ( m_pMesh->nBoneCount )
		{
			bPosterBones = TRUE;
			PosterMtx.Mul( m_PosMtx, m_Xfm.m_MtxR );
		}
	}
#endif
	for (nPass = 0; nPass < nTotalPasses; nPass++)
	{
		BOOL bDirectionalOnly = fshadow_SetupShadowPass(nPass, m_pMesh->nFlags&FMESH_FLAGS_VOLUME_MESH);
		
		FDX8MeshCluster_t *pCluster = ((FDX8MeshMaterial_t *)pMaterial->pPlatformData)->aCluster;
		FDX8MeshCluster_t *pEndCluster = &pCluster[((FDX8MeshMaterial_t *)pMaterial->pPlatformData)->nClusterCount];
		for ( ;pCluster < pEndCluster; pCluster++ )
		{
			if ( !(m_nDrawnPartsMask & (1 << pCluster->nPartID)) )
			{
				continue;
			}

			if ( pCluster->nLODID != nLODIndex )
			{
				continue;
			}

			if ( bDirectionalOnly && (pCluster->nFlags & CLUSTER_FACING_OPP_DIR_LIGHT) )
			{
				continue;
			}

			fdx8vb_Select( &pMeshIS->aVB[ pCluster->nVBIndex ], FALSE, FALSE, FALSE );

			if ( nCurrentIB != pCluster->nIBIndex )
			{
				nCurrentIB = pCluster->nIBIndex;
				FDX8_pDev->SetIndices( (IDirect3DIndexBuffer8 *)pMeshIS->apDXIB[nCurrentIB], 0 );
			}

			D3DXMATRIX boneMtx;

			if ( nMeshBoneCount ) 
			{
				FASSERT( pCluster->nSegmentIdx < m_pMesh->nSegCount );

				if ( pSeg != &aSeg[pCluster->nSegmentIdx] )
				{
					pSeg = &aSeg[pCluster->nSegmentIdx];

					if ( pSeg->nBoneMtxCount == 0 )
					{
						// No bones for this segment, so just set this mesh instance's xfm
						// as the model matrix...
						fdx8_SetRenderState_VERTEXBLEND( D3DVBF_DISABLE );
						fdx8xfm_SetCustomDXMatrix( D3DTS_WORLDMATRIX(0), &m_PosMtx, TRUE );
						fdx8xfm_SetRenormalizeNormalsIfNeeded( m_fMatrixScale );
					} 
					else if ( pSeg->nBoneMtxCount == 1 ) 
					{
						// This is a non-skinned segment...

						fdx8_SetRenderState_VERTEXBLEND( D3DVBF_DISABLE );

						if ( !(m_nFlags & FMESHINST_FLAG_NOBONES) ) 
						{
							#if FMESH_ALLOW_POSTER_BONES
							if ( !bPosterBones )
							{
								fdx8xfm_SetCustomDXMatrix( D3DTS_WORLDMATRIX(0), m_apBoneMtxPalette[ pSeg->anBoneMtxIndex[0] ], TRUE );
							} 
							else
							{
								Mtx43A.Mul33( PosterMtx, *m_apBoneMtxPalette[ pSeg->anBoneMtxIndex[0] ] );
								Mtx43A.m_vPos = m_apBoneMtxPalette[ pSeg->anBoneMtxIndex[0] ]->m_vPos;
								fdx8xfm_SetCustomDXMatrix( D3DTS_WORLDMATRIX(0), &Mtx43A, TRUE );
							}
							#else
							fdx8xfm_SetCustomDXMatrix( D3DTS_WORLDMATRIX(0), m_apBoneMtxPalette[ pSeg->anBoneMtxIndex[0] ], TRUE );
							#endif
							fdx8xfm_SetRenormalizeNormalsIfNeeded( m_apBoneMtxPalette[ pSeg->anBoneMtxIndex[0] ] );
						} 
						else 
						{
							// The FMESHINST_FLAG_NOBONES flag is set and this is a non-skinned segment.
							// This means that the vertices have already been transformed into bone space,
							// and we need to transform them back into at-rest model space...
							Mtx43A.Mul( m_PosMtx, m_pMesh->pBoneArray[ pSeg->anBoneMtxIndex[0] ].AtRestBoneToModelMtx );
							fdx8xfm_SetCustomDXMatrix( D3DTS_WORLDMATRIX(0), &Mtx43A, TRUE );
							fdx8xfm_SetRenormalizeNormalsIfNeeded( &Mtx43A );
						}
					} 
					else 
					{
						// This is a skinned segment...

						if ( !(m_nFlags & FMESHINST_FLAG_NOBONES) ) 
						{
							fdx8_SetRenderState_VERTEXBLEND( _aD3DVertexBlendFlags[pSeg->nBoneMtxCount] );

							for( nBoneMtxNum=0; nBoneMtxNum<pSeg->nBoneMtxCount; nBoneMtxNum++ ) 
							{
								pMtx43A = m_apBoneMtxPalette[ pSeg->anBoneMtxIndex[nBoneMtxNum] ];
								D3DXMatrixTranspose(&boneMtx, (const D3DXMATRIX *)pMtx43A);
								FDX8_pDev->SetVertexShaderConstant(CV_BONEMTX0_0 + 3*nBoneMtxNum, &boneMtx(0, 0), 3);
							}
						} 
						else 
						{
							fdx8_SetRenderState_VERTEXBLEND( D3DVBF_DISABLE );
							fdx8xfm_SetCustomDXMatrix( D3DTS_WORLDMATRIX(0), &m_PosMtx, TRUE );
							fdx8xfm_SetRenormalizeNormalsIfNeeded( m_fMatrixScale );
						}
					}
				}
			}

			// Render the tri-list...
			if ( pCluster->TriList.nTriCount )
			{
				if (FAILED(FDX8_pDev->DrawIndexedPrimitive(
					D3DPT_TRIANGLELIST,
					pCluster->TriList.nVtxIndexMin,
					pCluster->TriList.nVtxIndexRange,
					pCluster->TriList.nStartVindex,
					pCluster->TriList.nTriCount
				)))
				{
					_TEMPDipFail("tri", pCluster->TriList.nVtxIndexMin, pCluster->TriList.nVtxIndexRange, pCluster->TriList.nStartVindex, pCluster->TriList.nTriCount);
				}

				#if FPERF_ENABLE
					FPerf_nRawTriListCount++;
					FPerf_nRawListTriCount += pCluster->TriList.nTriCount;
					FPerf_nRawVertexCount += (pCluster->TriList.nTriCount<<1) + pCluster->TriList.nTriCount;
					FPerf_nTotalTrisShadow += pCluster->TriList.nTriCount;
				#endif
			}

			// Render the strip-list...
			FDX8MeshStrip_t *pStrip;
			for ( nStrip=0, pStrip = pCluster->paStripBuffer; nStrip < pCluster->nStripCount; nStrip++, pStrip++ ) 
			{
				if (FAILED(FDX8_pDev->DrawIndexedPrimitive(
					D3DPT_TRIANGLESTRIP,
					pStrip->nVtxIndexMin,
					pStrip->nVtxIndexRange,
					pStrip->nStartVindex,
					pStrip->nTriCount
				)))
				{
					_TEMPDipFail("strip", pStrip->nVtxIndexMin, pStrip->nVtxIndexRange, pStrip->nStartVindex, pStrip->nTriCount);
				}

				#if FPERF_ENABLE
					FPerf_nRawTriStripCount++;
					FPerf_nRawStripTriCount += pStrip->nTriCount;
					FPerf_nRawVertexCount += pStrip->nTriCount + 2;
					FPerf_nTotalTrisShadow += pStrip->nTriCount;
				#endif
			}
		}
	}

//	fdx8shadow_ClearTextures();
}


//
//
//
void CFMeshInst::AddShaderLights( void ) 
{
	u32 i;
	BOOL bLightPerPixel = (m_pMesh->nFlags & FMESH_FLAGS_VOLUME_MESH) || (m_nFlags & FMESHINST_FLAG_LIGHT_PER_PIXEL);

	fsh_Light_ResetList();

	//Add Spotlights
	for ( i = 0; i < m_nRenderLightCount; i++ ) 
	{
		if (m_pRenderLights[i].pLight->m_nType == FLIGHT_TYPE_SPOT)
		{
			fsh_Light_Add( m_pRenderLights[i].pLight, bLightPerPixel );
		}
	}
	//Add Everything else
	for ( i = 0; i < m_nRenderLightCount; i++ ) 
	{
		if (m_pRenderLights[i].pLight->m_nType != FLIGHT_TYPE_SPOT)
		{
			fsh_Light_Add( m_pRenderLights[i].pLight, bLightPerPixel );
		}
	}

	if ( !(m_nFlags & (FMESHINST_FLAG_NOLIGHT_AMBIENT | FMESHINST_FLAG_LM | FMESHINST_FLAG_VERT_RADIOSITY)) ) 
	{
		fsh_SetGlobalRegister( FSHREG_AMBIENT, (u32)&FMesh_AmbientMotif );
	} 
	else 
	{
		fsh_SetGlobalRegister( FSHREG_AMBIENT, (u32)&FColor_MotifBlack );
	}

	//R48: per-instance dynamic-light collection health. The vertex-lit prop
	//class (elevator slats, cables, door fences) renders black while the
	//lightmapped world draws fine -- log which lights this instance actually
	//collected (m_pRenderLights, filled by fvis _DL_AddMesh) plus the ambient
	//motif uploaded with it. Keyed by instance pointer, once per 600f.
	if ( !_TEMPMDSeen( this, 'L' ) )
	{
		BOOL bAmb = !(m_nFlags & (FMESHINST_FLAG_NOLIGHT_AMBIENT | FMESHINST_FLAG_LM | FMESHINST_FLAG_VERT_RADIOSITY));
		DEVPRINTF( "[ma] TEMP shlights inst=%p name=%s rlc=%u amb=%d ambcol=(%g,%g,%g)\r\n",
			this, m_pMesh ? m_pMesh->szName : "?", (unsigned)m_nRenderLightCount, bAmb ? 1 : 0,
			(float)FMesh_AmbientMotif.ColorRGB.fRed, (float)FMesh_AmbientMotif.ColorRGB.fGreen, (float)FMesh_AmbientMotif.ColorRGB.fBlue );
		//R49: cap raised 4->8 (review round: cable draws selected POINT6
		//after collecting six lights; slots 4+ were never dumped) and each
		//entry now reports m_fOOR_WS -- the inverse range _BuildD3DLight
		//multiplies the attenuation K1/K2 constants by.
		for ( i = 0; i < m_nRenderLightCount && i < 8; i++ )
		{
			CFLight *pTempL = m_pRenderLights[i].pLight;
			if ( !pTempL ) { continue; }
			pTempL->ComputeColor();
			DEVPRINTF( "[ma] TEMP shlight1 inst=%p i=%u type=%d oor=%g p=(%g,%g,%g) c=(%g,%g,%g)\r\n",
				this, (unsigned)i, (int)pTempL->m_nType, (float)pTempL->m_fOOR_WS,
				(float)pTempL->m_spInfluence_WS.m_Pos.x, (float)pTempL->m_spInfluence_WS.m_Pos.y, (float)pTempL->m_spInfluence_WS.m_Pos.z,
				(float)pTempL->m_ScaledColor.fRed, (float)pTempL->m_ScaledColor.fGreen, (float)pTempL->m_ScaledColor.fBlue );
		}
	}
}


//
//
//
static BOOL _WindowCreatedCallback( FDX8VidEvent_e nEvent ) 
{
	FASSERT( _bModuleInitialized );
	FASSERT( nEvent>=0 && nEvent<FDX8VID_EVENT_COUNT );

	switch( nEvent ) 
	{
		case FDX8VID_EVENT_WINDOW_CREATED:
			_bWindowCreated = TRUE;

			FMesh_AmbientMotif.nMotifIndex = 0;
			FMesh_AmbientMotif.fAlpha = 1.0f;
			fmesh_Ambient_Set( 1.0f, 1.0f, 1.0f, 1.0f );

			break;

		case FDX8VID_EVENT_WINDOW_DESTROYED:
			_bWindowCreated = FALSE;
			break;

		case FDX8VID_EVENT_PRE_RESET:
			break;

		case FDX8VID_EVENT_POST_RESET:
			break;
	}

	return TRUE;
}


//
//
//
static BOOL _ResLoadCreate( FResHandle_t hRes, void *pLoadedBase, u32 nLoadedBytes, cchar *pszResName ) 
{
	FMesh_t *pMesh;

	// Create the runtime mesh data...
	pMesh = (FMesh_t *)fdx8load_Create( (FMesh_t *)pLoadedBase, pszResName );
	if ( pMesh == NULL ) 
	{
		// Could not create mesh...
		return FALSE;
	}

#if FHEAP_TRACK_MEM_ALLOCATIONS		
	if ( pMesh->pMeshIS )
	{
		u32 i;
		if ( pMesh->pMeshIS->aVB )
		{
			u32 nVBCount = pMesh->pMeshIS->nVBCount;
			FDX8VB_t *aVB = pMesh->pMeshIS->aVB;
			u32 nVBMem = 0;
			for ( i = 0; i < nVBCount; i++ )
			{
				nVBMem += (aVB[i].nVtxCount * aVB[i].nBytesPerVertex);
			}
			Fheap_nTotalMeshMemTracked += nVBMem;
		}

		for ( i = 0; i < pMesh->pMeshIS->nIBCount; i++ )
		{
			if ( pMesh->pMeshIS->apDXIB[i] )
			{
				Fheap_nTotalMeshMemTracked += pMesh->pMeshIS->anIndicesCount[i] * sizeof(u16);
			}
		}
	}
#endif

	// Mesh created successfully...

	if ( hRes != FRES_NULLHANDLE )
	{
		fres_SetBase( hRes, pMesh );
	}

	return TRUE;
}


//
//
//
static void _ResLoadDestroy( void *pBase ) 
{

	FMesh_t *pMesh = (FMesh_t *)pBase;

#if FHEAP_TRACK_MEM_ALLOCATIONS
	if ( pMesh && pMesh->pMeshIS )
	{
		if ( pMesh->pMeshIS->aVB )
		{
			u32 nVBCount = pMesh->pMeshIS->nVBCount;
			FDX8VB_t *aVB = pMesh->pMeshIS->aVB;
			u32 i, nVBMem = 0;
			for ( i = 0; i < nVBCount; i++ )
			{
				nVBMem += (aVB[i].nVtxCount * aVB[i].nBytesPerVertex);
			}
			
			Fheap_nTotalMeshMemTracked -= nVBMem;
		}

		u32 i;
		for ( i = 0; i < pMesh->pMeshIS->nIBCount; i++ )
		{
			if ( pMesh->pMeshIS->apDXIB[i] )
			{
				Fheap_nTotalMeshMemTracked -= pMesh->pMeshIS->anIndicesCount[i] * sizeof(u16);
			}
		}
	}
#endif

	fdx8load_Destroy( (FMesh_t *)pBase );
}


//
//
//
// Counts actual GPU draw submissions. Nothing in the engine reports these, so
// there is no way to tell 'the renderer produced no geometry' from
// 'geometry was submitted and came out black' -- the window looks identical
// either way. Report the total from the previous frame once per frame so
// the two cases are distinguishable.

void fmesh_Renderer_Open( void ) 
{

	fdx8_SetRenderState_NORMALIZENORMALS( FALSE );
	fmesh_SetCullDir( FMESH_CULLDIR_CW );
	fdx8_SetRenderState_SHADEMODE( D3DSHADE_GOURAUD );

	fsh_Open();
}


//
//
//
void fmesh_Renderer_Close( void ) 
{
	fsh_Close();
}


