//////////////////////////////////////////////////////////////////////////////////////
// fdx8audio.cpp - 
//
// Author: Albert Yale
//////////////////////////////////////////////////////////////////////////////////////
// THIS CODE IS PROPRIETARY PROPERTY OF SWINGIN' APE STUDIOS, INC.
// Copyright (c) 2002
//
// The contents of this file may not be disclosed to third
// parties, copied or duplicated in any form, in whole or in part,
// without the prior written permission of Swingin' Ape Studios, Inc.
//////////////////////////////////////////////////////////////////////////////////////
// Modification History:
//
// Date     Who         Description
// -------- ----------  --------------------------------------------------------------
// 05/20/02 ayale       Created.
//////////////////////////////////////////////////////////////////////////////////////

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

#include "fang.h"
#include "fdx8.h"
#include "floop.h"
#include "faudio.h"
#include "fresload.h"
#include "fclib.h"
#include "ffile.h"
#include "floop.h"

#include <dsound.h>
#include <mmreg.h>
#include <math.h>

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

#define _EMITTERS_LISTENERS_WORK_DELAY				( 0.10f ) //  10 fps.

#define _DSOUND_REAL_EMITTERS_MAX_3D				( 64 )
#define _DSOUND_REAL_EMITTERS_MAX_2D				( 188 )

// Does not include bank headers, but does include wave headers and wave data.
#define _MAX_TOTAL_WAVE_DATA_SIZE					( 2047 * ( 4 * 1024 ) )
#define _MAX_TOTAL_STREAMS							( 4 )
#define _STREAMS_WORK_DELAY							( 0.00f ) // full fps.
#define _STREAM_PCM_BUFFER_SECS						( 2.0f )
#define _STREAM_COMPRESS_CHUNK_BYTES				( 8192 )
#define _STREAM_WRITE_MARGIN_BYTES					( 256 )

// _StreamStateChange_e is defined with the other enums below.

struct _Stream_t
{
	CFAudioStream *poAudioStream;

	FFileHandle ohFile;
	LPDIRECTSOUNDBUFFER8 poDSBuffer;

	f32	fVolume;
	f32 fPanLeftRight;
	f32 fFrequencyInHz;
	f32 fFrequencyFactor;
	f32 fLengthInSeconds;
	f32 fSecondsPlayed;
	f32 fSecondsToPlay;

	BOOL8 bIsActive;
	BOOL8 bIsLooping;
	BOOL8 bIgnoreInPauseMode;
	BOOL8 bTreatAsSfx;
	BOOL8 bEOF;

	u32 uStateChanges;

	u32 uDataStart;
	u32 uDataBytes;
	u32 uBytesReadCompressed;

	u32 uBufferSize;
	u32 uWritePos;
	u32 uPlayPosLast;
	u32 uBytesPerSecPCM;

	FDX8Data_WaveFormatEx_t oWavFormat;

	u8	*pbyCompressScratch;
	u8	*pbyPcmScratch;
	u32 uCompressChunkBytes;
	u32 uPcmChunkBytes;

	char	szName[ FAUDIO_MAX_ASSET_NAME_LENGTH + 1 ];

	FAudio_StreamState_e oeState;
	FAudio_PauseLevel_e ePauseLevel;

	CFAudioStream::FAudio_StreamEndOfPlayCallback_t *pEndOfPlayCallback;

}; // _Stream_t


#define _UNIQUE_VOLUME_LEVELS						( 128 )
#define _UNIQUE_FLOAT_VOL_LEVEL_INDICES				( (f32)( _UNIQUE_VOLUME_LEVELS - 1 ) )

#define _SIGNIFICANT_DISTANCE_CHANGE				( 0.001f )
#define _SIGNIFICANT_DISTANCE_CHANGE_SQ				( _SIGNIFICANT_DISTANCE_CHANGE * _SIGNIFICANT_DISTANCE_CHANGE )
#define _SIGNIFICANT_DOT_CHANGE						( 1.0f - 0.00001f ) //+- 2.562 degrees
#define _SIGNIFICANT_DOT_CHANGE_SQ					( _SIGNIFICANT_DOT_CHANGE * _SIGNIFICANT_DOT_CHANGE )
#define _SIGNIFICANT_FACTOR_CHANGE					( 0.001f )
#define _SIGNIFICANT_FACTOR_CHANGE_SQ				( _SIGNIFICANT_FACTOR_CHANGE * _SIGNIFICANT_FACTOR_CHANGE )
#define _SIGNIFICANT_VOLUME_CHANGE					( 1.0f / 127.0f )
#define _SIGNIFICANT_VOLUME_CHANGE_SQ				( _SIGNIFICANT_VOLUME_CHANGE * _SIGNIFICANT_VOLUME_CHANGE )

#if 1
#define _EMITTERS_CREATE_MAX_ERRORS					( 5 )
#else
#define _EMITTERS_CREATE_MAX_ERRORS					( 5000000 )
#endif

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

enum _EmitterProperties_e
{
	_EMITTER_PROPERTIES_3D					= 0x0001,
	_EMITTER_PROPERTIES_FIRENFORGET			= 0x0002,
	_EMITTER_PROPERTIES_AUTOPOSUPDATE 		= 0x0004,
	_EMITTER_PROPERTIES_DUCKABLE			= 0x0008,
	_EMITTER_PROPERTIES_LOOPING				= 0x0010,
	_EMITTER_PROPERTIES_IGNOREINPAUSEMODE	= 0x0020,
			
	_EMITTER_PROPERTIES_NONE				= 0x0000
}; // _EmitterProperties_e

enum _EmitterStateChange_e
{
	// Consumed by faudio_Work().
	_EMITTER_STATE_CHANGE_NONE			= 0x00000000,
	_EMITTER_STATE_CHANGE_PLAY			= 0x00000001,
	_EMITTER_STATE_CHANGE_STOP			= 0x00000002,
	_EMITTER_STATE_CHANGE_PAUSE			= 0x00000004,
	_EMITTER_STATE_CHANGE_UNPAUSE		= 0x00000008,
	_EMITTER_STATE_CHANGE_VOLUME		= 0x00000010,
	_EMITTER_STATE_CHANGE_FREQUENCY		= 0x00000020,
	_EMITTER_STATE_CHANGE_POSITION		= 0x00000040,
	_EMITTER_STATE_CHANGE_PAN			= 0x00000080,
//	_EMITTER_STATE_CHANGE_RADIUS		= 0x00000100,
	_EMITTER_STATE_CHANGE_DOPPLER		= 0x00000200,
	_EMITTER_STATE_CHANGE_REVERB		= 0x00000400,

	// Consumed by CFAudioEmitter::SetVolume().
	_EMITTER_STATE_CHANGE_DUCKABLE		= 0x00000800,
	_EMITTER_STATE_CHANGE_DUCKING		= 0x00001000

}; // _EmitterStateChange_e

enum _StreamStateChange_e
{
	// Consumed by faudio_Work().
	_STREAM_STATE_CHANGE_NONE			= 0x00000000,
	_STREAM_STATE_CHANGE_PLAY			= 0x00000001,
	_STREAM_STATE_CHANGE_STOP			= 0x00000002,
	_STREAM_STATE_CHANGE_PAUSE			= 0x00000004,
	_STREAM_STATE_CHANGE_UNPAUSE		= 0x00000008,
	_STREAM_STATE_CHANGE_VOLUME			= 0x00000010,
	_STREAM_STATE_CHANGE_FREQUENCY		= 0x00000020,
	_STREAM_STATE_CHANGE_PAN			= 0x00000040

}; // _StreamStateChange_e

enum _ListenerStateChange_e
{
	_LISTENER_STATE_CHANGE_NONE			= 0x00000000,
	_LISTENER_STATE_CHANGE_ORIENTATION  = 0x00000001

}; // _ListenerStateChange_e

enum _ListenerIntersection_e
{
	_LISTENER_INTERSECTION_NONE			= 0x00000000,
	_LISTENER_INTERSECTION_ENTERED		= 0x00000001,
	_LISTENER_INTERSECTION_EXITED		= 0x00000002,
	_LISTENER_INTERSECTION_SWITCHED		= 0x00000004,
	_LISTENER_INTERSECTION_PRESENT		= 0x00000008

}; // _ListenerIntersection_e

struct _VirtualEmitter_t;
struct _RealEmitter_t;

struct _VirtualListener_t
{
	CFWorldUser *poWorldUser;
	CFXfm *poXfmCurrentOrientation_WS;
	CFVec3A *poVecPreviousPosition_WS;	// Used for velocity / Doppler.
	CFVec3A *poVecVelocity_WS;			// Used for velocity / Doppler.

	u32 uStateChange; // See _ListenerStateChange_e.

}; // _VirtualListener_t

struct _RealEmitter_t
{
	LPDIRECTSOUNDBUFFER8 poDSBuffer;
	LPDIRECTSOUND3DBUFFER8 poDS3DBuffer;

	FLink_t oLink;

}; // _RealEmitter_t

struct _RealEmitterLimit_t
{
	u32 uPlaying, uPlayable;

}; // _RealEmitterLimit_t

struct _VirtualEmitter_t
{
	CFAudioEmitter *poAudioEmitter; // Derives from CFWorldUser.

	FAudio_WaveHandle_t oWaveHandle;

	_RealEmitter_t *poRealEmitter;
	CFAudioEmitter **ppUserAudioEmitter;	// NULL=none

	//// 3D only.
	//
	u32		uTimeStampCurrent,
			uTimeStampPrevious;

	_VirtualListener_t	*poVirtualListenerCurrent,
						*poVirtualListenerPrevious;

	_ListenerIntersection_e oeListenerIntersection;
	f32 fVirtualListenerDistanceSq;
	FLinkRoot_t *paoVirtualEmittersListActive; // Optimization.
	//
	////

	f32		fRadiusOuter,
			fRadiusInner,
			fVolume, fVolumeDucked,
			fPanLeftRight,
			fFrequencyFactor, fDopplerFactor, fReverb,
			fSecondsPlayed, fSecondsToPlay;

	u32		uStateChanges;	// See _EmitterStateChange_e.

	u8 uPriority;
	u8 oeState;
	u8 uProperties;	// See _EmitterProperties_e.
	u8 uPauseLevel; //represents current pause mode level;

	CFVec3A			*poVecCurrentPosition_WS,
					*poVecPreviousPosition_WS;	// Used for velocity / Doppler.
	const CFVec3A 	*poVecAutoPosition_WS;		// Stores the pointer for an AutoUpdate3D sound  

	CFAudioEmitter::FAudio_EmitterEndOfPlayCallback_t *pEndOfPlayCallback;

	FLink_t oLink;

}; // _VirtualEmitter_t

// global vars
u8 FAudio_EmitterDefaultPriorityLevel = 0;
BOOL8 FAudio_bModuleStarted = FALSE;
BOOL8 FAudio_bModuleInstalled = FALSE;
BOOL8 FAudio_bMasterSfxVolChanged = FALSE;
BOOL8 FAudio_bMasterMusicVolChanged = FALSE;
f32 FAudio_fMasterSfxUnitVol = 1.0f;
f32 FAudio_fMasterMusicUnitVol = 1.0f;

// Wave banks.
static u32 _uSoundBytes, _uMaxSoundBytes, _uMaxBanks;
static FLinkRoot_t _oWaveBanksList;
static FResLoadReg_t _oLoadReg;

// Directsound.
static LPDIRECTSOUND8 _poDS;

// Real Listeners.
static LPDIRECTSOUND3DLISTENER _poDSRealListener;

// Virtual Listeners.
static u32 _uMaxVirtualListeners, _uActiveVirtualListeners;
static _VirtualListener_t _aoVirtualListeners[ FAUDIO_MAX_LISTENERS ];

// Real Emitters.
static _RealEmitter_t _aoRealEmitters2D[ _DSOUND_REAL_EMITTERS_MAX_2D ];
static FLinkRoot_t _oRealEmittersListFree2D;
static FLinkRoot_t _oRealEmittersListActive2D;

static _RealEmitter_t _aoRealEmitters3D[ _DSOUND_REAL_EMITTERS_MAX_3D ];
static FLinkRoot_t _oRealEmittersListFree3D;
static FLinkRoot_t _oRealEmittersListActive3D;

static DS3DBUFFER _oDSEmitterAttributes;
static DSBUFFERDESC _oBufferDescription;

static _RealEmitterLimit_t *_paoRealEmittersLimits; // Pointer to an array of _uMaxPriorityLevels elements.

// Virtual Emitters.
static u32 _uMaxPriorityLevels; // !!Nate - This will only store an 8-bit value
static u32 _uMaxVirtualEmitters;

static _VirtualEmitter_t *_paoVirtualEmitters; // Pointer to an array of _uMaxVirtualEmitters elements.
static FLinkRoot_t _oVirtualEmittersListFree;
static FLinkRoot_t *_paoVirtualEmittersListActive2D; // Pointer to an array of _uMaxPriorityLevels elements. Each lists are sorted by volumes.
static FLinkRoot_t *_paoVirtualEmittersListActive3D; // Pointer to an array of _uMaxPriorityLevels elements. Each lists are sorted by volumes.

static FAudio_PauseLevel_e _ePauseLevelEmitters;
static f32 _fDuckingFactor;
static u32 _uEmittersCreateErrors2D, _uEmittersCreateErrors3D;

// Work.
static f32 _fEmittersListenersWorkDelay, _fOOEmittersListenersWorkDelay; // (use fangdef instead?) Should be replaced by a scheduler eventually to avoid running in synch with other low-fps work()!!!!!!!!!!
static BOOL _bSkipEmittersListenersWorkDelay;
static u32 _uTimeStamp;
static s32 _anVolumes[ _UNIQUE_VOLUME_LEVELS ]; // Optimization.

// Streams.
static u32 _uMaxStreams;
static _Stream_t *_paoStreams;
static FAudio_PauseLevel_e _ePauseLevelStreams;
static f32 _fStreamsWorkDelay;
static BOOL _bSkipStreamsWorkDelay;

static void _StreamRefill( _Stream_t *poStream );
static void _StreamApplyChanges( _Stream_t *poStream );
static void _StreamFinish( _Stream_t *poStream );

// Temp.
static CFVec3A _oTempVec3A, _oTempVec3A_Velocity;
static CFSphere _oTempSphere;
static _VirtualListener_t *_poTempVirtualListener;

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static BOOL _BankLoadCallback( FResHandle_t hRes, void *pLoadedBase, u32 nLoadedBytes, cchar *pszResName );
static void _BankUnloadCallback( void *pResMem );
static BOOL _WorldLoadAndUnloadCallback( FWorldEvent_e oeEvent );
static void _AutoDestroyEmitterEndOfPlayCallback( CFAudioEmitter *poAudioEmitter );
static BOOL _EmitterIntersectionCallback( CFWorldTracker *poWorldTracker, FVisVolume_t *pVolume );
static void _TrackEmittersProgress( FLinkRoot_t *poVirtualEmittersListActive );
static void _ApplyRealEmittersChanges( FLinkRoot_t *poVirtualEmittersListActive );
static void _InvokeEmittersEndofplayCallbacks( FLinkRoot_t *poVirtualEmittersListActive );
static void _DestroyAllEmittersFromABank( FAudio_BankHandle_t hBank );

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

BOOL faudio_ModuleStartup( void )
{
	FASSERT_MSG( ( ! FAudio_bModuleStarted ), "[ FAUDIO ] Error: System already started !!!" );

	FAudio_bModuleStarted   = TRUE;
	FAudio_bModuleInstalled = FALSE;

	FAudio_bMasterSfxVolChanged = FALSE;
	FAudio_bMasterMusicVolChanged = FALSE;
	FAudio_fMasterSfxUnitVol = 1.0f;
	FAudio_fMasterMusicUnitVol = 1.0f;

	return TRUE;

} // faudio_ModuleStartup

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void faudio_ModuleShutdown( void )
{
	faudio_Uninstall();

	FAudio_bModuleStarted = FALSE;

} // faudio_ModuleShutdown

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

FAudio_Error_e faudio_Install( const FAudio_Init_t *poInit )
{
	FASSERT_MSG( FAudio_bModuleStarted,                                                                                  "[ FAUDIO ] Error: System not started !!!" );
	FASSERT_MSG( ( ! FAudio_bModuleInstalled ),                                                                          "[ FAUDIO ] Error: System already installed !!!" );
	FASSERT_MSG( poInit,                                                                                           "[ FAUDIO ] Error: NULL pointer !!!" );
	FASSERT_MSG( ( poInit->uMaxListeners && ( FAUDIO_MAX_LISTENERS >= poInit->uMaxListeners ) ),                   "[ FAUDIO ] Error: Invalid uMaxListeners !!!" );
	FASSERT_MSG( poInit->uMaxEmitters,                                                                             "[ FAUDIO ] Error: Invalid uMaxEmitters !!!" );
	FASSERT_MSG( poInit->uMaxStreams,                                                                              "[ FAUDIO ] Error: Invalid uMaxStreams !!!" );
	FASSERT_MSG( Fang_ConfigDefs.nAudio_MaxSoundBytes,                                                            "[ FAUDIO ] Error: Invalid uMaxSoundBytes !!!" );
	FASSERT_MSG( poInit->uMaxPriorityLevels,                                                                       "[ FAUDIO ] Error: Invalid uMaxPriorityLevels !!!" );
	FASSERT_MSG( poInit->uMaxBanks,                                                                                "[ FAUDIO ] Error: Invalid uMaxBanks !!!" );
	FASSERT_MSG( ( poInit->uPriorityLimits ? ( !! poInit->paoPriorityLimits ) : ( ! poInit->paoPriorityLimits ) ), "[ FAUDIO ] Error: Invalid priority limits !!!" );
	FASSERT_MSG( poInit->DX8ONLY_ohWnd,                                                                            "[ FAUDIO ] Error: Invalid DX8ONLY_ohWnd !!!" );
	FASSERT_MSG( poInit->DX8ONLY_ohInstance,                                                                       "[ FAUDIO ] Error: Invalid DX8ONLY_ohInstance !!!" );


	////
	//
	if( FAILED( DirectSoundCreate8( NULL, &_poDS, NULL ) ) )
	{
		DEVPRINTF( "[ FAUDIO ] Error %u: DirectSoundCreate8() failed !!!\n", __LINE__ );
		return FAUDIO_ERROR;
	}

	if( FAILED( _poDS->SetCooperativeLevel( (HWND)poInit->DX8ONLY_ohWnd, DSSCL_PRIORITY ) ) )
	{
		DEVPRINTF( "[ FAUDIO ] Error %u: SetCooperativeLevel() failed !!!\n", __LINE__ );
		FDX8_SAFE_RELEASE( _poDS );
		return FAUDIO_ERROR;
	}

	LPDIRECTSOUNDBUFFER poDSBufferPrimary;

	fang_MemZero( &_oBufferDescription, sizeof( _oBufferDescription ) );
	_oBufferDescription.dwSize  = sizeof( _oBufferDescription );
	_oBufferDescription.dwFlags = ( DSBCAPS_CTRL3D | DSBCAPS_PRIMARYBUFFER );

	if( FAILED( _poDS->CreateSoundBuffer( &_oBufferDescription, &poDSBufferPrimary, NULL ) ) )
	{
		DEVPRINTF( "[ FAUDIO ] Error %u: CreateSoundBuffer() failed !!!\n", __LINE__ );
		FDX8_SAFE_RELEASE( _poDS );
		return FAUDIO_ERROR;
	}

	WAVEFORMATEX oWaveformatex;

	fang_MemZero( &oWaveformatex, sizeof( oWaveformatex ) );
	oWaveformatex.wFormatTag      = WAVE_FORMAT_PCM;
	oWaveformatex.nChannels       = 2;
	oWaveformatex.nSamplesPerSec  = 44100;
	oWaveformatex.wBitsPerSample  = 16;
	oWaveformatex.nBlockAlign     = 4;
	oWaveformatex.nAvgBytesPerSec = 176400;

	if( FAILED( poDSBufferPrimary->SetFormat( &oWaveformatex ) ) )
	{
		DEVPRINTF( "[ FAUDIO ] Error %u: SetFormat() failed !!!\n", __LINE__ );
		FDX8_SAFE_RELEASE( poDSBufferPrimary );
		FDX8_SAFE_RELEASE( _poDS );
		return FAUDIO_ERROR;
	}

	if( FAILED( poDSBufferPrimary->QueryInterface( IID_IDirectSound3DListener, (void **)&_poDSRealListener ) ) )
	{
		DEVPRINTF( "[ FAUDIO ] Error %u: SetFormat() failed !!!\n", __LINE__ );
		FDX8_SAFE_RELEASE( poDSBufferPrimary );
		FDX8_SAFE_RELEASE( _poDS );
		return FAUDIO_ERROR;
	}

	FDX8_SAFE_RELEASE( poDSBufferPrimary );
	_poDSRealListener->SetDistanceFactor( FMATH_FEET2METERS( 1.0f ), DS3D_IMMEDIATE );
	//
	////

	//// Register .Wvb loader.
	//
	if( ! fresload_IsRegistered( FAUDIOBANK_RESTYPE ) )
	{
		fang_MemZero( &_oLoadReg, sizeof( _oLoadReg ) );
		fres_CopyType( _oLoadReg.sResType, FAUDIOBANK_RESTYPE );
		_oLoadReg.pszFileExtension = "wvb";
		_oLoadReg.nMemType         = FRESLOAD_MEMTYPE_PERM;
		_oLoadReg.nAlignment       = 16;
		_oLoadReg.pFcnCreate       = _BankLoadCallback;
		_oLoadReg.pFcnDestroy      = _BankUnloadCallback;

		if( ! fresload_RegisterHandler( &_oLoadReg ) )
		{
			DEVPRINTF( "[ FAUDIO ] Error %u: fresload_RegisterHandler() failed !!!\n", __LINE__ );
			FDX8_SAFE_RELEASE( _poDSRealListener );
			FDX8_SAFE_RELEASE( _poDS );
			return FAUDIO_ERROR;
		}
	}
	//
	////

	//// Init static values.
	//
	_uTimeStamp                      = 10;
	_ePauseLevelEmitters              = FAUDIO_PAUSE_LEVEL_NONE;
	_fDuckingFactor                  = 1.0f;
	_fEmittersListenersWorkDelay     = 0.0f;
	_fOOEmittersListenersWorkDelay   = 0.0f;
	_bSkipEmittersListenersWorkDelay = TRUE;
	_uSoundBytes                     = 0;
	_uEmittersCreateErrors2D         = _EMITTERS_CREATE_MAX_ERRORS;
	_uEmittersCreateErrors3D         = _EMITTERS_CREATE_MAX_ERRORS;
	_uMaxVirtualListeners            = poInit->uMaxListeners;
	_uMaxVirtualEmitters             = poInit->uMaxEmitters;
	_uMaxPriorityLevels              = poInit->uMaxPriorityLevels;
	_uMaxSoundBytes                  = Fang_ConfigDefs.nAudio_MaxSoundBytes;
	_uMaxBanks                       = poInit->uMaxBanks;

	if( _MAX_TOTAL_WAVE_DATA_SIZE < _uMaxSoundBytes )
	{
		DEVPRINTF( "[ FAUDIO ] Warning %u: Clamping max sound bytes to %u.\n", __LINE__, _MAX_TOTAL_WAVE_DATA_SIZE );
		_uMaxSoundBytes = _MAX_TOTAL_WAVE_DATA_SIZE;
	}

	if( ! fworld_IsWorldCallbackFunctionRegistered( _WorldLoadAndUnloadCallback ) )
	{
		fworld_RegisterWorldCallbackFunction( _WorldLoadAndUnloadCallback );
	}
	//
	////

	//// Set default DS emitter attributes.
	//
	fang_MemZero( &_oDSEmitterAttributes, sizeof( _oDSEmitterAttributes ) );
	_oDSEmitterAttributes.dwSize             = sizeof( _oDSEmitterAttributes );
	_oDSEmitterAttributes.vPosition.x        = 0.0f;
	_oDSEmitterAttributes.vPosition.y        = 0.0f;
	_oDSEmitterAttributes.vPosition.z        = 0.0f;
	_oDSEmitterAttributes.vVelocity.x        = 0.0f;
	_oDSEmitterAttributes.vVelocity.y        = 0.0f;
	_oDSEmitterAttributes.vVelocity.z        = 0.0f;
	_oDSEmitterAttributes.dwInsideConeAngle  = DS3D_DEFAULTCONEANGLE;
	_oDSEmitterAttributes.dwOutsideConeAngle = DS3D_DEFAULTCONEANGLE;
	_oDSEmitterAttributes.vConeOrientation.x = 0.0f;
	_oDSEmitterAttributes.vConeOrientation.y = 0.0f;
	_oDSEmitterAttributes.vConeOrientation.z = 1.0f;
	_oDSEmitterAttributes.lConeOutsideVolume = DS3D_DEFAULTCONEOUTSIDEVOLUME;
	_oDSEmitterAttributes.flMinDistance      = DS3D_DEFAULTMINDISTANCE;
	_oDSEmitterAttributes.flMaxDistance      = DS3D_DEFAULTMAXDISTANCE;
	_oDSEmitterAttributes.dwMode             = DS3DMODE_HEADRELATIVE;
	//
	////

	//// Init arrays and lists.
	//
	// Virtual listeners.
	fang_MemZero( _aoVirtualListeners, sizeof( _aoVirtualListeners ) );

	// Virtual emitters.
	flinklist_InitRoot( &_oVirtualEmittersListFree, (s32)FANG_OFFSETOF( _VirtualEmitter_t, oLink ) );

	// Real emitters.
	fang_MemZero( _aoRealEmitters2D, sizeof( _aoRealEmitters2D ) );
	flinklist_InitRoot( &_oRealEmittersListFree2D, (s32)FANG_OFFSETOF( _RealEmitter_t, oLink ) );
	flinklist_InitRoot( &_oRealEmittersListActive2D, (s32)FANG_OFFSETOF( _RealEmitter_t, oLink ) );

	fang_MemZero( _aoRealEmitters3D, sizeof( _aoRealEmitters3D ) );
	flinklist_InitRoot( &_oRealEmittersListFree3D, (s32)FANG_OFFSETOF( _RealEmitter_t, oLink ) );
	flinklist_InitRoot( &_oRealEmittersListActive3D, (s32)FANG_OFFSETOF( _RealEmitter_t, oLink ) );

	// Banks.
	flinklist_InitRoot( &_oWaveBanksList, (s32)FANG_OFFSETOF( FDataWvbFile_Bank_t, oLink ) );
	//
	////

	FResFrame_t oResFrame = fres_GetFrame();

	//// Allocate memory.
	//
	_paoVirtualEmitters             = (_VirtualEmitter_t *)fres_AllocAndZero( sizeof( _VirtualEmitter_t ) * _uMaxVirtualEmitters );
	_paoVirtualEmittersListActive2D = (FLinkRoot_t *)fres_AllocAndZero( sizeof( FLinkRoot_t ) * _uMaxPriorityLevels );
	_paoVirtualEmittersListActive3D = (FLinkRoot_t *)fres_AllocAndZero( sizeof( FLinkRoot_t ) * _uMaxPriorityLevels );
	_paoRealEmittersLimits          = (_RealEmitterLimit_t *)fres_AllocAndZero( sizeof( _RealEmitterLimit_t ) * _uMaxPriorityLevels );
	if( ! ( _paoVirtualEmitters &&
			_paoVirtualEmittersListActive2D &&
			_paoVirtualEmittersListActive3D &&
			_paoRealEmittersLimits ) )
	{
		DEVPRINTF( "[ FAUDIO ] Error %u: fres_AllocAndZero() failed !!!\n", __LINE__ );
		FDX8_SAFE_RELEASE( _poDSRealListener );
		FDX8_SAFE_RELEASE( _poDS );
		fres_ReleaseFrame( oResFrame );
		return FAUDIO_ERROR;
	}
	//
	////

	u32 uIndex;

	////
	//
	for( uIndex = 0; uIndex < _uMaxPriorityLevels; ++uIndex )
	{
		flinklist_InitRoot( &( _paoVirtualEmittersListActive2D[ uIndex ] ), (s32)FANG_OFFSETOF( _VirtualEmitter_t, oLink ) );
		flinklist_InitRoot( &( _paoVirtualEmittersListActive3D[ uIndex ] ), (s32)FANG_OFFSETOF( _VirtualEmitter_t, oLink ) );

		_paoRealEmittersLimits[ uIndex ].uPlayable = FAUDIO_UNLIMITED_EMITTERS;
	}
	//
	////

	//// Init priority limits.
	//
	if( poInit->paoPriorityLimits )
	{
		FAudio_PriorityLimit_t *poPriorityLimit;

		for( uIndex = 0; uIndex < poInit->uPriorityLimits; ++uIndex )
		{
			////
			//
			poPriorityLimit = &( poInit->paoPriorityLimits[ uIndex ] );

			FASSERT_MSG( ( _uMaxPriorityLevels > poPriorityLimit->uPriorityLevel ), "[ FAUDIO ] Error: Invalid uPriorityLevel !!!" );
			//
			////

			_paoRealEmittersLimits[ poPriorityLimit->uPriorityLevel ].uPlayable = poPriorityLimit->uMaxPlayable;
		}
	}
	//
	////

	_VirtualListener_t *poVirtualListener = &( _aoVirtualListeners[ 0 ] );
	_VirtualEmitter_t *poVirtualEmitter   = &( _paoVirtualEmitters[ 0 ] );

	//// Allocate memory.
	//
	poVirtualListener->poWorldUser                = (CFWorldUser *)fnew CFWorldUser[ _uMaxVirtualListeners ];
	poVirtualListener->poXfmCurrentOrientation_WS = (CFXfm *)fnew CFXfm[ _uMaxVirtualListeners ];
	poVirtualListener->poVecPreviousPosition_WS   = (CFVec3A *)fnew CFVec3A[ _uMaxVirtualListeners ];
	poVirtualListener->poVecVelocity_WS           = (CFVec3A *)fnew CFVec3A[ _uMaxVirtualListeners ];

	poVirtualEmitter->poAudioEmitter              = (CFAudioEmitter *)fnew CFAudioEmitter[ _uMaxVirtualEmitters ];
	poVirtualEmitter->poVecCurrentPosition_WS     = (CFVec3A *)fnew CFVec3A[ _uMaxVirtualEmitters ];
	poVirtualEmitter->poVecPreviousPosition_WS    = (CFVec3A *)fnew CFVec3A[ _uMaxVirtualEmitters ];

	if( ! (	poVirtualListener->poWorldUser &&
			poVirtualListener->poXfmCurrentOrientation_WS &&
			poVirtualListener->poVecPreviousPosition_WS &&
			poVirtualListener->poVecVelocity_WS &&

			poVirtualEmitter->poAudioEmitter &&
			poVirtualEmitter->poVecCurrentPosition_WS &&
			poVirtualEmitter->poVecPreviousPosition_WS ) )
	{
		DEVPRINTF( "[ FAUDIO ] Error %u: fnew() failed !!!\n", __LINE__ );

		fdelete_array( poVirtualEmitter->poVecPreviousPosition_WS );
		fdelete_array( poVirtualEmitter->poVecCurrentPosition_WS );
		fdelete_array( poVirtualEmitter->poAudioEmitter );

		fdelete_array( poVirtualListener->poVecVelocity_WS );
		fdelete_array( poVirtualListener->poVecPreviousPosition_WS );
		fdelete_array( poVirtualListener->poXfmCurrentOrientation_WS );
		fdelete_array( poVirtualListener->poWorldUser );

		FDX8_SAFE_RELEASE( _poDSRealListener );
		FDX8_SAFE_RELEASE( _poDS );
		fres_ReleaseFrame( oResFrame );

		return FAUDIO_ERROR;
	}
	//
	////

	//// Init 1st listener.
	//
	_uActiveVirtualListeners = 1;
	poVirtualListener->poXfmCurrentOrientation_WS->Identity();
	poVirtualListener->poVecPreviousPosition_WS->Zero();
	poVirtualListener->poVecVelocity_WS->Zero();
	poVirtualListener->poWorldUser->m_nUser = FWORLD_USERTYPE_AUDIO_LISTENER;
	poVirtualListener->poWorldUser->m_pUser = poVirtualListener;
	//
	////

	//// Fix-up.
	//
	for( uIndex = 1; uIndex < _uMaxVirtualListeners; ++uIndex )
	{
		poVirtualListener                             = &( _aoVirtualListeners[ uIndex ] );
		poVirtualListener->poWorldUser                = ( _aoVirtualListeners[ 0 ].poWorldUser + uIndex );
		poVirtualListener->poXfmCurrentOrientation_WS = ( _aoVirtualListeners[ 0 ].poXfmCurrentOrientation_WS + uIndex );
		poVirtualListener->poVecPreviousPosition_WS   = ( _aoVirtualListeners[ 0 ].poVecPreviousPosition_WS + uIndex );
		poVirtualListener->poVecVelocity_WS           = ( _aoVirtualListeners[ 0 ].poVecVelocity_WS + uIndex );

		poVirtualListener->poWorldUser->m_nUser       = FWORLD_USERTYPE_AUDIO_LISTENER;
		poVirtualListener->poWorldUser->m_pUser       = poVirtualListener;
		poVirtualListener->poXfmCurrentOrientation_WS->Identity();
		poVirtualListener->poVecPreviousPosition_WS->Zero();
		poVirtualListener->poVecVelocity_WS->Zero();
	}

	//// Init virtual emitter.
	//
	poVirtualEmitter->poVecCurrentPosition_WS->Zero();
	poVirtualEmitter->poVecPreviousPosition_WS->Zero();
	poVirtualEmitter->poVecAutoPosition_WS = NULL;
	poVirtualEmitter->poAudioEmitter->m_nUser   = FWORLD_USERTYPE_AUDIO_EMITTER_INACTIVE;
	poVirtualEmitter->poAudioEmitter->m_pUser   = poVirtualEmitter;
	poVirtualEmitter->poRealEmitter				= NULL;
	poVirtualEmitter->poVirtualListenerCurrent	= NULL;
	poVirtualEmitter->poVirtualListenerPrevious	= NULL;
	poVirtualEmitter->uTimeStampCurrent			= 0;
	poVirtualEmitter->uTimeStampPrevious		= 0;
	poVirtualEmitter->oeListenerIntersection	= _LISTENER_INTERSECTION_NONE;
	poVirtualEmitter->fRadiusOuter				= 1.0f;
	poVirtualEmitter->fRadiusInner				= DS3D_DEFAULTMINDISTANCE;
	poVirtualEmitter->fVolume					= 1.0f;
	poVirtualEmitter->fVolumeDucked				= 1.0f;
	poVirtualEmitter->fPanLeftRight				= 0.0f;
	poVirtualEmitter->fFrequencyFactor			= 1.0f;
	poVirtualEmitter->fDopplerFactor			= 0.0f;
	poVirtualEmitter->fReverb					= 0.0f;
	poVirtualEmitter->fSecondsPlayed			= 0.0f;
	poVirtualEmitter->fSecondsToPlay			= 0.0f;
	poVirtualEmitter->uProperties 				= _EMITTER_PROPERTIES_DUCKABLE; //default to duckable
	poVirtualEmitter->uPauseLevel 				= FAUDIO_PAUSE_LEVEL_1; //default pause level
	poVirtualEmitter->uStateChanges				= _EMITTER_STATE_CHANGE_NONE;
	poVirtualEmitter->oeState					= FAUDIO_EMITTER_STATE_STOPPED;
	poVirtualEmitter->pEndOfPlayCallback		= NULL;
	//
	////

	flinklist_AddTail( &_oVirtualEmittersListFree, poVirtualEmitter );

	for( uIndex = 1; uIndex < _uMaxVirtualEmitters; ++uIndex )
	{
		poVirtualEmitter                            = &( _paoVirtualEmitters[ uIndex ] );
		poVirtualEmitter->poAudioEmitter            = ( _paoVirtualEmitters[ 0 ].poAudioEmitter + uIndex );
		poVirtualEmitter->poVecCurrentPosition_WS   = ( _paoVirtualEmitters[ 0 ].poVecCurrentPosition_WS + uIndex );
		poVirtualEmitter->poVecPreviousPosition_WS  = ( _paoVirtualEmitters[ 0 ].poVecPreviousPosition_WS + uIndex );

		//// Init virtual emitter.
		//
		poVirtualEmitter->poVecCurrentPosition_WS->Zero();
		poVirtualEmitter->poVecPreviousPosition_WS->Zero();
		poVirtualEmitter->poVecAutoPosition_WS = NULL;
		poVirtualEmitter->poAudioEmitter->m_nUser   = FWORLD_USERTYPE_AUDIO_EMITTER_INACTIVE;
		poVirtualEmitter->poAudioEmitter->m_pUser   = poVirtualEmitter;
		poVirtualEmitter->poRealEmitter				= NULL;
		poVirtualEmitter->poVirtualListenerCurrent	= NULL;
		poVirtualEmitter->poVirtualListenerPrevious	= NULL;
		poVirtualEmitter->uTimeStampCurrent			= 0;
		poVirtualEmitter->uTimeStampPrevious		= 0;
		poVirtualEmitter->oeListenerIntersection	= _LISTENER_INTERSECTION_NONE;
		poVirtualEmitter->fRadiusOuter				= 1.0f;
		poVirtualEmitter->fRadiusInner				= DS3D_DEFAULTMINDISTANCE;
		poVirtualEmitter->fVolume					= 1.0f;
		poVirtualEmitter->fVolumeDucked				= 1.0f;
		poVirtualEmitter->fPanLeftRight				= 0.0f;
		poVirtualEmitter->fFrequencyFactor			= 1.0f;
		poVirtualEmitter->fDopplerFactor			= 0.0f;
		poVirtualEmitter->fReverb					= 0.0f;
		poVirtualEmitter->fSecondsPlayed			= 0.0f;
		poVirtualEmitter->fSecondsToPlay			= 0.0f;
		poVirtualEmitter->uProperties 				= _EMITTER_PROPERTIES_DUCKABLE; //default to duckable
		poVirtualEmitter->uPauseLevel 				= FAUDIO_PAUSE_LEVEL_1; //default pause level
		poVirtualEmitter->uStateChanges				= _EMITTER_STATE_CHANGE_NONE;
		poVirtualEmitter->oeState					= FAUDIO_EMITTER_STATE_STOPPED;
		poVirtualEmitter->pEndOfPlayCallback		= NULL;
		//
		////

		flinklist_AddTail( &_oVirtualEmittersListFree, poVirtualEmitter );
	}
	//
	////

	//// Add to free lists.
	//
	_RealEmitter_t *poRealEmitter;

	//// 2D.
	//
	for( uIndex = 0; uIndex < _DSOUND_REAL_EMITTERS_MAX_2D; ++uIndex )
	{
		poRealEmitter = &( _aoRealEmitters2D[ uIndex ] );

		flinklist_AddTail( &_oRealEmittersListFree2D, poRealEmitter );
	}
	//
	////

	//// 3D.
	//
	for( uIndex = 0; uIndex < _DSOUND_REAL_EMITTERS_MAX_3D; ++uIndex )
	{
		poRealEmitter = &( _aoRealEmitters3D[ uIndex ] );

		flinklist_AddTail( &_oRealEmittersListFree3D, poRealEmitter );
	}
	//
	////
	//
	//// Add to free lists.

	//// Volume look-up table.
	//
	double dTempVolume;
	for( uIndex = 0; uIndex < _UNIQUE_VOLUME_LEVELS; ++uIndex )
	{
		dTempVolume = 2500.0f * log10( (double)uIndex / (double)( _UNIQUE_VOLUME_LEVELS - 1 ) );

		if( dTempVolume < DSBVOLUME_MIN )
		{
			dTempVolume = DSBVOLUME_MIN;
		}
		else if( dTempVolume > DSBVOLUME_MAX )
		{
			dTempVolume = DSBVOLUME_MAX;
		}

		_anVolumes[ uIndex ] = (s32)( dTempVolume );
	}
	//
	////

	//// Init streams.
	//
	_ePauseLevelStreams   = FAUDIO_PAUSE_LEVEL_NONE;
	_fStreamsWorkDelay    = 0.0f;
	_bSkipStreamsWorkDelay = TRUE;
	_uMaxStreams          = poInit->uMaxStreams;

	if( _MAX_TOTAL_STREAMS < _uMaxStreams )
	{
		DEVPRINTF( "[ FAUDIO ] Warning %u: Clamping max streams to %u.\n", __LINE__, _MAX_TOTAL_STREAMS );
		_uMaxStreams = _MAX_TOTAL_STREAMS;
	}

	_paoStreams = (_Stream_t *)fres_AllocAndZero( sizeof( _Stream_t ) * _uMaxStreams );
	if( ! _paoStreams )
	{
		DEVPRINTF( "[ FAUDIO ] Error %u: fres_AllocAndZero() failed !!!\n", __LINE__ );
		FDX8_SAFE_RELEASE( _poDSRealListener );
		FDX8_SAFE_RELEASE( _poDS );
		fres_ReleaseFrame( oResFrame );
		return FAUDIO_ERROR;
	}

	{
		CFAudioStream *poStreamObjects = (CFAudioStream *)fnew CFAudioStream[ _uMaxStreams ];
		if( ! poStreamObjects )
		{
			DEVPRINTF( "[ FAUDIO ] Error %u: fnew() failed !!!\n", __LINE__ );
			FDX8_SAFE_RELEASE( _poDSRealListener );
			FDX8_SAFE_RELEASE( _poDS );
			fres_ReleaseFrame( oResFrame );
			return FAUDIO_ERROR;
		}

		for( uIndex = 0; uIndex < _uMaxStreams; ++uIndex )
		{
			_Stream_t *poStream = &( _paoStreams[ uIndex ] );

			poStream->poAudioStream          = ( poStreamObjects + uIndex );
			poStream->poAudioStream->m_uData = (u32)poStream;
			poStream->bIsActive              = FALSE;
			poStream->ohFile                 = FFILE_INVALID_HANDLE;
			poStream->poDSBuffer             = NULL;
			poStream->pbyCompressScratch     = NULL;
			poStream->pbyPcmScratch          = NULL;
			poStream->ePauseLevel            = FAUDIO_PAUSE_LEVEL_1;
			poStream->oeState                = FAUDIO_STREAM_STATE_NONE;
			poStream->szName[ 0 ]            = 0;
		}
	}
	//
	////

	FAudio_bModuleInstalled = TRUE;

	return FAUDIO_NO_ERROR;

} // faudio_Install

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void faudio_Uninstall( void )
{
	if( ! FAudio_bModuleInstalled )
	{
		return;
	}

	CFAudioEmitter::DestroyAll();
	CFAudioStream::DestroyAll();

	u32 uIndex;

	//// Free streams.
	//
	if( _paoStreams )
	{
		for( uIndex = 0; uIndex < _uMaxStreams; ++uIndex )
		{
			_Stream_t *poStream = &( _paoStreams[ uIndex ] );

			FDX8_SAFE_RELEASE( poStream->poDSBuffer );
			delete[] poStream->pbyCompressScratch;
			delete[] poStream->pbyPcmScratch;

			if( FFILE_INVALID_HANDLE != poStream->ohFile )
			{
				ffile_Close( poStream->ohFile );
				poStream->ohFile = FFILE_INVALID_HANDLE;
			}
		}

		fdelete_array( _paoStreams[ 0 ].poAudioStream );
		_paoStreams = NULL;
		_uMaxStreams = 0;
	}
	//
	////

	////
	//
	if( fworld_IsWorldCallbackFunctionRegistered( _WorldLoadAndUnloadCallback ) )
	{
		fworld_UnregisterWorldCallbackFunction( _WorldLoadAndUnloadCallback );
	}
	//
	////

	_poDSRealListener->CommitDeferredSettings();
	FDX8_SAFE_RELEASE( _poDSRealListener );

	//// Stop and release DS emitters.
	//
	// 3D.
	for( uIndex = 0; uIndex < _DSOUND_REAL_EMITTERS_MAX_3D; ++uIndex )
	{
		FDX8_SAFE_RELEASE( _aoRealEmitters3D[ uIndex ].poDS3DBuffer );
		FDX8_SAFE_RELEASE( _aoRealEmitters3D[ uIndex ].poDSBuffer );
	}

	// 2D.
	for( uIndex = 0; uIndex < _DSOUND_REAL_EMITTERS_MAX_2D; ++uIndex )
	{
		FDX8_SAFE_RELEASE( _aoRealEmitters2D[ uIndex ].poDSBuffer );
	}
	//
	////

	FDX8_SAFE_RELEASE( _poDS );

	//// Free memory.
	//
	_VirtualEmitter_t *poVirtualEmitter = &( _paoVirtualEmitters[ 0 ] );
	fdelete_array( poVirtualEmitter->poVecPreviousPosition_WS );
	fdelete_array( poVirtualEmitter->poVecCurrentPosition_WS );
	fdelete_array( poVirtualEmitter->poAudioEmitter );

	_VirtualListener_t *poVirtualListener = &( _aoVirtualListeners[ 0 ] );
	fdelete_array( poVirtualListener->poVecVelocity_WS );
	fdelete_array( poVirtualListener->poVecPreviousPosition_WS );
	fdelete_array( poVirtualListener->poXfmCurrentOrientation_WS );
	fdelete_array( poVirtualListener->poWorldUser );
	//
	////


	FAudio_bModuleInstalled = FALSE;

} // faudio_Uninstall

static const s16 _asnXAdpcmStepTable[89] = {
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
	19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
	50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
	130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
	337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
	876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
	2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
	5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
	15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};
static const s8 _anXAdpcmIndexTable[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

static u32 _xadpcm_DecodedSize( const FDX8Data_WaveFormatEx_t *poFormat, u32 uSourceBytes )
{
	if( FDX8DATA_WAVEFORMATEX_ID_PCM == poFormat->WaveFormatEx.wFormatTag )
	{
		return( uSourceBytes );
	}

	if( FDX8DATA_WAVEFORMATEX_ID_XBOX_ADPCM == poFormat->WaveFormatEx.wFormatTag )
	{
		u32 uBlocks;

		if( ( 0 == poFormat->WaveFormatEx.nBlockAlign ) ||
			( 0 == poFormat->nSamplesPerBlock ) ||
			( 0 == poFormat->WaveFormatEx.nChannels ) ||
			( 2 < poFormat->WaveFormatEx.nChannels ) ||
			( poFormat->WaveFormatEx.nBlockAlign < ( 4 * poFormat->WaveFormatEx.nChannels ) ) )
		{
			return( 0 );
		}

		uBlocks = ( uSourceBytes + poFormat->WaveFormatEx.nBlockAlign - 1 ) / poFormat->WaveFormatEx.nBlockAlign;
		return( uBlocks * poFormat->nSamplesPerBlock * poFormat->WaveFormatEx.nChannels * sizeof( s16 ) );
	}

	return( 0 );
}

static BOOL _xadpcm_DecodeWave( const FDX8Data_WaveFormatEx_t *poFormat, const u8 *pbySource, u32 uSourceBytes, void *pDestination )
{
	u32 uChannels, uBlockAlign, uSamplesPerBlock, uGroups, uBlocks, uBlock, uChannel, uGroup, uByte, uNibble, uSample;
	s16 *psDestination;
	s32 anPredictor[2], anStepIndex[2], nPredictor, nStepIndex, nNibble, nDelta, nStep, nDiff;

	uChannels        = poFormat->WaveFormatEx.nChannels;
	uBlockAlign      = poFormat->WaveFormatEx.nBlockAlign;
	uSamplesPerBlock = poFormat->nSamplesPerBlock;

	if( FDX8DATA_WAVEFORMATEX_ID_PCM == poFormat->WaveFormatEx.wFormatTag )
	{
		fang_MemCopy( pDestination, pbySource, uSourceBytes );
		return( TRUE );
	}

	if( ( FDX8DATA_WAVEFORMATEX_ID_XBOX_ADPCM != poFormat->WaveFormatEx.wFormatTag ) ||
		( 0 == uBlockAlign ) ||
		( 0 == uSamplesPerBlock ) ||
		( 0 == uChannels ) ||
		( 2 < uChannels ) ||
		( uBlockAlign < ( 4 * uChannels ) ) )
	{
		return( FALSE );
	}

	uGroups       = ( uBlockAlign - ( 4 * uChannels ) ) / ( 4 * uChannels );
	uBlocks       = ( uSourceBytes + uBlockAlign - 1 ) / uBlockAlign;
	psDestination = (s16 *)pDestination;

	for( uBlock = 0; uBlock < uBlocks; ++uBlock )
	{
		const u8 *pbyBlock = pbySource + ( uBlock * uBlockAlign );

		for( uChannel = 0; uChannel < uChannels; ++uChannel )
		{
			anPredictor[ uChannel ] = (s32)(s16)( pbyBlock[0] | ( pbyBlock[1] << 8 ) );
			anStepIndex[ uChannel ] = (s32)(s16)( pbyBlock[2] | ( pbyBlock[3] << 8 ) );
			if( ( anStepIndex[ uChannel ] < 0 ) || ( 88 < anStepIndex[ uChannel ] ) )
			{
				return( FALSE );
			}
			psDestination[ uChannel ] = (s16)anPredictor[ uChannel ];
			pbyBlock += 4;
		}

		for( uGroup = 0; uGroup < uGroups; ++uGroup )
		{
			for( uChannel = 0; uChannel < uChannels; ++uChannel )
			{
				const u8 *pbyChunk = pbyBlock + ( ( uGroup * uChannels + uChannel ) * 4 );

				for( uByte = 0; uByte < 4; ++uByte )
				{
					u8 byValue = pbyChunk[ uByte ];

					for( uNibble = 0; uNibble < 2; ++uNibble )
					{
						nNibble    = uNibble ? ( (s32)( byValue >> 4 ) ) : ( (s32)( byValue & 0x0F ) );
						nStep      = _asnXAdpcmStepTable[ anStepIndex[ uChannel ] ];
						nStepIndex = anStepIndex[ uChannel ] + _anXAdpcmIndexTable[ nNibble ];
						if( nStepIndex < 0 ) { nStepIndex = 0; }
						if( nStepIndex > 88 ) { nStepIndex = 88; }
						nDelta = nNibble & 0x07;
						nDiff  = ( ( ( 2 * nDelta ) + 1 ) * nStep ) >> 3;
						if( nNibble & 0x08 ) { nPredictor = anPredictor[ uChannel ] - nDiff; }
						else                 { nPredictor = anPredictor[ uChannel ] + nDiff; }
						if( nPredictor > 32767 )  { nPredictor = 32767; }
						if( nPredictor < -32768 ) { nPredictor = -32768; }
						anPredictor[ uChannel ] = nPredictor;
						anStepIndex[ uChannel ] = nStepIndex;

						uSample = 1 + ( uGroup * 8 ) + ( uByte * 2 ) + uNibble;
						if( uSample < uSamplesPerBlock )
						{
							psDestination[ ( uSample * uChannels ) + uChannel ] = (s16)nPredictor;
						}
					}
				}
			}
		}

		psDestination += ( uSamplesPerBlock * uChannels );
	}

	return( TRUE );
}

static BOOL _BankLoadCallback( FResHandle_t hRes, void *pLoadedBase, u32 nLoadedBytes, cchar *pszResName )
{
	if( ! FAudio_bModuleInstalled )
	{
		return FALSE;
	}

	FASSERT_MSG( hRes,        "[ FAUDIO ] Error: Invalid handle !!!" );
	FASSERT_MSG( pLoadedBase, "[ FAUDIO ] Error: NULL pointer !!!" );
	FASSERT_MSG( pszResName,  "[ FAUDIO ] Error: NULL pointer !!!" );
	FASSERT_MSG( *pszResName, "[ FAUDIO ] Error: Zero length string !!!" );

	////
	//
	if( ( sizeof( FDataWvbFile_Bank_t ) + sizeof( FDataWvbFile_Wave_t ) + sizeof( FDX8Data_WaveFormatEx_t ) + 1 ) > nLoadedBytes )
	{
		DEVPRINTF( "[ FAUDIO ] Error %u: Invalid bank !!!\n", __LINE__ );
		return FALSE;
	}
	//
	////

	////
	//
	if( _oWaveBanksList.nCount == _uMaxBanks )
	{
		DEVPRINTF( "[ FAUDIO ] Error %u: Maximum number of banks already loaded !!!\n", __LINE__ );
		return FALSE;
	}
	//
	////

	////
	//
	FDataWvbFile_Bank_t *poBankSource = (FDataWvbFile_Bank_t *)pLoadedBase;
	u32 uFormatDataSize = ( sizeof( FDX8Data_WaveFormatEx_t ) * poBankSource->uWaves );
//relevant??????
	u32 uWaveDataSize = ( poBankSource->uDataLength - uFormatDataSize );
	//
	////

	////
	//
//relevant??????
//fmath_FloatToU32( * 3.5f (XBADPCM) or 4.0f (MSADPCM) )
	if( ( uWaveDataSize + _uSoundBytes ) > _uMaxSoundBytes )
	{
		DEVPRINTF( "[ FAUDIO ] Error %u: Bank too large for available audio memory !!!\n", __LINE__ );
		return FALSE;
	}
	//
	////

	//// Find total memory required for uncompressed audio.
	//
	FDataWvbFile_Bank_t *poBankDestination;
	FDataWvbFile_Wave_t *poWaveSource, *poWaveDestination;
	FDX8Data_WaveFormatEx_t *poWaveformatSource, *poWaveformatDestination;
	u32 uIndex, uSize, uTotalSize;

	uTotalSize         = ( sizeof( FDataWvbFile_Bank_t ) + ( poBankSource->uWaves * ( sizeof( FDataWvbFile_Wave_t ) + sizeof( FDX8Data_WaveFormatEx_t ) ) ) );
	poWaveSource       = (FDataWvbFile_Wave_t *)( (u32)poBankSource + sizeof( FDataWvbFile_Bank_t ) );
	poWaveformatSource = (FDX8Data_WaveFormatEx_t *)( (u32)poWaveSource + ( poBankSource->uWaves * sizeof( FDataWvbFile_Wave_t ) ) );

	for( uIndex = 0; uIndex < poBankSource->uWaves; ++uIndex, ++poWaveSource, ++poWaveformatSource )
	{
		uSize = _xadpcm_DecodedSize( poWaveformatSource, poWaveSource->uLength );
		if( 0 == uSize )
		{
			DEVPRINTF( "[ FAUDIO ] Error %u: unsupported wave format 0x%X in bank '%s' !!!\n", __LINE__, poWaveformatSource->WaveFormatEx.wFormatTag, pszResName );
			return FALSE;
		}

		uTotalSize += uSize;
	}
	//
	////

	//// Allocate memory for uncompressed audio.
	//
	poBankDestination = (FDataWvbFile_Bank_t *)fres_Alloc( uTotalSize );
	if( ! poBankDestination )
	{
		DEVPRINTF( "[ FAUDIO ] Error %u: fres_Alloc() !!!\n", __LINE__ );
		return FALSE;
	}

	fang_MemCopy( poBankDestination, poBankSource, ( sizeof( FDataWvbFile_Bank_t ) + ( poBankSource->uWaves * sizeof( FDataWvbFile_Wave_t ) ) ) );
	//
	////

	//// Uncompress audio.
	//
	poBankSource->pData            = (void *)( (u32)poBankSource + sizeof( FDataWvbFile_Bank_t ) + ( poBankSource->uWaves * sizeof( FDataWvbFile_Wave_t ) ) );
	poBankDestination->pData       = (void *)( (u32)poBankDestination + sizeof( FDataWvbFile_Bank_t ) + ( poBankDestination->uWaves * sizeof( FDataWvbFile_Wave_t ) ) );
	poBankDestination->uDataLength = ( uTotalSize - sizeof( FDataWvbFile_Bank_t ) - ( poBankDestination->uWaves * sizeof( FDataWvbFile_Wave_t ) ) );

	poWaveSource                   = (FDataWvbFile_Wave_t *)( (u32)poBankSource + sizeof( FDataWvbFile_Bank_t ) );
	poWaveformatSource             = (FDX8Data_WaveFormatEx_t *)( (u32)poWaveSource + ( poBankSource->uWaves * sizeof( FDataWvbFile_Wave_t ) ) );

	poWaveDestination              = (FDataWvbFile_Wave_t *)( (u32)poBankDestination + sizeof( FDataWvbFile_Bank_t ) );
	poWaveformatDestination        = (FDX8Data_WaveFormatEx_t *)( (u32)poWaveDestination + ( poBankSource->uWaves * sizeof( FDataWvbFile_Wave_t ) ) );

	for( uIndex = 0; uIndex < poBankSource->uWaves; ++uIndex, ++poWaveSource, ++poWaveDestination, ++poWaveformatSource, ++poWaveformatDestination )
	{
		*poWaveformatDestination                              = *poWaveformatSource;
		poWaveformatDestination->WaveFormatEx.wFormatTag      = FDX8DATA_WAVEFORMATEX_ID_PCM;
		poWaveformatDestination->WaveFormatEx.wBitsPerSample  = 16;
		poWaveformatDestination->WaveFormatEx.nBlockAlign     = (u16)( poWaveformatSource->WaveFormatEx.nChannels * 2 );
		poWaveformatDestination->WaveFormatEx.nAvgBytesPerSec = poWaveformatSource->WaveFormatEx.nSamplesPerSec * poWaveformatDestination->WaveFormatEx.nBlockAlign;
		poWaveformatDestination->WaveFormatEx.cbSize          = 0;

		uSize = _xadpcm_DecodedSize( poWaveformatSource, poWaveSource->uLength );
		if( 0 == uSize )
		{
			DEVPRINTF( "[ FAUDIO ] Error %u: unsupported wave format 0x%X in bank '%s' !!!\n", __LINE__, poWaveformatSource->WaveFormatEx.wFormatTag, pszResName );
			return FALSE;
		}

		poWaveDestination->oBankHandle = (FAudio_BankHandle_t)poBankDestination;
		poWaveDestination->uLength = uSize;
		if( uIndex )
		{
			poWaveDestination->uOffset = ( ( poWaveDestination - 1 )->uOffset + ( poWaveDestination - 1 )->uLength );
		}
		else
		{
			poWaveDestination->uOffset = 0;
		}

		if( !_xadpcm_DecodeWave( poWaveformatSource,
									(const u8 *)( (u32)poBankSource->pData + uFormatDataSize + poWaveSource->uOffset ),
									poWaveSource->uLength,
									(u8 *)( (u32)poBankDestination->pData + uFormatDataSize + poWaveDestination->uOffset ) ) )
		{
			DEVPRINTF( "[ FAUDIO ] Error %u: decoding wave '%s' failed !!!\n", __LINE__, poWaveSource->szName );
			return FALSE;
		}
	}
	//
	////

	////
	//
//	_uSoundBytes += uWaveDataSize;
	flinklist_AddTail( &_oWaveBanksList, poBankDestination );
	fres_SetBase( hRes, poBankDestination );
	//
	////

	return TRUE;

} // _BankLoadCallback

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void _BankUnloadCallback( void *pResMem )
{
	FASSERT_MSG( pResMem, "[ FAUDIO ] Error: NULL pointer !!!" );

	////
	//
	FDataWvbFile_Bank_t *poBank = (FDataWvbFile_Bank_t *)pResMem;

//	_uSoundBytes -= ( poBank->uDataLength - ( sizeof( FDX8Data_WaveFormatEx_t ) * poBank->uWaves ) );

	// kill all sounds that are currently using this bank
	_DestroyAllEmittersFromABank( (FAudio_BankHandle_t)poBank );

	flinklist_RemoveTail( &_oWaveBanksList );
	//
	////

} // _BankUnloadCallback

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static BOOL _WorldLoadAndUnloadCallback( FWorldEvent_e oeEvent )
{
	if( ! FAudio_bModuleInstalled )
	{
		return TRUE;
	}

	u32 uIndex;

	if( FWORLD_EVENT_WORLD_POSTLOAD == oeEvent )
	{
		FLinkRoot_t *poVirtualEmittersList;
		_VirtualListener_t *poVirtualListener;
		_VirtualEmitter_t *poVirtualEmitter;

		//// Insert virtual listeners in the world.
		//
		for( uIndex = 0; uIndex < _uMaxVirtualListeners; ++uIndex )
		{
			poVirtualListener      = &( _aoVirtualListeners[ uIndex ] );
			_oTempSphere.m_Pos     = poVirtualListener->poXfmCurrentOrientation_WS->m_MtxF.m_vPos.v3;
			_oTempSphere.m_fRadius = DS3D_DEFAULTMINDISTANCE;

			poVirtualListener->poWorldUser->MoveTracker( _oTempSphere );
		}
		//
		////

		//// Insert active 3D virtual emitters in the world.
		//
		for( uIndex = 0; uIndex < _uMaxPriorityLevels; ++uIndex )
		{
			poVirtualEmittersList = &( _paoVirtualEmittersListActive3D[ uIndex ] );
			poVirtualEmitter      = (_VirtualEmitter_t *)flinklist_GetHead( poVirtualEmittersList );

			while( poVirtualEmitter )
			{
				_oTempSphere.m_Pos     = poVirtualEmitter->poVecCurrentPosition_WS->v3;
				_oTempSphere.m_fRadius = poVirtualEmitter->fRadiusOuter;
				poVirtualEmitter->poAudioEmitter->MoveTracker( _oTempSphere );
				poVirtualEmitter       = (_VirtualEmitter_t *)flinklist_GetNext( poVirtualEmittersList, poVirtualEmitter );
			}
		}
		//
		////
	}
	else if( FWORLD_EVENT_WORLD_PREDESTROY == oeEvent )
	{
		//// Remove virtual listeners in the world.
		//
		for( uIndex = 0; uIndex < _uMaxVirtualListeners; ++uIndex )
		{
			_aoVirtualListeners[ uIndex ].poWorldUser->RemoveFromWorld();
		}
		//
		////

		FLinkRoot_t *poVirtualEmittersList;
		_VirtualEmitter_t *poVirtualEmitter;

		//// Remove active 3D virtual emitters in the world.
		for( uIndex = 0; uIndex < _uMaxPriorityLevels; ++uIndex ) {
			poVirtualEmittersList = &( _paoVirtualEmittersListActive3D[ uIndex ] );
			poVirtualEmitter      = (_VirtualEmitter_t *)flinklist_GetHead( poVirtualEmittersList );

			while( poVirtualEmitter ) {
				// ALBERT WAS REMOVING THE EMITTERS FROM THE WORLD, BUT NOT KILLING THE SOUNDS, DORK.
				// MOVIE QUOTE: "THEY MOVED THE HEADSTONES, BUT DIDN'T MOVE THE BODIES" - mike
				//poVirtualEmitter->poAudioEmitter->RemoveFromWorld();
				//poVirtualEmitter = (_VirtualEmitter_t *)flinklist_GetNext( poVirtualEmittersList, poVirtualEmitter );

				// The new way removes and stops all 3d sounds when the world is unloaded, this makes sense
				poVirtualEmitter->poAudioEmitter->Destroy();
				poVirtualEmitter = (_VirtualEmitter_t *)flinklist_GetHead( poVirtualEmittersList );
			}
		}
	}

	return TRUE;

} // _WorldLoadAndUnloadCallback

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void faudio_Work( void )
{
	if( ! FAudio_bModuleInstalled )
	{
		return;
	}

	////
	//
	_TrackEmittersProgress( _paoVirtualEmittersListActive2D );
	_TrackEmittersProgress( _paoVirtualEmittersListActive3D );
	//
	////

	u32 uIndex;

	//// Emitters and listeners work.
	//
	_fEmittersListenersWorkDelay += FLoop_fRealPreviousLoopSecs;

	if( ( ! _bSkipEmittersListenersWorkDelay ) || ( _EMITTERS_LISTENERS_WORK_DELAY < _fEmittersListenersWorkDelay ) )
	{
		FLinkRoot_t *poVirtualEmittersList;
		_VirtualListener_t *poVirtualListener;
		_VirtualEmitter_t *poVirtualEmitter;
		s32 nList;

		////
		//
		if( FWorld_pWorld )
		{
			//// Apply virtual listeners changes (position and velocity in world).
			//
			for( uIndex = 0; uIndex < _uActiveVirtualListeners; ++uIndex )
			{
				poVirtualListener = &( _aoVirtualListeners[ uIndex ] );

				if( _LISTENER_STATE_CHANGE_ORIENTATION & poVirtualListener->uStateChange )
				{
					// Listener velocity.
					*( poVirtualListener->poVecVelocity_WS ) = poVirtualListener->poXfmCurrentOrientation_WS->m_MtxF.m_vPos;
					poVirtualListener->poVecVelocity_WS->Sub( *( poVirtualListener->poVecPreviousPosition_WS ) );

					_oTempSphere.m_Pos     = poVirtualListener->poXfmCurrentOrientation_WS->m_MtxF.m_vPos.v3;
					_oTempSphere.m_fRadius = DS3D_DEFAULTMINDISTANCE;
					poVirtualListener->poWorldUser->MoveTracker( _oTempSphere );
				}
				else
				{
					poVirtualListener->poVecVelocity_WS->Zero();
				}
			}
			//
			////

			//// Update 3D virtual emitters position in world.
			//
			for( uIndex = 0; uIndex < _uMaxPriorityLevels; ++uIndex )
			{
				poVirtualEmittersList = &( _paoVirtualEmittersListActive3D[ uIndex ] );
				poVirtualEmitter      = (_VirtualEmitter_t *)flinklist_GetHead( poVirtualEmittersList );

				while( poVirtualEmitter )
				{
					//first, update the position if the listener is in a auto update mode...
					if( poVirtualEmitter->uProperties & _EMITTER_PROPERTIES_AUTOPOSUPDATE ) {
						poVirtualEmitter->poAudioEmitter->SetPosition( poVirtualEmitter->poVecAutoPosition_WS );
					}

					if( _EMITTER_STATE_CHANGE_POSITION & poVirtualEmitter->uStateChanges )
					{
						_oTempSphere.m_Pos     = poVirtualEmitter->poVecCurrentPosition_WS->v3;
						_oTempSphere.m_fRadius = poVirtualEmitter->fRadiusOuter;
						poVirtualEmitter->poAudioEmitter->MoveTracker( _oTempSphere );
					}

					poVirtualEmitter = (_VirtualEmitter_t *)flinklist_GetNext( poVirtualEmittersList, poVirtualEmitter );
				}
			}
			//
			////

			//// Listener and 3D emitter intersection.
			//
			for( nList = 0; nList < (s32)_uActiveVirtualListeners; ++nList )
			{
				_poTempVirtualListener = &( _aoVirtualListeners[ nList ] );
				_poTempVirtualListener->poWorldUser->FindIntersectingTrackers( _EmitterIntersectionCallback, FWORLD_TRACKERTYPE_USER );
			}

			// Intersection states.
			for( nList = 0; nList < (s32)_uMaxPriorityLevels; ++nList )
			{
				poVirtualEmittersList = &( _paoVirtualEmittersListActive3D[ nList ] );
				poVirtualEmitter      = (_VirtualEmitter_t *)flinklist_GetHead( poVirtualEmittersList );

				while( poVirtualEmitter )
				{
					if( ( _uTimeStamp - 2 ) > poVirtualEmitter->uTimeStampCurrent )
					{
						poVirtualEmitter = (_VirtualEmitter_t *)flinklist_GetNext( poVirtualEmittersList, poVirtualEmitter );
						continue;
					}

					if( ( _uTimeStamp - 2 ) == poVirtualEmitter->uTimeStampCurrent )
					{
						poVirtualEmitter->oeListenerIntersection = _LISTENER_INTERSECTION_NONE;
					}
					else if( ( _uTimeStamp - 1 ) == poVirtualEmitter->uTimeStampCurrent )
					{
						poVirtualEmitter->oeListenerIntersection = _LISTENER_INTERSECTION_EXITED;
					}
					else if( _uTimeStamp == poVirtualEmitter->uTimeStampCurrent )
					{
						if( poVirtualEmitter->uTimeStampPrevious != ( _uTimeStamp - 1 ) )
						{
							poVirtualEmitter->oeListenerIntersection = _LISTENER_INTERSECTION_ENTERED;
						}
						else
						{
							if( poVirtualEmitter->poVirtualListenerPrevious == poVirtualEmitter->poVirtualListenerCurrent )
							{
								poVirtualEmitter->oeListenerIntersection = _LISTENER_INTERSECTION_PRESENT;
							}
							else
							{
								poVirtualEmitter->oeListenerIntersection = _LISTENER_INTERSECTION_SWITCHED;
							}
						}
					}

					poVirtualEmitter->uTimeStampPrevious        = poVirtualEmitter->uTimeStampCurrent;
					poVirtualEmitter->poVirtualListenerPrevious = poVirtualEmitter->poVirtualListenerCurrent;

					poVirtualEmitter = (_VirtualEmitter_t *)flinklist_GetNext( poVirtualEmittersList, poVirtualEmitter );
				}
			}
			//
			////
		}
		//
		////

		//// Deallocate real emitters.
		//
		// 2D.
		for( nList = 0; nList < (s32)_uMaxPriorityLevels; ++nList )
		{
			poVirtualEmittersList = &( _paoVirtualEmittersListActive2D[ nList ] );
			poVirtualEmitter      = (_VirtualEmitter_t *)flinklist_GetHead( poVirtualEmittersList );

			while( poVirtualEmitter )
			{
				if( poVirtualEmitter->poRealEmitter )
				{
					if( _EMITTER_STATE_CHANGE_STOP & poVirtualEmitter->uStateChanges )
					{
						//// Stop.
						//
						poVirtualEmitter->poRealEmitter->poDSBuffer->Stop();
						FDX8_SAFE_RELEASE( poVirtualEmitter->poRealEmitter->poDSBuffer );
						flinklist_Remove( &_oRealEmittersListActive2D, poVirtualEmitter->poRealEmitter );
						flinklist_AddTail( &_oRealEmittersListFree2D, poVirtualEmitter->poRealEmitter );
						poVirtualEmitter->poRealEmitter = NULL;
						--( _paoRealEmittersLimits[ poVirtualEmitter->uPriority ].uPlaying );
						//
						////
					}
					else if( _EMITTER_STATE_CHANGE_PAUSE & poVirtualEmitter->uStateChanges )
					{
						//// Pause.
						//

						poVirtualEmitter->poRealEmitter->poDSBuffer->Stop();
/*
						FDX8_SAFE_RELEASE( poVirtualEmitter->poRealEmitter->poDSBuffer );
						flinklist_Remove( &_oRealEmittersListActive2D, poVirtualEmitter->poRealEmitter );
						flinklist_AddTail( &_oRealEmittersListFree2D, poVirtualEmitter->poRealEmitter );
						poVirtualEmitter->poRealEmitter = NULL;
						--( _paoRealEmittersLimits[ poVirtualEmitter->uPriority ].uPlaying );
*/

						poVirtualEmitter->uStateChanges = _EMITTER_STATE_CHANGE_NONE;
						//
						////
					}
				}

				poVirtualEmitter = (_VirtualEmitter_t *)flinklist_GetNext( poVirtualEmittersList, poVirtualEmitter );
			}
		}

		// 3D.
		for( nList = 0; nList < (s32)_uMaxPriorityLevels; ++nList )
		{
			poVirtualEmittersList = &( _paoVirtualEmittersListActive3D[ nList ] );
			poVirtualEmitter      = (_VirtualEmitter_t *)flinklist_GetHead( poVirtualEmittersList );

			while( poVirtualEmitter )
			{
				if( poVirtualEmitter->poRealEmitter )
				{
					if( _EMITTER_STATE_CHANGE_STOP & poVirtualEmitter->uStateChanges )
					{
						//// Stop.
						//
						poVirtualEmitter->poRealEmitter->poDSBuffer->Stop();
						FDX8_SAFE_RELEASE( poVirtualEmitter->poRealEmitter->poDS3DBuffer );
						FDX8_SAFE_RELEASE( poVirtualEmitter->poRealEmitter->poDSBuffer );
						flinklist_Remove( &_oRealEmittersListActive3D, poVirtualEmitter->poRealEmitter );
						flinklist_AddTail( &_oRealEmittersListFree3D, poVirtualEmitter->poRealEmitter );
						poVirtualEmitter->poRealEmitter = NULL;
						--( _paoRealEmittersLimits[ poVirtualEmitter->uPriority ].uPlaying );

						// remove the tracker from the world when you stop a 3d sound, MIKE
						poVirtualEmitter->poAudioEmitter->RemoveFromWorld();
						//
						////
					}
					else if( _EMITTER_STATE_CHANGE_PAUSE & poVirtualEmitter->uStateChanges )
					{
						//// Pause.
						//
						poVirtualEmitter->poRealEmitter->poDSBuffer->Stop();
/*
						FDX8_SAFE_RELEASE( poVirtualEmitter->poRealEmitter->poDS3DBuffer );
						FDX8_SAFE_RELEASE( poVirtualEmitter->poRealEmitter->poDSBuffer );
						flinklist_Remove( &_oRealEmittersListActive3D, poVirtualEmitter->poRealEmitter );
						flinklist_AddTail( &_oRealEmittersListFree3D, poVirtualEmitter->poRealEmitter );
						poVirtualEmitter->poRealEmitter = NULL;
						--( _paoRealEmittersLimits[ poVirtualEmitter->uPriority ].uPlaying );
*/
						poVirtualEmitter->uStateChanges = _EMITTER_STATE_CHANGE_NONE;
						//
						////
					}
				}

				if( poVirtualEmitter->poRealEmitter )
				{
					if( _LISTENER_INTERSECTION_EXITED == poVirtualEmitter->oeListenerIntersection )
					{
						//// Exited intersection.
						//
						poVirtualEmitter->poRealEmitter->poDSBuffer->Stop();
						FDX8_SAFE_RELEASE( poVirtualEmitter->poRealEmitter->poDS3DBuffer );
						FDX8_SAFE_RELEASE( poVirtualEmitter->poRealEmitter->poDSBuffer );
						flinklist_Remove( &_oRealEmittersListActive3D, poVirtualEmitter->poRealEmitter );
						flinklist_AddTail( &_oRealEmittersListFree3D, poVirtualEmitter->poRealEmitter );
						poVirtualEmitter->poRealEmitter = NULL;
						--( _paoRealEmittersLimits[ poVirtualEmitter->uPriority ].uPlaying );

						poVirtualEmitter->uStateChanges          = _EMITTER_STATE_CHANGE_NONE;
						poVirtualEmitter->oeListenerIntersection = _LISTENER_INTERSECTION_NONE;
						//
						////
					}
				}

				poVirtualEmitter = (_VirtualEmitter_t *)flinklist_GetNext( poVirtualEmittersList, poVirtualEmitter );
			}
		}
		//
		//// Deallocate real emitters.

		FLinkRoot_t *poVirtualEmittersListCull;
		_RealEmitter_t *poRealEmitter, *poRealEmitterCull;
		_VirtualEmitter_t *poVirtualEmitterCull;
		u32 uPriority;
		LPVOID pData;
		DWORD dwData;
		FDataWvbFile_Bank_t *poBank;
		FDataWvbFile_Wave_t *poWave;

		//// Allocate real emitters.
		//
		// 2D.
		_oBufferDescription.dwFlags = ( DSBCAPS_CTRLFREQUENCY | DSBCAPS_CTRLPAN | DSBCAPS_CTRLVOLUME | DSBCAPS_GLOBALFOCUS | DSBCAPS_LOCDEFER );
		for( nList = ( (s32)_uMaxPriorityLevels - 1 ); 0 <= nList ; --nList )
		{
			poVirtualEmittersList = &( _paoVirtualEmittersListActive2D[ nList ] );
			poVirtualEmitter      = (_VirtualEmitter_t *)flinklist_GetHead( poVirtualEmittersList );

			while( poVirtualEmitter )
			{
				if( ( _EMITTER_STATE_CHANGE_PLAY & poVirtualEmitter->uStateChanges ) &&
					( ! poVirtualEmitter->poRealEmitter ) )
				{
					uPriority = poVirtualEmitter->uPriority;

					if( _oRealEmittersListFree2D.nCount )
					{
						if( _paoRealEmittersLimits[ uPriority ].uPlaying < _paoRealEmittersLimits[ uPriority ].uPlayable )
						{
							poRealEmitter = (_RealEmitter_t *)flinklist_RemoveHead( &_oRealEmittersListFree2D );

							////
							//
							poWave = (FDataWvbFile_Wave_t *)poVirtualEmitter->oWaveHandle;
							poBank = (FDataWvbFile_Bank_t *)(poWave->oBankHandle);

							_oBufferDescription.dwBufferBytes = poWave->uLength;
							_oBufferDescription.lpwfxFormat   = (WAVEFORMATEX *)( (u32)poBank->pData + ( poWave->uIndex * sizeof( FDX8Data_WaveFormatEx_t ) ) );

							if( FAILED( _poDS->CreateSoundBuffer( &_oBufferDescription, (LPDIRECTSOUNDBUFFER *)&( poRealEmitter->poDSBuffer ), NULL ) ) )
							{
								DEVPRINTF( "[ FAUDIO ] Error %u: CreateSoundBuffer() failed !!!\n", __LINE__ );
								flinklist_AddHead( &_oRealEmittersListFree2D, poRealEmitter );
								poVirtualEmitter = (_VirtualEmitter_t *)flinklist_GetNext( poVirtualEmittersList, poVirtualEmitter );
								continue;
							}

							if( FAILED( poRealEmitter->poDSBuffer->Lock( 0, 0, &pData, &dwData, NULL, NULL, DSBLOCK_ENTIREBUFFER ) ) )
							{
								DEVPRINTF( "[ FAUDIO ] Error %u: Lock() failed !!!\n", __LINE__ );
								FDX8_SAFE_RELEASE( poRealEmitter->poDSBuffer );
								flinklist_AddHead( &_oRealEmittersListFree2D, poRealEmitter );
								poVirtualEmitter = (_VirtualEmitter_t *)flinklist_GetNext( poVirtualEmittersList, poVirtualEmitter );
								continue;
							}

							FASSERT( poWave->uLength == dwData );

							fang_MemCopy( pData, (void *)( (u32)poBank->pData + ( poBank->uWaves * sizeof( FDX8Data_WaveFormatEx_t ) ) + poWave->uOffset ), poWave->uLength );

							if( FAILED( poRealEmitter->poDSBuffer->Unlock( pData, poWave->uLength, NULL, 0 ) ) )
							{
								DEVPRINTF( "[ FAUDIO ] Error %u: Unlock() failed !!!\n", __LINE__ );
								FDX8_SAFE_RELEASE( poRealEmitter->poDSBuffer );
								flinklist_AddHead( &_oRealEmittersListFree2D, poRealEmitter );
								poVirtualEmitter = (_VirtualEmitter_t *)flinklist_GetNext( poVirtualEmittersList, poVirtualEmitter );
								continue;
							}
							//
							////

							flinklist_AddTail( &_oRealEmittersListActive2D, poRealEmitter );
							poVirtualEmitter->poRealEmitter  = poRealEmitter;
							poVirtualEmitter->uStateChanges |= ( _EMITTER_STATE_CHANGE_PLAY | _EMITTER_STATE_CHANGE_VOLUME | _EMITTER_STATE_CHANGE_FREQUENCY | _EMITTER_STATE_CHANGE_PAN );
							++( _paoRealEmittersLimits[ uPriority ].uPlaying );
						}
					}
					else
					{
						// Culling.
						poRealEmitterCull = NULL;
						for( uIndex = 0; ( ( uIndex < uPriority ) && ( ! poRealEmitterCull ) ); ++uIndex )
						{
							// Find lower priority emitter to cull.
							if( _paoRealEmittersLimits[ uIndex ].uPlaying )
							{
								poVirtualEmittersListCull = &( _paoVirtualEmittersListActive2D[ uIndex ] );
								poVirtualEmitterCull      = (_VirtualEmitter_t *)flinklist_GetTail( poVirtualEmittersListCull );

								do
								{
									if( poVirtualEmitterCull->poRealEmitter )
									{
										poRealEmitterCull = poVirtualEmitterCull->poRealEmitter;
										break;
									}

									poVirtualEmitterCull = (_VirtualEmitter_t *)flinklist_GetPrev( poVirtualEmittersListCull, poVirtualEmitterCull );

								} while( 1 );
							}
						}

						if( ( ! poRealEmitterCull ) && _paoRealEmittersLimits[ uPriority ].uPlaying )
						{
							// Find lower volume emitter to cull.
							poVirtualEmittersListCull = &( _paoVirtualEmittersListActive2D[ uPriority ] );
							poVirtualEmitterCull      = (_VirtualEmitter_t *)flinklist_GetTail( poVirtualEmittersListCull );

							do
							{
								if( poVirtualEmitterCull == poVirtualEmitter )
								{
									break;
								}

								if( poVirtualEmitterCull->poRealEmitter )
								{
									if( poVirtualEmitterCull->fVolumeDucked < poVirtualEmitter->fVolumeDucked )
									{
										poRealEmitterCull = poVirtualEmitterCull->poRealEmitter;
									}

									break;
								}

								poVirtualEmitterCull = (_VirtualEmitter_t *)flinklist_GetPrev( poVirtualEmittersListCull, poVirtualEmitterCull );

							} while( 1 );
						}

						if( poRealEmitterCull )
						{
							poRealEmitterCull->poDSBuffer->Stop();
							FDX8_SAFE_RELEASE( poRealEmitterCull->poDSBuffer );
							poVirtualEmitterCull->poRealEmitter = NULL;
							--( _paoRealEmittersLimits[ poVirtualEmitterCull->uPriority ].uPlaying );

							////
							//
							poWave = (FDataWvbFile_Wave_t *)poVirtualEmitter->oWaveHandle;
							poBank = (FDataWvbFile_Bank_t *)(poWave->oBankHandle);

							_oBufferDescription.dwBufferBytes = poWave->uLength;
							_oBufferDescription.lpwfxFormat   = (WAVEFORMATEX *)( (u32)poBank->pData + ( poWave->uIndex * sizeof( FDX8Data_WaveFormatEx_t ) ) );

							if( FAILED( _poDS->CreateSoundBuffer( &_oBufferDescription, (LPDIRECTSOUNDBUFFER *)&( poRealEmitterCull->poDSBuffer ), NULL ) ) )
							{
								DEVPRINTF( "[ FAUDIO ] Error %u: CreateSoundBuffer() failed !!!\n", __LINE__ );
								flinklist_Remove( &_oRealEmittersListActive2D, poRealEmitterCull );
								flinklist_AddTail( &_oRealEmittersListFree2D, poRealEmitterCull );
								poVirtualEmitter = (_VirtualEmitter_t *)flinklist_GetNext( poVirtualEmittersList, poVirtualEmitter );
								continue;
							}

							if( FAILED( poRealEmitterCull->poDSBuffer->Lock( 0, 0, &pData, &dwData, NULL, NULL, DSBLOCK_ENTIREBUFFER ) ) )
							{
								DEVPRINTF( "[ FAUDIO ] Error %u: Lock() failed !!!\n", __LINE__ );
								FDX8_SAFE_RELEASE( poRealEmitterCull->poDSBuffer );
								flinklist_Remove( &_oRealEmittersListActive2D, poRealEmitterCull );
								flinklist_AddTail( &_oRealEmittersListFree2D, poRealEmitterCull );
								poVirtualEmitter = (_VirtualEmitter_t *)flinklist_GetNext( poVirtualEmittersList, poVirtualEmitter );
								continue;
							}

							FASSERT( poWave->uLength == dwData );

							fang_MemCopy( pData, (void *)( (u32)poBank->pData + ( poBank->uWaves * sizeof( FDX8Data_WaveFormatEx_t ) ) + poWave->uOffset ), poWave->uLength );

							if( FAILED( poRealEmitterCull->poDSBuffer->Unlock( pData, poWave->uLength, NULL, 0 ) ) )
							{
								DEVPRINTF( "[ FAUDIO ] Error %u: Unlock() failed !!!\n", __LINE__ );
								FDX8_SAFE_RELEASE( poRealEmitterCull->poDSBuffer );
								flinklist_Remove( &_oRealEmittersListActive2D, poRealEmitterCull );
								flinklist_AddTail( &_oRealEmittersListFree2D, poRealEmitterCull );
								poVirtualEmitter = (_VirtualEmitter_t *)flinklist_GetNext( poVirtualEmittersList, poVirtualEmitter );
								continue;
							}
							//
							////

							poVirtualEmitter->poRealEmitter  = poRealEmitterCull;
							poVirtualEmitter->uStateChanges |= ( _EMITTER_STATE_CHANGE_PLAY | _EMITTER_STATE_CHANGE_VOLUME | _EMITTER_STATE_CHANGE_FREQUENCY | _EMITTER_STATE_CHANGE_PAN );
							++( _paoRealEmittersLimits[ poVirtualEmitter->uPriority ].uPlaying );
						}
					}
				}

				poVirtualEmitter = (_VirtualEmitter_t *)flinklist_GetNext( poVirtualEmittersList, poVirtualEmitter );
			}
		}

		if( FWorld_pWorld )
		{
			// 3D.
			_oBufferDescription.dwFlags = ( DSBCAPS_CTRL3D | DSBCAPS_CTRLFREQUENCY | DSBCAPS_CTRLVOLUME | DSBCAPS_GLOBALFOCUS | DSBCAPS_LOCDEFER | DSBCAPS_MUTE3DATMAXDISTANCE );
			for( nList = ( (s32)_uMaxPriorityLevels - 1 ); 0 <= nList ; --nList )
			{
				poVirtualEmittersList = &( _paoVirtualEmittersListActive3D[ nList ] );
				poVirtualEmitter      = (_VirtualEmitter_t *)flinklist_GetHead( poVirtualEmittersList );

				while( poVirtualEmitter )
				{
					if( ( _LISTENER_INTERSECTION_ENTERED == poVirtualEmitter->oeListenerIntersection ) &&
						( FAUDIO_EMITTER_STATE_PLAYING == poVirtualEmitter->oeState ) &&
						( ! poVirtualEmitter->poRealEmitter ) )
					{
						uPriority = poVirtualEmitter->uPriority;

						if( _oRealEmittersListFree3D.nCount )
						{
							if( _paoRealEmittersLimits[ uPriority ].uPlaying < _paoRealEmittersLimits[ uPriority ].uPlayable )
							{
								poRealEmitter = (_RealEmitter_t *)flinklist_RemoveHead( &_oRealEmittersListFree3D );

								////
								//
								poWave = (FDataWvbFile_Wave_t *)poVirtualEmitter->oWaveHandle;
								poBank = (FDataWvbFile_Bank_t *)(poWave->oBankHandle);

								_oBufferDescription.dwBufferBytes = poWave->uLength;
								_oBufferDescription.lpwfxFormat   = (WAVEFORMATEX *)( (u32)poBank->pData + ( poWave->uIndex * sizeof( FDX8Data_WaveFormatEx_t ) ) );

								if( FAILED( _poDS->CreateSoundBuffer( &_oBufferDescription, (LPDIRECTSOUNDBUFFER *)&( poRealEmitter->poDSBuffer ), NULL ) ) )
								{
									DEVPRINTF( "[ FAUDIO ] Error %u: CreateSoundBuffer() failed !!!\n", __LINE__ );
									flinklist_AddHead( &_oRealEmittersListFree3D, poRealEmitter );
									poVirtualEmitter = (_VirtualEmitter_t *)flinklist_GetNext( poVirtualEmittersList, poVirtualEmitter );
									continue;
								}

								if( FAILED( poRealEmitter->poDSBuffer->QueryInterface( IID_IDirectSound3DBuffer, (void **)&( poRealEmitter->poDS3DBuffer ) ) ) )
								{
									DEVPRINTF( "[ FAUDIO ] Error %u: QueryInterface() failed !!!\n", __LINE__ );
									FDX8_SAFE_RELEASE( poRealEmitter->poDSBuffer );
									flinklist_AddHead( &_oRealEmittersListFree3D, poRealEmitter );
									poVirtualEmitter = (_VirtualEmitter_t *)flinklist_GetNext( poVirtualEmittersList, poVirtualEmitter );
									continue;
								}

								if( FAILED( poRealEmitter->poDSBuffer->Lock( 0, 0, &pData, &dwData, NULL, NULL, DSBLOCK_ENTIREBUFFER ) ) )
								{
									DEVPRINTF( "[ FAUDIO ] Error %u: Lock() failed !!!\n", __LINE__ );
									FDX8_SAFE_RELEASE( poRealEmitter->poDS3DBuffer );
									FDX8_SAFE_RELEASE( poRealEmitter->poDSBuffer );
									flinklist_AddHead( &_oRealEmittersListFree3D, poRealEmitter );
									poVirtualEmitter = (_VirtualEmitter_t *)flinklist_GetNext( poVirtualEmittersList, poVirtualEmitter );
									continue;
								}

								FASSERT( poWave->uLength == dwData );

								fang_MemCopy( pData, (void *)( (u32)poBank->pData + ( poBank->uWaves * sizeof( FDX8Data_WaveFormatEx_t ) ) + poWave->uOffset ), poWave->uLength );

								if( FAILED( poRealEmitter->poDSBuffer->Unlock( pData, poWave->uLength, NULL, 0 ) ) )
								{
									DEVPRINTF( "[ FAUDIO ] Error %u: Unlock() failed !!!\n", __LINE__ );
									FDX8_SAFE_RELEASE( poRealEmitter->poDS3DBuffer );
									FDX8_SAFE_RELEASE( poRealEmitter->poDSBuffer );
									flinklist_AddHead( &_oRealEmittersListFree3D, poRealEmitter );
									poVirtualEmitter = (_VirtualEmitter_t *)flinklist_GetNext( poVirtualEmittersList, poVirtualEmitter );
									continue;
								}
								//
								////

								flinklist_AddTail( &_oRealEmittersListActive3D, poRealEmitter );
								poVirtualEmitter->poRealEmitter  = poRealEmitter;
//								poVirtualEmitter->uStateChanges |= ( _EMITTER_STATE_CHANGE_PLAY | _EMITTER_STATE_CHANGE_VOLUME | _EMITTER_STATE_CHANGE_FREQUENCY | _EMITTER_STATE_CHANGE_POSITION | _EMITTER_STATE_CHANGE_RADIUS | _EMITTER_STATE_CHANGE_DOPPLER | _EMITTER_STATE_CHANGE_REVERB );
								poVirtualEmitter->uStateChanges |= ( _EMITTER_STATE_CHANGE_PLAY | _EMITTER_STATE_CHANGE_VOLUME | _EMITTER_STATE_CHANGE_FREQUENCY | _EMITTER_STATE_CHANGE_POSITION | _EMITTER_STATE_CHANGE_DOPPLER | _EMITTER_STATE_CHANGE_REVERB );
								++( _paoRealEmittersLimits[ uPriority ].uPlaying );
							}
						}
						else
						{
							// Culling.
							poRealEmitterCull = NULL;
							for( uIndex = 0; ( ( uIndex < uPriority ) && ( ! poRealEmitterCull ) ); ++uIndex )
							{
								// Find lower priority emitter to cull.
								if( _paoRealEmittersLimits[ uIndex ].uPlaying )
								{
									poVirtualEmittersListCull = &( _paoVirtualEmittersListActive3D[ uIndex ] );
									poVirtualEmitterCull      = (_VirtualEmitter_t *)flinklist_GetTail( poVirtualEmittersListCull );

									do
									{
										if( poVirtualEmitterCull->poRealEmitter )
										{
											poRealEmitterCull = poVirtualEmitterCull->poRealEmitter;
											break;
										}

										poVirtualEmitterCull = (_VirtualEmitter_t *)flinklist_GetPrev( poVirtualEmittersListCull, poVirtualEmitterCull );

									} while( 1 );
								}
							}

							if( ( ! poRealEmitterCull ) && _paoRealEmittersLimits[ uPriority ].uPlaying )
							{
								// Find lower volume emitter to cull.
								poVirtualEmittersListCull = &( _paoVirtualEmittersListActive3D[ uPriority ] );
								poVirtualEmitterCull      = (_VirtualEmitter_t *)flinklist_GetTail( poVirtualEmittersListCull );

								do
								{
									if( poVirtualEmitterCull == poVirtualEmitter )
									{
										break;
									}

									if( poVirtualEmitterCull->poRealEmitter )
									{
										if( poVirtualEmitterCull->fVolumeDucked < poVirtualEmitter->fVolumeDucked )
										{
											poRealEmitterCull = poVirtualEmitterCull->poRealEmitter;
										}

										break;
									}

									poVirtualEmitterCull = (_VirtualEmitter_t *)flinklist_GetPrev( poVirtualEmittersListCull, poVirtualEmitterCull );

								} while( 1 );
							}

							if( poRealEmitterCull )
							{
								poRealEmitterCull->poDSBuffer->Stop();
								FDX8_SAFE_RELEASE( poRealEmitterCull->poDS3DBuffer );
								FDX8_SAFE_RELEASE( poRealEmitterCull->poDSBuffer );
								poVirtualEmitterCull->poRealEmitter = NULL;
								--( _paoRealEmittersLimits[ poVirtualEmitterCull->uPriority ].uPlaying );

								////
								//
								poWave = (FDataWvbFile_Wave_t *)poVirtualEmitter->oWaveHandle;
								poBank = (FDataWvbFile_Bank_t *)(poWave->oBankHandle);

								_oBufferDescription.dwBufferBytes = poWave->uLength;
								_oBufferDescription.lpwfxFormat   = (WAVEFORMATEX *)( (u32)poBank->pData + ( poWave->uIndex * sizeof( FDX8Data_WaveFormatEx_t ) ) );

								if( FAILED( _poDS->CreateSoundBuffer( &_oBufferDescription, (LPDIRECTSOUNDBUFFER *)&( poRealEmitterCull->poDSBuffer ), NULL ) ) )
								{
									DEVPRINTF( "[ FAUDIO ] Error %u: CreateSoundBuffer() failed !!!\n", __LINE__ );
									flinklist_Remove( &_oRealEmittersListActive3D, poRealEmitterCull );
									flinklist_AddTail( &_oRealEmittersListFree3D, poRealEmitterCull );
									poVirtualEmitter = (_VirtualEmitter_t *)flinklist_GetNext( poVirtualEmittersList, poVirtualEmitter );
									continue;
								}

								if( FAILED( poRealEmitterCull->poDSBuffer->QueryInterface( IID_IDirectSound3DBuffer, (void **)&( poRealEmitterCull->poDS3DBuffer ) ) ) )
								{
									DEVPRINTF( "[ FAUDIO ] Error %u: QueryInterface() failed !!!\n", __LINE__ );
									FDX8_SAFE_RELEASE( poRealEmitterCull->poDSBuffer );
									flinklist_Remove( &_oRealEmittersListActive3D, poRealEmitterCull );
									flinklist_AddTail( &_oRealEmittersListFree3D, poRealEmitterCull );
									poVirtualEmitter = (_VirtualEmitter_t *)flinklist_GetNext( poVirtualEmittersList, poVirtualEmitter );
									continue;
								}

								if( FAILED( poRealEmitterCull->poDSBuffer->Lock( 0, 0, &pData, &dwData, NULL, NULL, DSBLOCK_ENTIREBUFFER ) ) )
								{
									DEVPRINTF( "[ FAUDIO ] Error %u: Lock() failed !!!\n", __LINE__ );
									FDX8_SAFE_RELEASE( poRealEmitterCull->poDS3DBuffer );
									FDX8_SAFE_RELEASE( poRealEmitterCull->poDSBuffer );
									flinklist_Remove( &_oRealEmittersListActive3D, poRealEmitterCull );
									flinklist_AddTail( &_oRealEmittersListFree3D, poRealEmitterCull );
									poVirtualEmitter = (_VirtualEmitter_t *)flinklist_GetNext( poVirtualEmittersList, poVirtualEmitter );
									continue;
								}

								FASSERT( poWave->uLength == dwData );

								fang_MemCopy( pData, (void *)( (u32)poBank->pData + ( poBank->uWaves * sizeof( FDX8Data_WaveFormatEx_t ) ) + poWave->uOffset ), poWave->uLength );

								if( FAILED( poRealEmitterCull->poDSBuffer->Unlock( pData, poWave->uLength, NULL, 0 ) ) )
								{
									DEVPRINTF( "[ FAUDIO ] Error %u: Unlock() failed !!!\n", __LINE__ );
									FDX8_SAFE_RELEASE( poRealEmitterCull->poDS3DBuffer );
									FDX8_SAFE_RELEASE( poRealEmitterCull->poDSBuffer );
									flinklist_Remove( &_oRealEmittersListActive3D, poRealEmitterCull );
									flinklist_AddTail( &_oRealEmittersListFree3D, poRealEmitterCull );
									poVirtualEmitter = (_VirtualEmitter_t *)flinklist_GetNext( poVirtualEmittersList, poVirtualEmitter );
									continue;
								}
								//
								////

								poVirtualEmitter->poRealEmitter  = poRealEmitterCull;
//								poVirtualEmitter->uStateChanges |= ( _EMITTER_STATE_CHANGE_PLAY | _EMITTER_STATE_CHANGE_VOLUME | _EMITTER_STATE_CHANGE_FREQUENCY | _EMITTER_STATE_CHANGE_POSITION | _EMITTER_STATE_CHANGE_RADIUS | _EMITTER_STATE_CHANGE_DOPPLER | _EMITTER_STATE_CHANGE_REVERB );
								poVirtualEmitter->uStateChanges |= ( _EMITTER_STATE_CHANGE_PLAY | _EMITTER_STATE_CHANGE_VOLUME | _EMITTER_STATE_CHANGE_FREQUENCY | _EMITTER_STATE_CHANGE_POSITION | _EMITTER_STATE_CHANGE_DOPPLER | _EMITTER_STATE_CHANGE_REVERB );
								++( _paoRealEmittersLimits[ poVirtualEmitter->uPriority ].uPlaying );
							}
						}
					}

					poVirtualEmitter = (_VirtualEmitter_t *)flinklist_GetNext( poVirtualEmittersList, poVirtualEmitter );
				}
			}
		}
		//
		//// Allocate real emitters.

		////
		//
		_ApplyRealEmittersChanges( _paoVirtualEmittersListActive2D );
		if( FWorld_pWorld )
		{
			_fOOEmittersListenersWorkDelay = ( 1.0f / _fEmittersListenersWorkDelay );
			_ApplyRealEmittersChanges( _paoVirtualEmittersListActive3D );
		}
		//
		////

		//// Invoke end-of-play callbacks.
		//
		_InvokeEmittersEndofplayCallbacks( _paoVirtualEmittersListActive2D );
		_InvokeEmittersEndofplayCallbacks( _paoVirtualEmittersListActive3D );
		//
		////

		////
		//
		for( nList = 0; nList < (s32)_uMaxPriorityLevels; ++nList )
		{
			poVirtualEmittersList = &( _paoVirtualEmittersListActive2D[ nList ] );
			poVirtualEmitter      = (_VirtualEmitter_t *)flinklist_GetHead( poVirtualEmittersList );

			while( poVirtualEmitter )
			{
				poVirtualEmitter->uStateChanges = _EMITTER_STATE_CHANGE_NONE;

				poVirtualEmitter = (_VirtualEmitter_t *)flinklist_GetNext( poVirtualEmittersList, poVirtualEmitter );
			}
		}

		if( FWorld_pWorld )
		{
			//// Update previous positions of virtual listeners.
			//
			for( uIndex = 0; uIndex < _uActiveVirtualListeners; ++uIndex )
			{
				poVirtualListener = &( _aoVirtualListeners[ uIndex ] );

				if( _LISTENER_STATE_CHANGE_ORIENTATION & poVirtualListener->uStateChange )
				{
					*( poVirtualListener->poVecPreviousPosition_WS ) = poVirtualListener->poXfmCurrentOrientation_WS->m_MtxF.m_vPos;
				}

				poVirtualListener->uStateChange = _LISTENER_STATE_CHANGE_NONE;
			}
			//
			////

			//// Update previous positions of 3D virtual emitters.
			//
			for( nList = 0; nList < (s32)_uMaxPriorityLevels; ++nList )
			{
				poVirtualEmittersList = &( _paoVirtualEmittersListActive3D[ nList ] );
				poVirtualEmitter      = (_VirtualEmitter_t *)flinklist_GetHead( poVirtualEmittersList );

				while( poVirtualEmitter )
				{
					if( _EMITTER_STATE_CHANGE_POSITION & poVirtualEmitter->uStateChanges )
					{
						*( poVirtualEmitter->poVecPreviousPosition_WS ) = *( poVirtualEmitter->poVecCurrentPosition_WS );
					}

					poVirtualEmitter->uStateChanges = _EMITTER_STATE_CHANGE_NONE;

					poVirtualEmitter = (_VirtualEmitter_t *)flinklist_GetNext( poVirtualEmittersList, poVirtualEmitter );
				}
			}
			//
			////

			//// Apply all deffered changes.
			//
			_poDSRealListener->CommitDeferredSettings();
			//
			////
		}
		//
		////

		++_uTimeStamp;
		_fEmittersListenersWorkDelay     = 0.0f;
		_bSkipEmittersListenersWorkDelay = TRUE;
	}
	//
	//// Emitters and listeners work.

	//// Stream work.
	//
	_fStreamsWorkDelay += FLoop_fRealPreviousLoopSecs;

	if( ( ! _bSkipStreamsWorkDelay ) || ( _STREAMS_WORK_DELAY < _fStreamsWorkDelay ) )
	{
		for( uIndex = 0; uIndex < _uMaxStreams; ++uIndex )
		{
			_Stream_t *poStream = &( _paoStreams[ uIndex ] );

			if( ( ! poStream->bIsActive ) || ( FAUDIO_STREAM_STATE_ERROR == poStream->oeState ) )
			{
				continue;
			}

			if( poStream->bTreatAsSfx )
			{
				if( FAudio_bMasterSfxVolChanged )
				{
					poStream->uStateChanges |= _STREAM_STATE_CHANGE_VOLUME;
				}
			}
			else if( FAudio_bMasterMusicVolChanged )
			{
				poStream->uStateChanges |= _STREAM_STATE_CHANGE_VOLUME;
			}

			_StreamApplyChanges( poStream );

			if( FAUDIO_STREAM_STATE_PLAYING == poStream->oeState )
			{
				_StreamRefill( poStream );
			}
		}

		// Invoke end-of-play callbacks.
		for( uIndex = 0; uIndex < _uMaxStreams; ++uIndex )
		{
			_Stream_t *poStream = &( _paoStreams[ uIndex ] );

			if( ! poStream->bIsActive )
			{
				continue;
			}

			if( _STREAM_STATE_CHANGE_STOP & poStream->uStateChanges )
			{
				if( poStream->pEndOfPlayCallback )
				{
					poStream->pEndOfPlayCallback( poStream->poAudioStream );
				}
			}

			poStream->uStateChanges = _STREAM_STATE_CHANGE_NONE;
		}

		_bSkipStreamsWorkDelay = TRUE;
		_fStreamsWorkDelay     = 0.0f;
	}
	//
	//// Stream work.

	// reset the master volume change vars
	FAudio_bMasterSfxVolChanged = FALSE;
	FAudio_bMasterMusicVolChanged = FALSE;

} // faudio_Work

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

BOOL _EmitterIntersectionCallback( CFWorldTracker *poWorldTracker, FVisVolume_t *pVolume )
{
	if( poWorldTracker->m_nUser != FWORLD_USERTYPE_AUDIO_EMITTER_3D ) {
		return TRUE;
	}

	// NOTE : RAF - Added a check to see if the sound was paused because because if we return
	// from this function without updating poVirtualEmitter->uTimeStampCurrent, then the work 
	// function will deallocate our real emitter.  We don't want that because when we unpause 
	// the sound, it will start from the begining.  It's better to fool the audio system into 
	// keeping around the emitter even though it's not currently being played.  Then when it 
	// gets un-paused, the same sound buffer will be played and it will play from the point it
	// was paused at.
	_VirtualEmitter_t *poVirtualEmitter = (_VirtualEmitter_t *)poWorldTracker->m_pUser;
	if( ( poVirtualEmitter->oeState != FAUDIO_EMITTER_STATE_PLAYING ) && ( poVirtualEmitter->oeState != FAUDIO_EMITTER_STATE_PAUSED ) ) {
		return TRUE;
	}

	//_VirtualEmitter_t *poVirtualEmitter = (_VirtualEmitter_t *)poWorldTracker->m_pUser;

	//if( ( FAUDIO_EMITTER_STATE_PLAYING != poVirtualEmitter->oeState ) ||
	//	( FWORLD_USERTYPE_AUDIO_EMITTER_3D != poWorldTracker->m_nUser ) )
	//{
	//	return TRUE;
	//}

	_oTempVec3A = _poTempVirtualListener->poXfmCurrentOrientation_WS->m_MtxF.m_vPos;
	_oTempVec3A.Sub( *( poVirtualEmitter->poVecCurrentPosition_WS ) );

	f32 fTempDistanceSq = _oTempVec3A.MagSq();

	if( _uTimeStamp != poVirtualEmitter->uTimeStampCurrent )
	{
		poVirtualEmitter->uTimeStampCurrent          = _uTimeStamp;
		poVirtualEmitter->fVirtualListenerDistanceSq = fTempDistanceSq;
		poVirtualEmitter->poVirtualListenerCurrent   = _poTempVirtualListener;
	}
	else if( fTempDistanceSq < poVirtualEmitter->fVirtualListenerDistanceSq )
	{
		poVirtualEmitter->fVirtualListenerDistanceSq = fTempDistanceSq;
		poVirtualEmitter->poVirtualListenerCurrent   = _poTempVirtualListener;
	}

	return TRUE;

} // _EmitterIntersectionCallback

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

FAudio_PauseLevel_e CFAudioEmitter::SetGlobalPauseLevel( FAudio_PauseLevel_e eNewPauseLevel )
{
	if( ! FAudio_bModuleInstalled )
	{
		return eNewPauseLevel;
	}

	FAudio_PauseLevel_e eOldPauseLevel = _ePauseLevelEmitters;

	_VirtualEmitter_t *poVirtualEmitter;
	FLinkRoot_t *poVirtualEmittersList;

	//run through the list of emitters pausing and unpausing sounds based on the new
	//pause level
	for( u32 uIndex = 0; uIndex < _uMaxPriorityLevels; ++uIndex )
	{
		// 2D.
		poVirtualEmittersList = &( _paoVirtualEmittersListActive2D[ uIndex ] );
		poVirtualEmitter      = (_VirtualEmitter_t *)flinklist_GetHead( poVirtualEmittersList );

		while( poVirtualEmitter )
		{
			//check to see if we should pause or unpause this sound
			if( poVirtualEmitter->uPauseLevel <= eNewPauseLevel ) { 
				//this guy needs to be paused!
				poVirtualEmitter->poAudioEmitter->Pause( TRUE );
				poVirtualEmitter->uProperties |= _EMITTER_PROPERTIES_IGNOREINPAUSEMODE;
			} else if( poVirtualEmitter->uProperties & _EMITTER_PROPERTIES_IGNOREINPAUSEMODE ) {
				//this guy needs to be UNPAUSED!
				poVirtualEmitter->poAudioEmitter->Pause( FALSE );
				poVirtualEmitter->uProperties &= ~_EMITTER_PROPERTIES_IGNOREINPAUSEMODE;
			}
			poVirtualEmitter                     = (_VirtualEmitter_t *)flinklist_GetNext( poVirtualEmittersList, poVirtualEmitter );
		}

		// 3D.
		poVirtualEmittersList = &( _paoVirtualEmittersListActive3D[ uIndex ] );
		poVirtualEmitter      = (_VirtualEmitter_t *)flinklist_GetHead( poVirtualEmittersList );

		while( poVirtualEmitter )
		{
			if( poVirtualEmitter->uPauseLevel <= eNewPauseLevel ) { 
				//this guy needs to be paused!
				poVirtualEmitter->poAudioEmitter->Pause( TRUE );
				poVirtualEmitter->uProperties |= _EMITTER_PROPERTIES_IGNOREINPAUSEMODE;
			} else if( poVirtualEmitter->uProperties & _EMITTER_PROPERTIES_IGNOREINPAUSEMODE ) {
				//this guy needs to be UNPAUSED!
				poVirtualEmitter->poAudioEmitter->Pause( FALSE );
				poVirtualEmitter->uProperties &= ~_EMITTER_PROPERTIES_IGNOREINPAUSEMODE;
			}
			poVirtualEmitter                     = (_VirtualEmitter_t *)flinklist_GetNext( poVirtualEmittersList, poVirtualEmitter );
		}
	}

	_ePauseLevelEmitters = eNewPauseLevel;	

	return eOldPauseLevel;

} // CFAudioEmitter::PauseMode

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

FAudio_PauseLevel_e CFAudioEmitter::GetGlobalPauseLevel( void )
{
	if( ! FAudio_bModuleInstalled )
	{
		return FAUDIO_PAUSE_LEVEL_NONE;
	}

	return ( _ePauseLevelEmitters );

} // CFAudioEmitter::IsPauseModeActive

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void faudio_SetPlayableLimitPerPriorityLevel( u32 uPriorityLevel, u32 uMaxPlayable )
{
	if( ! FAudio_bModuleInstalled )
	{
		return;
	}

	FASSERT_MSG( ( _uMaxPriorityLevels > uPriorityLevel ), "[ FAUDIO ] Error: Invalid uPriorityLevel !!!" );

	_paoRealEmittersLimits[ uPriorityLevel ].uPlayable = uMaxPlayable;

} // faudio_SetPlayableLimitPerPriorityLevel

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

u32 faudio_GetPlayableLimitPerPriorityLevel( u32 uPriorityLevel )
{
	if( ! FAudio_bModuleInstalled )
	{
		return 0;
	}

	FASSERT_MSG( ( _uMaxPriorityLevels > uPriorityLevel ), "[ FAUDIO ] Error: Invalid uPriorityLevel !!!" );

	return _paoRealEmittersLimits[ uPriorityLevel ].uPlayable;

} // faudio_GetPlayableLimitPerPriorityLevel

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

u32 faudio_GetBankMemoryAvailable( void )
{
	if( ! FAudio_bModuleInstalled )
	{
		return 0;
	}

	return ( _uMaxSoundBytes - _uSoundBytes );

} // faudio_GetBankMemoryAvailable

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

u32 faudio_GetBankMemoryUsed( void )
{
	if( ! FAudio_bModuleInstalled )
	{
		return 0;
	}

	return _uSoundBytes;

} // faudio_GetBankMemoryUsed

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

FAudio_Error_e faudio_LoadBanks( cchar **papszNames, FAudio_BankHandle_t *paoBankHandles )
{
	if( ! FAudio_bModuleInstalled )
	{
		return FAUDIO_ERROR;
	}

	FASSERT_MSG( papszNames,        "[ FAUDIO ] Error: NULL pointer !!!" );
	FASSERT_MSG( *papszNames,       "[ FAUDIO ] Error: Zero length array !!!" );
	FASSERT_MSG( **papszNames,      "[ FAUDIO ] Error: Zero length string !!!" );
	FASSERT_MSG( paoBankHandles,    "[ FAUDIO ] Error: NULL pointer !!!" );

	cchar **pszName = papszNames;
	FAudio_BankHandle_t *poBankHandle = paoBankHandles;

	FResFrame_t oResFrame = fres_GetFrame();

	do
	{
		FASSERT_MSG( ( FAUDIO_MAX_ASSET_NAME_LENGTH >= fclib_strlen( *pszName ) ), "[ FAUDIO ] Error: Invalid bank name !!!" );

//		*poBankHandle = (FAudio_BankHandle_t)fresload_Load( FAUDIOBANK_RESTYPE, *pszName );
		*poBankHandle = faudio_LoadBank( *pszName );
		if( ! *poBankHandle )
		{
			DEVPRINTF( "[ FAUDIO ] Error %u: fresload_Load() failed !!!\n", __LINE__ );
			fres_ReleaseFrame( oResFrame );
			return FAUDIO_ERROR;
		}

		++pszName;
		++poBankHandle;

	} while( *pszName );

	return FAUDIO_NO_ERROR;

} // faudio_LoadBanks

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

FAudio_BankHandle_t faudio_LoadBank( cchar *pszName )
{
	if( ! FAudio_bModuleInstalled )
	{
		return FAUDIO_INVALID_HANDLE;
	}

	FASSERT_MSG( pszName,                                                     "[ FAUDIO ] Error: NULL pointer !!!" );
	FASSERT_MSG( *pszName,                                                    "[ FAUDIO ] Error: Zero length string !!!" );
	FASSERT_MSG( ( FAUDIO_MAX_ASSET_NAME_LENGTH >= fclib_strlen( pszName ) ), "[ FAUDIO ] Error: Invalid bank name !!!" );

	FAudio_BankHandle_t oBankHandle = FAUDIO_INVALID_HANDLE;
	FResFrame_t oResFrame = fres_GetFrame();

	// Determine if we should even try and load a localized version...
	char cLanguageChar, cAudioLanguageChar;
	ffile_GetLanguageChars( &cLanguageChar, &cAudioLanguageChar );
	if( cAudioLanguageChar != 0 ) 
	{
		// Try and load a localized version of the file...
		char szLocalizedFilename[ FAUDIO_MAX_ASSET_NAME_LENGTH + 1 ];
		fclib_strcpy( szLocalizedFilename, pszName );
		
		u32 nStringLen = fclib_strlen( szLocalizedFilename );
		if( nStringLen < FAUDIO_MAX_ASSET_NAME_LENGTH )
		{
			szLocalizedFilename[ nStringLen ] = cAudioLanguageChar;
			szLocalizedFilename[ nStringLen + 1 ] = 0x00;

			// Now, try and load this filename
			oBankHandle = (FAudio_BankHandle_t)fresload_Load( FAUDIOBANK_RESTYPE, szLocalizedFilename );
		}
	}
	
	if( !oBankHandle )
	{
		// Try and load a NON-LOCALIZED version of this audio bank
		oBankHandle = (FAudio_BankHandle_t)fresload_Load( FAUDIOBANK_RESTYPE, pszName );
	}

	if( ! oBankHandle )
	{
		// We have utterly failed... error out.
		DEVPRINTF( "[ FAUDIO ] Error %u: fresload_Load() failed !!!\n", __LINE__ );
		fres_ReleaseFrame( oResFrame );
		return FAUDIO_INVALID_HANDLE;
	}

	return oBankHandle;

} // faudio_LoadBank

BOOL faudio_IsValidBankHandle( FAudio_BankHandle_t oBankHandle )
{
	if( ! FAudio_bModuleInstalled )
	{
		return FALSE;
	}

	if( ( FAUDIO_INVALID_HANDLE == oBankHandle ) || ( (u32)fres_GetFrame() > (u32)oBankHandle ) )
	{
		return FALSE;
	}

	FDataWvbFile_Bank_t *poBank = (FDataWvbFile_Bank_t *)flinklist_GetHead( &_oWaveBanksList );
	while( poBank )
	{
		if( poBank == (FDataWvbFile_Bank_t *)oBankHandle )
		{
			return TRUE;
		}

		poBank = (FDataWvbFile_Bank_t *)flinklist_GetNext( &_oWaveBanksList, poBank );
	}

	return FALSE;

} // faudio_IsValidBankHandle

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

BOOL faudio_IsValidWaveHandle( FAudio_WaveHandle_t oWaveHandle )
{
	if( ! FAudio_bModuleInstalled )
	{
		return FALSE;
	}

	if( ( FAUDIO_INVALID_HANDLE == oWaveHandle ) || ( (u32)fres_GetFrame() > (u32)oWaveHandle ) )
	{
		return FALSE;
	}

	u32 uIndex;
	FDataWvbFile_Wave_t *poWave;
	FDataWvbFile_Bank_t *poBank = (FDataWvbFile_Bank_t *)flinklist_GetHead( &_oWaveBanksList );

	while( poBank )
	{
		poWave = (FDataWvbFile_Wave_t *)( (u32)poBank + sizeof( FDataWvbFile_Bank_t ) );
		for( uIndex = 0; uIndex < poBank->uWaves; ++uIndex, ++poWave )
		{
			if( poWave == (FDataWvbFile_Wave_t *)oWaveHandle )
			{
				return TRUE;
			}
		}

		poBank = (FDataWvbFile_Bank_t *)flinklist_GetNext( &_oWaveBanksList, poBank );
	}

	return FALSE;

} // faudio_IsValidWaveHandle

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void faudio_SetActiveListenerCount( u32 uCount )
{
	if( ! FAudio_bModuleInstalled )
	{
		return;
	}

	FASSERT_MSG( uCount,                              "[ FAUDIO ] Error: Invalid uCount !!!" );
	FASSERT_MSG( ( uCount <= _uMaxVirtualListeners ), "[ FAUDIO ] Error: Invalid uCount !!!" );

	_VirtualListener_t *poVirtualListener;

	_uActiveVirtualListeners = uCount;

	if( FWorld_pWorld )
	{
		_oTempSphere.m_Pos.Zero();
		_oTempSphere.m_fRadius = DS3D_DEFAULTMINDISTANCE;

		for( u32 uIndex = 0; uIndex < _uActiveVirtualListeners; ++uIndex )
		{
			poVirtualListener               = &( _aoVirtualListeners[ uIndex ] );
			poVirtualListener->uStateChange = _LISTENER_STATE_CHANGE_NONE;
			poVirtualListener->poXfmCurrentOrientation_WS->Identity();
			poVirtualListener->poVecPreviousPosition_WS->Zero();
			poVirtualListener->poVecVelocity_WS->Zero();

			poVirtualListener->poWorldUser->MoveTracker( _oTempSphere );
		}
	}
	else
	{
		for( u32 uIndex = 0; uIndex < _uActiveVirtualListeners; ++uIndex )
		{
			poVirtualListener               = &( _aoVirtualListeners[ uIndex ] );
			poVirtualListener->uStateChange = _LISTENER_STATE_CHANGE_NONE;
			poVirtualListener->poXfmCurrentOrientation_WS->Identity();
			poVirtualListener->poVecPreviousPosition_WS->Zero();
			poVirtualListener->poVecVelocity_WS->Zero();
		}
	}

} // faudio_SetActiveListenerCount

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

u32 faudio_GetActiveListenerCount( void )
{
	if( ! FAudio_bModuleInstalled )
	{
		return 0;
	}

	return _uActiveVirtualListeners;

} // faudio_GetActiveListenerCount

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void faudio_SetListenerOrientation( u32 uListenerIndex, const CFXfm *poXfmOrientation_WS )
{
	if( ! FAudio_bModuleInstalled )
	{
		return;
	}

	FASSERT_MSG( ( uListenerIndex < _uActiveVirtualListeners ), "[ FAUDIO ] Error: Invalid uCount !!!" );
	FASSERT_MSG( poXfmOrientation_WS,                           "[ FAUDIO ] Error: NULL pointer !!!" );

	_VirtualListener_t *poVirtualListener = &( _aoVirtualListeners[ uListenerIndex ] );

	const CFMtx43A *pMtx1 = &( poXfmOrientation_WS->m_MtxF );
	CFMtx43A *pMtx2 = &( poVirtualListener->poXfmCurrentOrientation_WS->m_MtxF );

	////
	//
	_oTempVec3A = pMtx1->m_vPos;
	_oTempVec3A.Sub( pMtx2->m_vPos );

	if( ( _SIGNIFICANT_DISTANCE_CHANGE_SQ < _oTempVec3A.MagSq() ) ||
		( _SIGNIFICANT_DOT_CHANGE > pMtx1->m_vFront.Dot( pMtx2->m_vFront ) ) ||
		( _SIGNIFICANT_DOT_CHANGE > pMtx1->m_vUp.Dot( pMtx2->m_vUp ) ) )
	{
		poVirtualListener->uStateChange                    |= _LISTENER_STATE_CHANGE_ORIENTATION;
		*( poVirtualListener->poXfmCurrentOrientation_WS )  = *( poXfmOrientation_WS );
	}
	//
	////

} // faudio_SetListenerOrientation

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void faudio_SetListenerOrientationAndMaintainDoppler( u32 uListenerIndex, const CFXfm *poXfmOrientation_WS )
{
	if( ! FAudio_bModuleInstalled )
	{
		return;
	}

	FASSERT_MSG( ( uListenerIndex < _uActiveVirtualListeners ), "[ FAUDIO ] Error: Invalid uCount !!!" );
	FASSERT_MSG( poXfmOrientation_WS,                           "[ FAUDIO ] Error: NULL pointer !!!" );

	_VirtualListener_t *poVirtualListener = &( _aoVirtualListeners[ uListenerIndex ] );

	const CFMtx43A *pMtx1 = &( poXfmOrientation_WS->m_MtxF );
	CFMtx43A *pMtx2 = &( poVirtualListener->poXfmCurrentOrientation_WS->m_MtxF );

	////
	//
	_oTempVec3A = pMtx1->m_vPos;
	_oTempVec3A.Sub( pMtx2->m_vPos );

	if( ( _SIGNIFICANT_DISTANCE_CHANGE_SQ < _oTempVec3A.MagSq() ) ||
		( _SIGNIFICANT_DOT_CHANGE > pMtx1->m_vFront.Dot( pMtx2->m_vFront ) ) ||
		( _SIGNIFICANT_DOT_CHANGE > pMtx1->m_vUp.Dot( pMtx2->m_vUp ) ) )
	{
		poVirtualListener->uStateChange |= _LISTENER_STATE_CHANGE_ORIENTATION;

		_oTempVec3A_Velocity = poVirtualListener->poXfmCurrentOrientation_WS->m_MtxF.m_vPos;
		_oTempVec3A_Velocity.Sub( *( poVirtualListener->poVecPreviousPosition_WS ) );
		_oTempVec3A = poXfmOrientation_WS->m_MtxF.m_vPos;
		_oTempVec3A.Sub( _oTempVec3A_Velocity );

		*( poVirtualListener->poVecPreviousPosition_WS )   = _oTempVec3A;
		*( poVirtualListener->poXfmCurrentOrientation_WS ) = *( poXfmOrientation_WS );
	}
	//
	////

} // faudio_SetListenerOrientationAndMaintainDoppler

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

CFAudioEmitter *CFAudioEmitter::Create2D( FAudio_WaveHandle_t oWaveHandle, u8 uPriority /* = FAudio_EmitterDefaultPriorityLevel */, CFAudioEmitter **ppUserAudioEmitter )
{
	if( ! FAudio_bModuleInstalled )
	{
		return NULL;
	}

	if( ! oWaveHandle )
	{
		if( _uEmittersCreateErrors2D )
		{
			DEVPRINTF( "[ FAUDIO ] Error %u: CFAudioEmitter::Create2D() failed !!!\n", __LINE__ );
			--_uEmittersCreateErrors2D;
			if( ! _uEmittersCreateErrors2D )
			{
				DEVPRINTF( "[ FAUDIO ] Warning %u: The previous error will not be issued anymore !!!\n", __LINE__ );
			}
		}
		return NULL;
	}

//	FASSERT_MSG( faudio_IsValidWaveHandle( oWaveHandle ), "[ FAUDIO ] Error: Invalid handle !!!" );
	FASSERT_MSG( ( _uMaxPriorityLevels > uPriority ),     "[ FAUDIO ] Error: Invalid uPriority !!!" );

	_VirtualEmitter_t *poVirtualEmitter = (_VirtualEmitter_t *)flinklist_RemoveHead( &_oVirtualEmittersListFree );
	if( poVirtualEmitter )
	{
		poVirtualEmitter->oWaveHandle                  = oWaveHandle;
		poVirtualEmitter->ppUserAudioEmitter           = ppUserAudioEmitter;
		poVirtualEmitter->uPriority                    = uPriority;
		poVirtualEmitter->fVolume                      = 1.0f;
		poVirtualEmitter->fVolumeDucked                = 1.0f;
		poVirtualEmitter->fPanLeftRight                = 0.0f;
		poVirtualEmitter->fFrequencyFactor             = 1.0f;
		poVirtualEmitter->fDopplerFactor               = 0.0f;
		poVirtualEmitter->fReverb                      = 0.0f;
		poVirtualEmitter->fSecondsPlayed               = 0.0f;
		poVirtualEmitter->fSecondsToPlay               = 0.0f;
		poVirtualEmitter->uProperties                  = _EMITTER_PROPERTIES_DUCKABLE;
		poVirtualEmitter->uPauseLevel				   = FAUDIO_PAUSE_LEVEL_1; //default pause level
		poVirtualEmitter->uStateChanges                = _EMITTER_STATE_CHANGE_NONE;
		poVirtualEmitter->oeState                      = FAUDIO_EMITTER_STATE_STOPPED;
		poVirtualEmitter->pEndOfPlayCallback           = NULL;
		poVirtualEmitter->poVirtualListenerCurrent     = NULL;
		poVirtualEmitter->poAudioEmitter->m_nUser      = FWORLD_USERTYPE_AUDIO_EMITTER_2D;
		poVirtualEmitter->paoVirtualEmittersListActive = &( _paoVirtualEmittersListActive2D[ uPriority ] ); // Optimization.

		if( ppUserAudioEmitter ) {
			*ppUserAudioEmitter = poVirtualEmitter->poAudioEmitter;
		}

		flinklist_AddTail( poVirtualEmitter->paoVirtualEmittersListActive, poVirtualEmitter );

		return poVirtualEmitter->poAudioEmitter;
	}
	else
	{
		if( _uEmittersCreateErrors2D )
		{
			DEVPRINTF( "[ FAUDIO ] Error %u: CFAudioEmitter::Create2D() failed !!!\n", __LINE__ );
			--_uEmittersCreateErrors2D;
			if( ! _uEmittersCreateErrors2D )
			{
				DEVPRINTF( "[ FAUDIO ] Warning %u: The previous error will not be issued anymore !!!\n", __LINE__ );
			}
		}
		return NULL;
	}

} // CFAudioEmitter::Create2D

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

CFAudioEmitter *CFAudioEmitter::Create3D( FAudio_WaveHandle_t oWaveHandle, u8 uPriority /* = FAudio_EmitterDefaultPriorityLevel */, const CFVec3A *poVecPosition_WS /* = NULL */, f32 fRadiusOuter /* = FAUDIO_MIN_RADIUS */, BOOL bAutoUpdatePosition /* = FALSE */, CFAudioEmitter **ppUserAudioEmitter )
{
	if( ! FAudio_bModuleInstalled )
	{
		return NULL;
	}

	if( ! oWaveHandle )
	{
		if( _uEmittersCreateErrors3D )
		{
			DEVPRINTF( "[ FAUDIO ] Error %u: CFAudioEmitter::Create3D() failed !!!\n", __LINE__ );
			--_uEmittersCreateErrors3D;
			if( ! _uEmittersCreateErrors3D )
			{
				DEVPRINTF( "[ FAUDIO ] Warning %u: The previous error will not be issued anymore !!!\n", __LINE__ );
			}
		}
		return NULL;
	}

//	FASSERT_MSG( faudio_IsValidWaveHandle( oWaveHandle ), "[ FAUDIO ] Error: Invalid handle !!!" );
	FASSERT_MSG( ( _uMaxPriorityLevels > uPriority ),     "[ FAUDIO ] Error: Invalid uPriority !!!" );
	FASSERT_MSG( ( DS3D_DEFAULTMINDISTANCE <= fRadiusOuter ),   "[ FAUDIO ] Error: Invalid fRadiusOuter !!!" );

	_VirtualEmitter_t *poVirtualEmitter = (_VirtualEmitter_t *)flinklist_RemoveHead( &_oVirtualEmittersListFree );
	if( poVirtualEmitter )
	{
		poVirtualEmitter->oWaveHandle                  = oWaveHandle;
		poVirtualEmitter->ppUserAudioEmitter           = ppUserAudioEmitter;
		poVirtualEmitter->uPriority                    = uPriority;
		poVirtualEmitter->uProperties				  |= _EMITTER_PROPERTIES_3D;
		poVirtualEmitter->fVolume                      = 1.0f;
		poVirtualEmitter->fVolumeDucked                = 1.0f;
		poVirtualEmitter->fPanLeftRight                = 0.0f;
		poVirtualEmitter->fFrequencyFactor             = 1.0f;
		poVirtualEmitter->fDopplerFactor               = 0.0f;
		poVirtualEmitter->fReverb                      = 0.0f;
		poVirtualEmitter->fSecondsPlayed               = 0.0f;
		poVirtualEmitter->fSecondsToPlay               = 0.0f;
		poVirtualEmitter->uProperties				   = ( _EMITTER_PROPERTIES_3D | _EMITTER_PROPERTIES_DUCKABLE );
		if( bAutoUpdatePosition ) {
			poVirtualEmitter->uProperties			  |= _EMITTER_PROPERTIES_AUTOPOSUPDATE;
		}             
		poVirtualEmitter->uPauseLevel 				   = FAUDIO_PAUSE_LEVEL_1; //default pause level
		poVirtualEmitter->uStateChanges                = _EMITTER_STATE_CHANGE_NONE;
		poVirtualEmitter->oeState                      = FAUDIO_EMITTER_STATE_STOPPED;
		poVirtualEmitter->pEndOfPlayCallback           = NULL;
		poVirtualEmitter->poVirtualListenerCurrent     = NULL;
		poVirtualEmitter->fRadiusOuter                 = fRadiusOuter;
		poVirtualEmitter->fRadiusInner                 = DS3D_DEFAULTMINDISTANCE;
		poVirtualEmitter->poAudioEmitter->m_nUser      = FWORLD_USERTYPE_AUDIO_EMITTER_3D;
		poVirtualEmitter->paoVirtualEmittersListActive = &( _paoVirtualEmittersListActive3D[ uPriority ] ); // Optimization.

		if( poVecPosition_WS )
		{
			*( poVirtualEmitter->poVecPreviousPosition_WS ) = *( poVirtualEmitter->poVecCurrentPosition_WS ) = *( poVecPosition_WS );
			if( bAutoUpdatePosition ) {
				poVirtualEmitter->poVecAutoPosition_WS = poVecPosition_WS;
			}
		}
		else
		{
			FASSERT( !bAutoUpdatePosition ); //Doesn't make sense to get autoupdate and not pass in a ws pointer
			poVirtualEmitter->poVecAutoPosition_WS = poVirtualEmitter->poVecCurrentPosition_WS;
			poVirtualEmitter->poVecPreviousPosition_WS->Zero();
			poVirtualEmitter->poVecCurrentPosition_WS->Zero();
		}

		//// Insert 3D virtual emitters in the world.
		//
		if( FWorld_pWorld )
		{
			_oTempSphere.m_Pos     = poVirtualEmitter->poVecCurrentPosition_WS->v3;
			_oTempSphere.m_fRadius = poVirtualEmitter->fRadiusOuter;
			poVirtualEmitter->poAudioEmitter->MoveTracker( _oTempSphere );
		}
		//
		////

		if( ppUserAudioEmitter ) {
			*ppUserAudioEmitter = poVirtualEmitter->poAudioEmitter;
		}

		flinklist_AddTail( poVirtualEmitter->paoVirtualEmittersListActive, poVirtualEmitter );

		return poVirtualEmitter->poAudioEmitter;
	}
	else
	{
		if( _uEmittersCreateErrors3D )
		{
			DEVPRINTF( "[ FAUDIO ] Error %u: CFAudioEmitter::Create3D() failed !!!\n", __LINE__ );
			--_uEmittersCreateErrors3D;
			if( ! _uEmittersCreateErrors3D )
			{
				DEVPRINTF( "[ FAUDIO ] Warning %u: The previous error will not be issued anymore !!!\n", __LINE__ );
			}
		}
		return NULL;
	}

} // CFAudioEmitter::Create3D

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void CFAudioEmitter::Destroy( void )
{
	FASSERT_MSG( FAudio_bModuleInstalled, "[ FAUDIO ] Error: System not installed !!!" );

	_VirtualEmitter_t *poVirtualEmitter = (_VirtualEmitter_t *)m_pUser;

	if (!poVirtualEmitter) return;

	if( poVirtualEmitter->ppUserAudioEmitter ) {
		*poVirtualEmitter->ppUserAudioEmitter = NULL;
		poVirtualEmitter->ppUserAudioEmitter = NULL;
	}

	if( FWORLD_USERTYPE_AUDIO_EMITTER_INACTIVE == poVirtualEmitter->poAudioEmitter->m_nUser )
	{
		return;
	}

	poVirtualEmitter->poAudioEmitter->m_nUser = FWORLD_USERTYPE_AUDIO_EMITTER_INACTIVE;

	if( poVirtualEmitter->poRealEmitter )
	{
		if( FAUDIO_EMITTER_STATE_PLAYING == poVirtualEmitter->oeState )
		{
			if( poVirtualEmitter->pEndOfPlayCallback )
			{
				poVirtualEmitter->pEndOfPlayCallback( poVirtualEmitter->poAudioEmitter );
			}

			poVirtualEmitter->poRealEmitter->poDSBuffer->Stop();
			FDX8_SAFE_RELEASE( poVirtualEmitter->poRealEmitter->poDS3DBuffer );
			FDX8_SAFE_RELEASE( poVirtualEmitter->poRealEmitter->poDSBuffer );
			--( _paoRealEmittersLimits[ poVirtualEmitter->uPriority ].uPlaying );
		}

		if( poVirtualEmitter->uProperties & _EMITTER_PROPERTIES_3D )
		{
			flinklist_Remove( &( _oRealEmittersListActive3D ), poVirtualEmitter->poRealEmitter );
			flinklist_AddTail( &( _oRealEmittersListFree3D ), poVirtualEmitter->poRealEmitter );

			//// Remove 3D virtual emitters in the world.
			//
			poVirtualEmitter->poAudioEmitter->RemoveFromWorld();
			//
			////
		}
		else
		{
			flinklist_Remove( &( _oRealEmittersListActive2D ), poVirtualEmitter->poRealEmitter );
			flinklist_AddTail( &( _oRealEmittersListFree2D ), poVirtualEmitter->poRealEmitter );
		}

		poVirtualEmitter->poRealEmitter = NULL;
	}

	flinklist_Remove( poVirtualEmitter->paoVirtualEmittersListActive, poVirtualEmitter );
	flinklist_AddTail( &_oVirtualEmittersListFree, poVirtualEmitter );

} // CFAudioEmitter::Destroy

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void CFAudioEmitter::DestroyAll( void )
{
	if( ! FAudio_bModuleInstalled )
	{
		return;
	}

	_VirtualEmitter_t *poVirtualEmitter;
	FLinkRoot_t *poVirtualEmittersList;

	for( u32 uIndex = 0; uIndex < _uMaxPriorityLevels; ++uIndex )
	{
		// 2D.
		poVirtualEmittersList = &( _paoVirtualEmittersListActive2D[ uIndex ] );

		while( poVirtualEmittersList->nCount )
		{
			poVirtualEmitter = (_VirtualEmitter_t *)flinklist_GetHead( poVirtualEmittersList );

			if( poVirtualEmitter ) {
				poVirtualEmitter->poAudioEmitter->Destroy();
			} else {
				DEVPRINTF( "fdx8audio() : The 2d emitter linklist is corupt, re-initing the link root, count = %d.\n", poVirtualEmittersList->nCount );
				flinklist_InitRoot( &( _paoVirtualEmittersListActive2D[ uIndex ] ), (s32)FANG_OFFSETOF( _VirtualEmitter_t, oLink ) );
				break;
			}
		}

		// 3D.
		poVirtualEmittersList = &( _paoVirtualEmittersListActive3D[ uIndex ] );

		while( poVirtualEmittersList->nCount )
		{
			poVirtualEmitter = (_VirtualEmitter_t *)flinklist_GetHead( poVirtualEmittersList );

			if( poVirtualEmitter ) {
				poVirtualEmitter->poAudioEmitter->Destroy();
			} else {
				DEVPRINTF( "fdx8audio() : The 3d emitter linklist is corupt, re-initing the link root, count = %d.\n", poVirtualEmittersList->nCount );
				flinklist_InitRoot( &( _paoVirtualEmittersListActive3D[ uIndex ] ), (s32)FANG_OFFSETOF( _VirtualEmitter_t, oLink ) );
				break;
			}
		}
	}

} // CFAudioEmitter::DestroyAll

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void CFAudioEmitter::SetPosition( const CFVec3A *poVecPosition_WS )
{
	FASSERT_MSG( FAudio_bModuleInstalled,                                  "[ FAUDIO ] Error: System not installed !!!" );
	FASSERT_MSG( poVecPosition_WS,                                   "[ FAUDIO ] Error: NULL pointer !!!" );
	FASSERT_MSG( ((_VirtualEmitter_t *)m_pUser)->uProperties & _EMITTER_PROPERTIES_3D, "[ FAUDIO ] Error: Illegal call !!!" );

	////
	//
	_VirtualEmitter_t *poVirtualEmitter = (_VirtualEmitter_t *)m_pUser;

	FASSERT_MSG( ( FWORLD_USERTYPE_AUDIO_EMITTER_INACTIVE != poVirtualEmitter->poAudioEmitter->m_nUser ), "[ FAUDIO ] Error: Invalid emitter !!!" );
	//
	////

	if( ( poVirtualEmitter->uProperties & _EMITTER_PROPERTIES_AUTOPOSUPDATE ) && 
	    ( poVirtualEmitter->poVecAutoPosition_WS != poVecPosition_WS ) ) {
		//update the autopos pointer to this one...
		poVirtualEmitter->poVecAutoPosition_WS = poVecPosition_WS;
	}

	////
	//
	_oTempVec3A = *poVecPosition_WS;
	_oTempVec3A.Sub( *( poVirtualEmitter->poVecCurrentPosition_WS ) );

	if( _SIGNIFICANT_DISTANCE_CHANGE_SQ < _oTempVec3A.MagSq() )
	{
		poVirtualEmitter->uStateChanges                |= _EMITTER_STATE_CHANGE_POSITION;
		*( poVirtualEmitter->poVecCurrentPosition_WS )  = *poVecPosition_WS;
	}
	//
	////

} // CFAudioEmitter::SetPosition

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void CFAudioEmitter::SetPositionAndMaintainDoppler( const CFVec3A *poVecPosition_WS )
{
	FASSERT_MSG( FAudio_bModuleInstalled,                                  "[ FAUDIO ] Error: System not installed !!!" );
	FASSERT_MSG( poVecPosition_WS,                                   "[ FAUDIO ] Error: NULL pointer !!!" );
	FASSERT_MSG( ((_VirtualEmitter_t *)m_pUser)->uProperties & _EMITTER_PROPERTIES_3D, "[ FAUDIO ] Error: Illegal call !!!" );

	////
	//
	_VirtualEmitter_t *poVirtualEmitter = (_VirtualEmitter_t *)m_pUser;

	FASSERT_MSG( ( FWORLD_USERTYPE_AUDIO_EMITTER_INACTIVE != poVirtualEmitter->poAudioEmitter->m_nUser ), "[ FAUDIO ] Error: Invalid emitter !!!" );
	//
	////

	if( ( poVirtualEmitter->uProperties & _EMITTER_PROPERTIES_AUTOPOSUPDATE ) && 
	    ( poVirtualEmitter->poVecAutoPosition_WS != poVecPosition_WS ) ) {
		//update the autopos pointer to this one...
		poVirtualEmitter->poVecAutoPosition_WS = poVecPosition_WS;
	}

	////
	//
	_oTempVec3A = *poVecPosition_WS;
	_oTempVec3A.Sub( *( poVirtualEmitter->poVecCurrentPosition_WS ) );

	if( _SIGNIFICANT_DISTANCE_CHANGE_SQ < _oTempVec3A.MagSq() )
	{
		poVirtualEmitter->uStateChanges |= _EMITTER_STATE_CHANGE_POSITION;

		_oTempVec3A_Velocity = *( poVirtualEmitter->poVecCurrentPosition_WS );
		_oTempVec3A_Velocity.Sub( *( poVirtualEmitter->poVecPreviousPosition_WS ) );
		_oTempVec3A = *( poVecPosition_WS );
		_oTempVec3A.Sub( _oTempVec3A_Velocity );

		*( poVirtualEmitter->poVecPreviousPosition_WS ) = _oTempVec3A;
		*( poVirtualEmitter->poVecCurrentPosition_WS )  = *( poVecPosition_WS );
	}

} // CFAudioEmitter::SetPositionAndMaintainDoppler

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void CFAudioEmitter::SetRadius( f32 fRadiusOuter, f32 fRadiusInner /* = DS3D_DEFAULTMINDISTANCE */ )
{
	FASSERT_MSG( FAudio_bModuleInstalled,                                  "[ FAUDIO ] Error: System not installed !!!" );
	FASSERT_MSG( ((_VirtualEmitter_t *)m_pUser)->uProperties & _EMITTER_PROPERTIES_3D, "[ FAUDIO ] Error: Illegal call !!!" );
	FASSERT_MSG( ( fRadiusInner <= fRadiusOuter ),                   "[ FAUDIO ] Error: Invalid fRadiusOuter !!!" );
	FASSERT_MSG( ( DS3D_DEFAULTMINDISTANCE <= fRadiusInner ),              "[ FAUDIO ] Error: Invalid fRadiusInner !!!" );

	////
	//
	_VirtualEmitter_t *poVirtualEmitter = (_VirtualEmitter_t *)m_pUser;

	FASSERT_MSG( ( FWORLD_USERTYPE_AUDIO_EMITTER_INACTIVE != poVirtualEmitter->poAudioEmitter->m_nUser ), "[ FAUDIO ] Error: Invalid emitter !!!" );
	//
	////

	////
	//
	f32 fTemp1 = ( poVirtualEmitter->fRadiusOuter - fRadiusOuter );
	fTemp1 = ( fTemp1 * fTemp1 );
/*
	f32 fTemp2 = ( poVirtualEmitter->fRadiusInner - fRadiusInner );
	fTemp2 = ( fTemp2 * fTemp2 );

	if( ( _SIGNIFICANT_DISTANCE_CHANGE_SQ < fTemp1 ) ||
		( _SIGNIFICANT_DISTANCE_CHANGE_SQ < fTemp2 ) )
*/
	if( _SIGNIFICANT_DISTANCE_CHANGE_SQ < fTemp1 )
	{
//		poVirtualEmitter->uStateChanges |= _EMITTER_STATE_CHANGE_RADIUS;
		poVirtualEmitter->fRadiusOuter   = fRadiusOuter;
//		poVirtualEmitter->fRadiusInner   = fRadiusInner;
	}
	//
	////

} // CFAudioEmitter::SetRadius

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void CFAudioEmitter::SetVolume( f32 fVolume )
{
	FASSERT_MSG( FAudio_bModuleInstalled,              "[ FAUDIO ] Error: System not installed !!!" );
	FASSERT_MSG( FMATH_IS_UNIT_FLOAT( fVolume ), "[ FAUDIO ] Error: Invalid fVolume !!!" );

	////
	//
	_VirtualEmitter_t *poVirtualEmitter = (_VirtualEmitter_t *)m_pUser;

	FASSERT_MSG( ( FWORLD_USERTYPE_AUDIO_EMITTER_INACTIVE != poVirtualEmitter->poAudioEmitter->m_nUser ), "[ FAUDIO ] Error: Invalid emitter !!!" );

#if !FANG_PRODUCTION_BUILD
	FASSERT(flinklist_IsLinkInList(poVirtualEmitter->paoVirtualEmittersListActive, flinklist_GetLinkPointer(poVirtualEmitter->paoVirtualEmittersListActive, poVirtualEmitter)));
#endif

// Windows 2D "loudness" hack (fVolume *= 0.1f) removed: it drove player-owned
// weapons, possessed-bot SFX, HUD transmissions and all dialog ~20dB below
// world/AI 3D sounds and below Bink cutscene audio.  Authored CSV fVolume2D
// plus the master SFX volume are the intended mix controls.
	//
	////

	////
	//
	f32 fTemp = ( fVolume - poVirtualEmitter->fVolume );
	BOOL bVolumeIncreased = ( fTemp >= 0.0f ) ? TRUE : FALSE;

	fTemp = ( fTemp * fTemp );
	if( ( ! ( ( _EMITTER_STATE_CHANGE_DUCKABLE | _EMITTER_STATE_CHANGE_DUCKING ) & poVirtualEmitter->uStateChanges ) ) && // Ducking state change.
		( _SIGNIFICANT_VOLUME_CHANGE_SQ > fTemp ) )
	{
		// Indistinguishable difference.
		return;
	}
	//
	////

	////
	//
	poVirtualEmitter->uStateChanges |= _EMITTER_STATE_CHANGE_VOLUME;
	poVirtualEmitter->fVolume        = fVolume;
	if( poVirtualEmitter->uProperties & _EMITTER_PROPERTIES_DUCKABLE )
	{
		poVirtualEmitter->fVolumeDucked = fVolume * _fDuckingFactor;
	}
	else
	{
		poVirtualEmitter->fVolumeDucked = fVolume;
	}
	//
	////

	//// Nothing to sort.
	//
	if( 1 == poVirtualEmitter->paoVirtualEmittersListActive->nCount )
	{
		poVirtualEmitter->uStateChanges &= ( ~ ( _EMITTER_STATE_CHANGE_DUCKABLE | _EMITTER_STATE_CHANGE_DUCKING ) );
		return;
	}

	if( _EMITTER_STATE_CHANGE_DUCKING & poVirtualEmitter->uStateChanges )
	{
		poVirtualEmitter->uStateChanges &= ( ~ ( _EMITTER_STATE_CHANGE_DUCKABLE | _EMITTER_STATE_CHANGE_DUCKING ) );
		return;
	}
	//
	////

	//// Keep list in volume order.
	//
	_VirtualEmitter_t *poVirtualEmitterIterator;
	fTemp = poVirtualEmitter->fVolumeDucked;
	FLinkRoot_t *poList = poVirtualEmitter->paoVirtualEmittersListActive;

	if( bVolumeIncreased )
	{
		// Look to move this element to the left.
		poVirtualEmitterIterator = (_VirtualEmitter_t *)flinklist_GetPrev( poList, poVirtualEmitter );
		flinklist_Remove( poList, poVirtualEmitter );

		while( poVirtualEmitterIterator )
		{
			// See if our volume is less than the previous one.
			if( poVirtualEmitterIterator->fVolumeDucked >= fTemp )
			{
				break;
			}

			// Get the next item to the left.
			poVirtualEmitterIterator = (_VirtualEmitter_t *)flinklist_GetPrev( poList, poVirtualEmitterIterator );
		}

		// Insert after poVirtualEmitterIterator.
		if( poVirtualEmitterIterator )
		{
			flinklist_AddAfter( poList, poVirtualEmitterIterator, poVirtualEmitter );
		}
		else
		{
			flinklist_AddHead( poList, poVirtualEmitter );
		}
	}
	else
	{
		// Look to move this element to the right.
		poVirtualEmitterIterator = (_VirtualEmitter_t *)flinklist_GetNext( poList, poVirtualEmitter );
		flinklist_Remove( poList, poVirtualEmitter );

		while( poVirtualEmitterIterator )
		{
			// See if our volume is greater than the previous one.
			if( poVirtualEmitterIterator->fVolumeDucked <= fTemp )
			{
				break;
			}

			// Get the next item to the right.
			poVirtualEmitterIterator = (_VirtualEmitter_t *)flinklist_GetNext( poList, poVirtualEmitterIterator );
		}

		// Insert before poVirtualEmitterIterator.
		if( poVirtualEmitterIterator )
		{
			flinklist_AddBefore( poList, poVirtualEmitterIterator, poVirtualEmitter );
		}
		else
		{
			flinklist_AddTail( poList, poVirtualEmitter );
		}
	}
	//
	////

} // CFAudioEmitter::SetVolume

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void CFAudioEmitter::SetPan( f32 fPanLeftRight )
{
	FASSERT_MSG( FAudio_bModuleInstalled,                                        "[ FAUDIO ] Error: System not installed !!!" );
	FASSERT_MSG( ( ! (((_VirtualEmitter_t *)m_pUser)->uProperties & _EMITTER_PROPERTIES_3D) ), "[ FAUDIO ] Error: Illegal call !!!" );
	FASSERT_MSG( FMATH_IS_BIPOLAR_UNIT_FLOAT( fPanLeftRight ),             "[ FAUDIO ] Error: Invalid fPanLeftRight !!!" );

	////
	//
	_VirtualEmitter_t *poVirtualEmitter = (_VirtualEmitter_t *)m_pUser;

	FASSERT_MSG( ( FWORLD_USERTYPE_AUDIO_EMITTER_INACTIVE != poVirtualEmitter->poAudioEmitter->m_nUser ), "[ FAUDIO ] Error: Invalid emitter !!!" );
	//
	////

	////
	//
	f32 fTemp = ( poVirtualEmitter->fPanLeftRight - fPanLeftRight );
	fTemp = ( fTemp * fTemp );

	if( _SIGNIFICANT_VOLUME_CHANGE_SQ < fTemp )
	{
		poVirtualEmitter->uStateChanges |= _EMITTER_STATE_CHANGE_PAN;
		poVirtualEmitter->fPanLeftRight  = fPanLeftRight;
	}
	//
	////

} // CFAudioEmitter::SetPan

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void CFAudioEmitter::SetEndOfPlayCallback( FAudio_EmitterEndOfPlayCallback_t *pEndOfPlayCallback )
{
	FASSERT_MSG( FAudio_bModuleInstalled,  "[ FAUDIO ] Error: System not installed !!!" );
	FASSERT_MSG( pEndOfPlayCallback, "[ FAUDIO ] Error: NULL pointer !!!" );

	////
	//
	_VirtualEmitter_t *poVirtualEmitter = (_VirtualEmitter_t *)m_pUser;

	FASSERT_MSG( ( FWORLD_USERTYPE_AUDIO_EMITTER_INACTIVE != poVirtualEmitter->poAudioEmitter->m_nUser ), "[ FAUDIO ] Error: Invalid emitter !!!" );
	//
	////

	poVirtualEmitter->pEndOfPlayCallback = pEndOfPlayCallback;

} // CFAudioEmitter::SetEndOfPlayCallback

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void CFAudioEmitter::SetFrequencyFactor( f32 fFrequencyFactor )
{
	FASSERT_MSG( FAudio_bModuleInstalled,            "[ FAUDIO ] Error: System not installed !!!" );
	FASSERT_MSG( ( 0.0f <= fFrequencyFactor ), "[ FAUDIO ] Error: Invalid fFrequencyFactor !!!" );

	////
	//
	_VirtualEmitter_t *poVirtualEmitter = (_VirtualEmitter_t *)m_pUser;

	FASSERT_MSG( ( FWORLD_USERTYPE_AUDIO_EMITTER_INACTIVE != poVirtualEmitter->poAudioEmitter->m_nUser ), "[ FAUDIO ] Error: Invalid emitter !!!" );
	//
	////

	////
	//
	f32 fTemp = ( poVirtualEmitter->fFrequencyFactor - fFrequencyFactor );
	fTemp = ( fTemp * fTemp );

	if( _SIGNIFICANT_FACTOR_CHANGE_SQ < fTemp )
	{
		poVirtualEmitter->uStateChanges    |= _EMITTER_STATE_CHANGE_FREQUENCY;
		poVirtualEmitter->fFrequencyFactor  = fFrequencyFactor;
	}
	//
	////

} // CFAudioEmitter::SetFrequencyFactor

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void CFAudioEmitter::SetDopplerFactor( f32 fDopplerFactor )
{
	FASSERT_MSG( FAudio_bModuleInstalled,                                  "[ FAUDIO ] Error: System not installed !!!" );
	FASSERT_MSG( ((_VirtualEmitter_t *)m_pUser)->uProperties & _EMITTER_PROPERTIES_3D, "[ FAUDIO ] Error: Illegal call !!!" );
	FASSERT_MSG( ( 0.0f <= fDopplerFactor ),                         "[ FAUDIO ] Error: Invalid fDopplerFactor !!!" );
	FASSERT_MSG( ( 10.0f >= fDopplerFactor ),                        "[ FAUDIO ] Error: Invalid fDopplerFactor !!!" );

	////
	//
	_VirtualEmitter_t *poVirtualEmitter = (_VirtualEmitter_t *)m_pUser;

	FASSERT_MSG( ( FWORLD_USERTYPE_AUDIO_EMITTER_INACTIVE != poVirtualEmitter->poAudioEmitter->m_nUser ), "[ FAUDIO ] Error: Invalid emitter !!!" );
	//
	////

	////
	//
	f32 fTemp = ( poVirtualEmitter->fDopplerFactor - fDopplerFactor );
	fTemp = ( fTemp * fTemp );

	if( _SIGNIFICANT_FACTOR_CHANGE_SQ < fTemp )
	{
		poVirtualEmitter->uStateChanges  |= _EMITTER_STATE_CHANGE_DOPPLER;
		poVirtualEmitter->fDopplerFactor  = fDopplerFactor;
	}
	//
	////

} // CFAudioEmitter::SetDopplerFactor

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void CFAudioEmitter::SetReverb( f32 fReverb )
{
	FASSERT_MSG( FAudio_bModuleInstalled,                                  "[ FAUDIO ] Error: System not installed !!!" );
	FASSERT_MSG( ((_VirtualEmitter_t *)m_pUser)->uProperties & _EMITTER_PROPERTIES_3D, "[ FAUDIO ] Error: Illegal call !!!" );
	FASSERT_MSG( FMATH_IS_UNIT_FLOAT( fReverb ),                     "[ FAUDIO ] Error: Invalid fReverb !!!" );

	////
	//
	_VirtualEmitter_t *poVirtualEmitter = (_VirtualEmitter_t *)m_pUser;

	FASSERT_MSG( ( FWORLD_USERTYPE_AUDIO_EMITTER_INACTIVE != poVirtualEmitter->poAudioEmitter->m_nUser ), "[ FAUDIO ] Error: Invalid emitter !!!" );
	//
	////

	////
	//
	f32 fTemp = ( poVirtualEmitter->fReverb - fReverb );
	fTemp = ( fTemp * fTemp );

	if( _SIGNIFICANT_FACTOR_CHANGE_SQ < fTemp )
	{
		poVirtualEmitter->uStateChanges |= _EMITTER_STATE_CHANGE_REVERB;
		poVirtualEmitter->fReverb        = fReverb;
	}
	//
	////

} // CFAudioEmitter::SetReverb

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void CFAudioEmitter::SetDuckable( BOOL bDuckable )
{
	FASSERT_MSG( FAudio_bModuleInstalled, "[ FAUDIO ] Error: System not installed !!!" );

	////
	//
	_VirtualEmitter_t *poVirtualEmitter = (_VirtualEmitter_t *)m_pUser;

	FASSERT_MSG( ( FWORLD_USERTYPE_AUDIO_EMITTER_INACTIVE != poVirtualEmitter->poAudioEmitter->m_nUser ), "[ FAUDIO ] Error: Invalid emitter !!!" );
	//
	////

	if( _EMITTER_PROPERTIES_DUCKABLE ^ ( poVirtualEmitter->uProperties & _EMITTER_PROPERTIES_DUCKABLE ) )
	{
		poVirtualEmitter->uStateChanges |= _EMITTER_STATE_CHANGE_DUCKABLE;
		if( bDuckable ) {
			poVirtualEmitter->uProperties |= _EMITTER_PROPERTIES_DUCKABLE;
		} else {
			poVirtualEmitter->uProperties &= ~_EMITTER_PROPERTIES_DUCKABLE;
		}
		SetVolume( poVirtualEmitter->fVolume );
	}

} // CFAudioEmitter::SetDuckable

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

BOOL CFAudioEmitter::GetDuckable( void )
{
	FASSERT_MSG( FAudio_bModuleInstalled, "[ FAUDIO ] Error: System not installed !!!" );

	////
	//
	_VirtualEmitter_t *poVirtualEmitter = (_VirtualEmitter_t *)m_pUser;

	FASSERT_MSG( ( FWORLD_USERTYPE_AUDIO_EMITTER_INACTIVE != poVirtualEmitter->poAudioEmitter->m_nUser ), "[ FAUDIO ] Error: Invalid emitter !!!" );
	//
	////

	return ( poVirtualEmitter->uProperties & _EMITTER_PROPERTIES_DUCKABLE );

} // CFAudioEmitter::GetDuckable

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void CFAudioEmitter::DuckAll( f32 fVolumeFactor )
{
	if( ! FAudio_bModuleInstalled )
	{
		return;
	}

	FASSERT_MSG( FMATH_IS_UNIT_FLOAT( fVolumeFactor ), "[ FAUDIO ] Error: Invalid fVolumeFactor !!!" );

	////
	//
	f32 fTemp = ( fVolumeFactor - _fDuckingFactor );
	fTemp = ( fTemp * fTemp );

	if( _SIGNIFICANT_VOLUME_CHANGE_SQ > fTemp )
	{
		// Indistinguishable difference.
		return;
	}
	//
	////

	_fDuckingFactor = fVolumeFactor;

	_VirtualEmitter_t *poVirtualEmitter, *poVirtualEmitterNext;
	FLinkRoot_t *poVirtualEmittersList;

	for( u32 uIndex = 0; uIndex < _uMaxPriorityLevels; ++uIndex )
	{
		// 2D.
		poVirtualEmittersList = &( _paoVirtualEmittersListActive2D[ uIndex ] );
		poVirtualEmitter      = (_VirtualEmitter_t *)flinklist_GetHead( poVirtualEmittersList );

		while( poVirtualEmitter )
		{
			poVirtualEmitterNext             = (_VirtualEmitter_t *)flinklist_GetNext( poVirtualEmittersList, poVirtualEmitter );
			poVirtualEmitter->uStateChanges |= _EMITTER_STATE_CHANGE_DUCKING;
			poVirtualEmitter->poAudioEmitter->SetVolume( poVirtualEmitter->fVolume );
			poVirtualEmitter                 = poVirtualEmitterNext;
		}

		// 3D.
		poVirtualEmittersList = &( _paoVirtualEmittersListActive3D[ uIndex ] );
		poVirtualEmitter      = (_VirtualEmitter_t *)flinklist_GetHead( poVirtualEmittersList );

		while( poVirtualEmitter )
		{
			poVirtualEmitterNext             = (_VirtualEmitter_t *)flinklist_GetNext( poVirtualEmittersList, poVirtualEmitter );
			poVirtualEmitter->uStateChanges |= _EMITTER_STATE_CHANGE_DUCKING;
			poVirtualEmitter->poAudioEmitter->SetVolume( poVirtualEmitter->fVolume );
			poVirtualEmitter                 = poVirtualEmitterNext;
		}
	}

} // CFAudioEmitter::DuckAll

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void CFAudioEmitter::SetPriority( u8 uPriority )
{
	FASSERT_MSG( FAudio_bModuleInstalled,                   "[ FAUDIO ] Error: System not installed !!!" );
	FASSERT_MSG( ( _uMaxPriorityLevels > uPriority ), "[ FAUDIO ] Error: Invalid uPriority !!!" );

	////
	//
	_VirtualEmitter_t *poVirtualEmitter = (_VirtualEmitter_t *)m_pUser;

	FASSERT_MSG( ( FWORLD_USERTYPE_AUDIO_EMITTER_INACTIVE != poVirtualEmitter->poAudioEmitter->m_nUser ), "[ FAUDIO ] Error: Invalid emitter !!!" );
	//
	////

	if( uPriority == poVirtualEmitter->uPriority )
	{
		return;
	}

	flinklist_Remove( poVirtualEmitter->paoVirtualEmittersListActive, poVirtualEmitter );
	--( _paoRealEmittersLimits[ poVirtualEmitter->uPriority ].uPlaying );
	poVirtualEmitter->uPriority = uPriority;
	++( _paoRealEmittersLimits[ uPriority ].uPlaying );

	if( poVirtualEmitter->uProperties & _EMITTER_PROPERTIES_3D )
	{
		poVirtualEmitter->paoVirtualEmittersListActive = &( _paoVirtualEmittersListActive3D[ uPriority ] );
	}
	else
	{
		poVirtualEmitter->paoVirtualEmittersListActive = &( _paoVirtualEmittersListActive2D[ uPriority ] );
	}

	//// Keep list in volume order.
	//
	if( poVirtualEmitter->paoVirtualEmittersListActive->nCount )
	{
		// Scan right.
		_VirtualEmitter_t *poVirtualEmitterIterator;
		f32 fVolume = poVirtualEmitter->fVolumeDucked;
		FLinkRoot_t *poList = poVirtualEmitter->paoVirtualEmittersListActive;

		poVirtualEmitterIterator = (_VirtualEmitter_t *)flinklist_GetHead( poList );

		do
		{
			if( poVirtualEmitterIterator->fVolumeDucked <= fVolume )
			{
				flinklist_AddBefore( poList, poVirtualEmitterIterator, poVirtualEmitter );
				return;
			}

			poVirtualEmitterIterator = (_VirtualEmitter_t *)flinklist_GetNext( poList, poVirtualEmitterIterator );

		} while( poVirtualEmitterIterator );

		flinklist_AddTail( poList, poVirtualEmitter );
	}
	else
	{
		flinklist_AddTail( poVirtualEmitter->paoVirtualEmittersListActive, poVirtualEmitter );
	}
	//
	////

} // CFAudioEmitter::SetPriority

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

u32 CFAudioEmitter::GetPriority( void )
{
	FASSERT_MSG( FAudio_bModuleInstalled, "[ FAUDIO ] Error: System not installed !!!" );

	////
	//
	_VirtualEmitter_t *poVirtualEmitter = (_VirtualEmitter_t *)m_pUser;

	FASSERT_MSG( ( FWORLD_USERTYPE_AUDIO_EMITTER_INACTIVE != poVirtualEmitter->poAudioEmitter->m_nUser ), "[ FAUDIO ] Error: Invalid emitter !!!" );
	//
	////

	return poVirtualEmitter->uPriority;

} // CFAudioEmitter::GetPriority

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

//void CFAudioEmitter::Play( u32 uLoops /* = 1 */, f32 fFirstLoopStartOffsetInSeconds /* = 0.0f */ )
void CFAudioEmitter::Play( u32 uLoops /* = 1 */ )
{
	FASSERT_MSG( FAudio_bModuleInstalled, "[ FAUDIO ] Error: System not installed !!!" );

	////
	//
	_VirtualEmitter_t *poVirtualEmitter = (_VirtualEmitter_t *)m_pUser;

	FASSERT_MSG( ( FWORLD_USERTYPE_AUDIO_EMITTER_INACTIVE != poVirtualEmitter->poAudioEmitter->m_nUser ), "[ FAUDIO ] Error: Invalid emitter !!!" );
	//
	////

	if( FAUDIO_EMITTER_STATE_PLAYING == poVirtualEmitter->oeState )
	{
		return;
	}

	////
	//
	FDataWvbFile_Wave_t *poWave = (FDataWvbFile_Wave_t *)poVirtualEmitter->oWaveHandle;

//	FASSERT_MSG( ( fFirstLoopStartOffsetInSeconds < poWave->fLengthInSeconds ), "[ FAUDIO ] Error: Invalid fFirstLoopStartOffsetInSeconds !!!" );
	//
	////

	poVirtualEmitter->oeState        = FAUDIO_EMITTER_STATE_PLAYING;
	poVirtualEmitter->uStateChanges &= ( ~ ( _EMITTER_STATE_CHANGE_STOP | _EMITTER_STATE_CHANGE_PAUSE | _EMITTER_STATE_CHANGE_UNPAUSE ) );
	poVirtualEmitter->uStateChanges |= _EMITTER_STATE_CHANGE_PLAY;
//	poVirtualEmitter->uStateChanges |= ( _EMITTER_STATE_CHANGE_VOLUME | _EMITTER_STATE_CHANGE_PLAY );

//	poVirtualEmitter->fSecondsPlayed = fFirstLoopStartOffsetInSeconds;
	poVirtualEmitter->fSecondsPlayed = 0.0f;

	if( uLoops )
	{
		if( 1 < uLoops ) {
			poVirtualEmitter->uProperties |= _EMITTER_PROPERTIES_LOOPING;
		} else {
			poVirtualEmitter->uProperties &= ~_EMITTER_PROPERTIES_LOOPING;
		}	
		poVirtualEmitter->fSecondsToPlay = ( poWave->fLengthInSeconds * (f32)uLoops );
	}
	else
	{
		poVirtualEmitter->uProperties 	|= _EMITTER_PROPERTIES_LOOPING;
		poVirtualEmitter->fSecondsToPlay = FAUDIO_UNLIMITED_SECONDS;
	}

	_bSkipEmittersListenersWorkDelay = FALSE;

} // CFAudioEmitter::Play

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void CFAudioEmitter::Pause( BOOL bEnabled )
{
	FASSERT_MSG( FAudio_bModuleInstalled, "[ FAUDIO ] Error: System not installed !!!" );

	////
	//
	_VirtualEmitter_t *poVirtualEmitter = (_VirtualEmitter_t *)m_pUser;

	FASSERT_MSG( ( FWORLD_USERTYPE_AUDIO_EMITTER_INACTIVE != poVirtualEmitter->poAudioEmitter->m_nUser ), "[ FAUDIO ] Error: Invalid emitter !!!" );
	//
	////

	if( FAUDIO_EMITTER_STATE_STOPPED == poVirtualEmitter->oeState )
	{
		return;
	}

	if( bEnabled )
	{
		if( FAUDIO_EMITTER_STATE_PLAYING == poVirtualEmitter->oeState )
		{
			poVirtualEmitter->oeState        = FAUDIO_EMITTER_STATE_PAUSED;
			poVirtualEmitter->uStateChanges &= ( ~ ( _EMITTER_STATE_CHANGE_PLAY | _EMITTER_STATE_CHANGE_STOP | _EMITTER_STATE_CHANGE_UNPAUSE ) );
			poVirtualEmitter->uStateChanges |= _EMITTER_STATE_CHANGE_PAUSE;
		}
	}
	else
	{
		if( FAUDIO_EMITTER_STATE_PAUSED == poVirtualEmitter->oeState )
		{
			poVirtualEmitter->oeState        = FAUDIO_EMITTER_STATE_PLAYING;
			poVirtualEmitter->uStateChanges &= ( ~ ( _EMITTER_STATE_CHANGE_PLAY | _EMITTER_STATE_CHANGE_STOP | _EMITTER_STATE_CHANGE_PAUSE ) );
			poVirtualEmitter->uStateChanges |= _EMITTER_STATE_CHANGE_UNPAUSE;
		}
	}

} // CFAudioEmitter::Pause

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void CFAudioEmitter::StopAll( void )
{
	if( ! FAudio_bModuleInstalled )
	{
		return;
	}

	_VirtualEmitter_t *poVirtualEmitter;
	FLinkRoot_t *poVirtualEmittersList;

	for( u32 uIndex = 0; uIndex < _uMaxPriorityLevels; ++uIndex )
	{
		// 2D.
		poVirtualEmittersList = &( _paoVirtualEmittersListActive2D[ uIndex ] );
		poVirtualEmitter      = (_VirtualEmitter_t *)flinklist_GetHead( poVirtualEmittersList );

		while( poVirtualEmitter )
		{
			poVirtualEmitter->poAudioEmitter->Stop( FALSE );
			poVirtualEmitter = (_VirtualEmitter_t *)flinklist_GetNext( poVirtualEmittersList, poVirtualEmitter );
		}

		// 3D.
		poVirtualEmittersList = &( _paoVirtualEmittersListActive3D[ uIndex ] );
		poVirtualEmitter      = (_VirtualEmitter_t *)flinklist_GetHead( poVirtualEmittersList );

		while( poVirtualEmitter )
		{
			poVirtualEmitter->poAudioEmitter->Stop( FALSE );
			poVirtualEmitter = (_VirtualEmitter_t *)flinklist_GetNext( poVirtualEmittersList, poVirtualEmitter );
		}
	}

} // CFAudioEmitter::StopAll

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void CFAudioEmitter::Stop( BOOL bAfterCurrentLoop )
{
	FASSERT_MSG( FAudio_bModuleInstalled, "[ FAUDIO ] Error: System not installed !!!" );

	////
	//
	_VirtualEmitter_t *poVirtualEmitter = (_VirtualEmitter_t *)m_pUser;

	FASSERT_MSG( ( FWORLD_USERTYPE_AUDIO_EMITTER_INACTIVE != poVirtualEmitter->poAudioEmitter->m_nUser ), "[ FAUDIO ] Error: Invalid emitter !!!" );
	//
	////

	if( FAUDIO_EMITTER_STATE_STOPPED == poVirtualEmitter->oeState )
	{
		return;
	}

	poVirtualEmitter->uProperties &= ~_EMITTER_PROPERTIES_LOOPING;

	if( ! bAfterCurrentLoop )
	{
		poVirtualEmitter->oeState        = FAUDIO_EMITTER_STATE_STOPPED;
		poVirtualEmitter->uStateChanges &= ( ~ ( _EMITTER_STATE_CHANGE_PLAY | _EMITTER_STATE_CHANGE_PAUSE | _EMITTER_STATE_CHANGE_UNPAUSE ) );
		poVirtualEmitter->uStateChanges |= _EMITTER_STATE_CHANGE_STOP;
	}
	else
	{
		FDataWvbFile_Wave_t *poWave      = (FDataWvbFile_Wave_t *)poVirtualEmitter->oWaveHandle;
		poVirtualEmitter->fSecondsToPlay = ( ( poWave->fLengthInSeconds * (f32)( (u32)( poVirtualEmitter->fSecondsPlayed / poWave->fLengthInSeconds ) ) ) + poWave->fLengthInSeconds );
	}

	_bSkipEmittersListenersWorkDelay = FALSE;

} // CFAudioEmitter::Stop

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

FAudio_EmitterState_e CFAudioEmitter::GetState( void )
{
	FASSERT_MSG( FAudio_bModuleInstalled, "[ FAUDIO ] Error: System not installed !!!" );

	////
	//
	_VirtualEmitter_t *poVirtualEmitter = (_VirtualEmitter_t *)m_pUser;

	FASSERT_MSG( ( FWORLD_USERTYPE_AUDIO_EMITTER_INACTIVE != poVirtualEmitter->poAudioEmitter->m_nUser ), "[ FAUDIO ] Error: Invalid emitter !!!" );
	//
	////

	return (FAudio_EmitterState_e) poVirtualEmitter->oeState;

} // CFAudioEmitter::GetState

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

f32 CFAudioEmitter::GetSecondsPlayed( void )
{
	FASSERT_MSG( FAudio_bModuleInstalled, "[ FAUDIO ] Error: System not installed !!!" );

	////
	//
	_VirtualEmitter_t *poVirtualEmitter = (_VirtualEmitter_t *)m_pUser;

	FASSERT_MSG( ( FWORLD_USERTYPE_AUDIO_EMITTER_INACTIVE != poVirtualEmitter->poAudioEmitter->m_nUser ), "[ FAUDIO ] Error: Invalid emitter !!!" );
	//
	////

	return poVirtualEmitter->fSecondsPlayed;

} // CFAudioEmitter::GetSecondsPlayed

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

f32 CFAudioEmitter::GetSecondsToPlay( void )
{
	FASSERT_MSG( FAudio_bModuleInstalled, "[ FAUDIO ] Error: System not installed !!!" );

	////
	//
	_VirtualEmitter_t *poVirtualEmitter = (_VirtualEmitter_t *)m_pUser;

	FASSERT_MSG( ( FWORLD_USERTYPE_AUDIO_EMITTER_INACTIVE != poVirtualEmitter->poAudioEmitter->m_nUser ), "[ FAUDIO ] Error: Invalid emitter !!!" );
	//
	////

	return poVirtualEmitter->fSecondsToPlay;

} // CFAudioEmitter::GetSecondsToPlay

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

FAudio_Error_e CFAudioEmitter::Play2D(	FAudio_WaveHandle_t oWaveHandle,
										f32 fVolume /* = 1.0f */,
										u8 uPriority /* = FAudio_EmitterDefaultPriorityLevel */,
										BOOL bDuckable /* = TRUE */,
										u32 uLoops /* = 1 */,
//										f32 fFirstLoopStartOffsetInSeconds /* = 0.0f */,
										f32 fPanLeftRight /* = 0.0f */,
										f32 fFrequencyFactor /* = 1.0f */,
										CFAudioEmitter **ppUserAudioEmitter )
{
	if( ! FAudio_bModuleInstalled )
	{
		return FAUDIO_ERROR;
	}

	CFAudioEmitter *poAudioEmitter = Create2D( oWaveHandle, uPriority, ppUserAudioEmitter );
	if( poAudioEmitter )
	{
		poAudioEmitter->SetVolume( fVolume );
		poAudioEmitter->SetDuckable( bDuckable );
		poAudioEmitter->SetPan( fPanLeftRight );
		poAudioEmitter->SetFrequencyFactor( fFrequencyFactor );
		poAudioEmitter->SetEndOfPlayCallback( _AutoDestroyEmitterEndOfPlayCallback );
#if !FANG_PRODUCTION_BUILD
		poAudioEmitter->m_pszModuleOwner = NULL;
		poAudioEmitter->m_nOwnerModuleLine = 0;
#endif
//		poAudioEmitter->Play( uLoops, fFirstLoopStartOffsetInSeconds );
		poAudioEmitter->Play( uLoops );
		_VirtualEmitter_t *poVirtualEmitter = (_VirtualEmitter_t *)poAudioEmitter->m_pUser;
		poVirtualEmitter->uProperties |= _EMITTER_PROPERTIES_FIRENFORGET;
		return FAUDIO_NO_ERROR;
	}
	else
	{
		return FAUDIO_ERROR;
	}

} // CFAudioEmitter::Play2D

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

FAudio_Error_e CFAudioEmitter::Play3D(	FAudio_WaveHandle_t oWaveHandle,
										const CFVec3A *poVecPosition_WS,
										f32 fRadiusOuter,
//										f32 fRadiusInner = DS3D_DEFAULTMINDISTANCE,
										f32 fVolume /* = 1.0f */,
										u8 uPriority /* = FAudio_EmitterDefaultPriorityLevel */,
										BOOL bDuckable /* = TRUE */,
										u32 uLoops /* = 1 */,
										f32 fFrequencyFactor /* = 1.0f */,
//										f32 fFirstLoopStartOffsetInSeconds /* = 0.0f */ )
										f32 fSpawnRadiusFactor /* = -1.0f */,
										BOOL bAutoUpdatePosition /* = FALSE */,
										CFAudioEmitter **ppUserAudioEmitter )
{
	if( ! FAudio_bModuleInstalled )
	{
		return FAUDIO_ERROR;
	}

	if( ! FWorld_pWorld )
	{
		return FAUDIO_ERROR;
	}

	_VirtualListener_t *poVirtualListener;
	u32 uIndex;

	////
	//
	/*if( 0.0f < fSpawnRadiusFactor )
	{
		for( uIndex = 0; uIndex < _uActiveVirtualListeners; ++uIndex )
		{
			poVirtualListener = &( _aoVirtualListeners[ uIndex ] );

			_oTempVec3A = poVirtualListener->poXfmCurrentOrientation_WS->m_MtxF.m_vPos;
			_oTempVec3A.Sub( *( poVecPosition_WS ) );

			if( _oTempVec3A.MagSq() <= ( fSpawnRadiusFactor * fSpawnRadiusFactor * fRadiusOuter * fRadiusOuter ) )
			{
				break;
			}
		}
		//
		////

		if( ! ( uIndex < _uActiveVirtualListeners ) )
		{
			return FAUDIO_ERROR;
		}
	}*/
	// !!Nate
	if( fSpawnRadiusFactor > 0.0f && uLoops == 1)
	{
		for( uIndex = 0; uIndex < _uActiveVirtualListeners; ++uIndex )
		{
			poVirtualListener = &( _aoVirtualListeners[ uIndex ] );

			_oTempVec3A = poVirtualListener->poXfmCurrentOrientation_WS->m_MtxF.m_vPos;
			_oTempVec3A.Sub( *( poVecPosition_WS ) );

			if( _oTempVec3A.MagSq() < ( fSpawnRadiusFactor * fSpawnRadiusFactor * fRadiusOuter * fRadiusOuter ) )
			{
				break;
			}
		}
		//
		////

		if( uIndex == _uActiveVirtualListeners ) 
		{
			return FAUDIO_ERROR;
		}
	}

	CFAudioEmitter *poAudioEmitter = Create3D( oWaveHandle, uPriority, poVecPosition_WS, fRadiusOuter, bAutoUpdatePosition, ppUserAudioEmitter );
	if( poAudioEmitter )
	{
//		poAudioEmitter->SetRadius( fRadiusOuter, fRadiusInner );
//		poAudioEmitter->SetRadius( fRadiusOuter, DS3D_DEFAULTMINDISTANCE );
		poAudioEmitter->SetVolume( fVolume );
		poAudioEmitter->SetDuckable( bDuckable );
		poAudioEmitter->SetFrequencyFactor( fFrequencyFactor );
		poAudioEmitter->SetEndOfPlayCallback( _AutoDestroyEmitterEndOfPlayCallback );
#if !FANG_PRODUCTION_BUILD
		poAudioEmitter->m_pszModuleOwner = NULL;
		poAudioEmitter->m_nOwnerModuleLine = 0;
#endif
//		poAudioEmitter->Play( uLoops, fFirstLoopStartOffsetInSeconds );
		poAudioEmitter->Play( uLoops );
		_VirtualEmitter_t *poVirtualEmitter = (_VirtualEmitter_t *)poAudioEmitter->m_pUser;
		poVirtualEmitter->uProperties |= _EMITTER_PROPERTIES_FIRENFORGET;
		return FAUDIO_NO_ERROR;
	}
	else
	{
		return FAUDIO_ERROR;
	}

} // CFAudioEmitter::Play3D

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

BOOL CFAudioEmitter::Is3D( void ) {
	FASSERT_MSG( FAudio_bModuleInstalled, "[ FAUDIO ] Error: System not installed !!!" );

	_VirtualEmitter_t *poVirtualEmitter = (_VirtualEmitter_t *)m_pUser;

	return !!( poVirtualEmitter->uProperties & _EMITTER_PROPERTIES_3D );
} // Is3D;


BOOL CFAudioEmitter::IsFireNForget( void ) {	 
	FASSERT_MSG( FAudio_bModuleInstalled, "[ FAUDIO ] Error: System not installed !!!" );

	_VirtualEmitter_t *poVirtualEmitter = (_VirtualEmitter_t *)m_pUser;

	return !!( poVirtualEmitter->uProperties & _EMITTER_PROPERTIES_FIRENFORGET );
}

void CFAudioEmitter::SetPauseLevel( FAudio_PauseLevel_e eNewPauseLevel ) {
	FASSERT_MSG( FAudio_bModuleInstalled, "[ FAUDIO ] Error: System not installed !!!" );

	_VirtualEmitter_t *poVirtualEmitter = (_VirtualEmitter_t *)m_pUser;

	poVirtualEmitter->uPauseLevel = eNewPauseLevel;
}

FAudio_PauseLevel_e CFAudioEmitter::GetPauseLevel( void ) {
	FASSERT_MSG( FAudio_bModuleInstalled, "[ FAUDIO ] Error: System not installed !!!" );

	_VirtualEmitter_t *poVirtualEmitter = (_VirtualEmitter_t *)m_pUser;

	return (FAudio_PauseLevel_e) poVirtualEmitter->uPauseLevel;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void _AutoDestroyEmitterEndOfPlayCallback( CFAudioEmitter *poAudioEmitter )
{
	poAudioEmitter->Destroy();

} // _AutoDestroyEmitterEndOfPlayCallback

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void _TrackEmittersProgress( FLinkRoot_t *poVirtualEmittersListActive )
{
	FLinkRoot_t *poVirtualEmittersList;
	_VirtualEmitter_t *poVirtualEmitter;
	u32 uList;

	for( uList = 0; uList < _uMaxPriorityLevels; ++uList )
	{
		poVirtualEmittersList = &( poVirtualEmittersListActive[ uList ] );
		poVirtualEmitter      = (_VirtualEmitter_t *)flinklist_GetHead( poVirtualEmittersList );

		while( poVirtualEmitter )
		{
			if( ( FAUDIO_EMITTER_STATE_PLAYING == poVirtualEmitter->oeState ) &&
				!( poVirtualEmitter->uStateChanges & _EMITTER_STATE_CHANGE_PLAY ) )
			{
				poVirtualEmitter->fSecondsPlayed += ( FLoop_fRealPreviousLoopSecs * poVirtualEmitter->fFrequencyFactor );

				if( ( 0.0f <= poVirtualEmitter->fSecondsToPlay ) &&		// Infinitely looping emitters don't stop by themselves.
					( poVirtualEmitter->fSecondsToPlay <= poVirtualEmitter->fSecondsPlayed ) )
				{
					poVirtualEmitter->oeState        = FAUDIO_EMITTER_STATE_STOPPED;
					poVirtualEmitter->uStateChanges  = _EMITTER_STATE_CHANGE_STOP;
					_bSkipEmittersListenersWorkDelay = FALSE;
				}
			}

			poVirtualEmitter = (_VirtualEmitter_t *)flinklist_GetNext( poVirtualEmittersList, poVirtualEmitter );
		}
	}

} // _TrackEmittersProgress

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void _ApplyRealEmittersChanges( FLinkRoot_t *poVirtualEmittersListActive ) {
	FLinkRoot_t *poVirtualEmittersList;
	_VirtualEmitter_t *poVirtualEmitter;
	FDataWvbFile_Wave_t *poWave;
	u32 uList, uNewFreq;
	LPDIRECTSOUNDBUFFER8 pDSBuffer;
	f32 fVolume;

	for( uList = 0; uList < _uMaxPriorityLevels; ++uList ) {
		poVirtualEmittersList = &poVirtualEmittersListActive[ uList ];

		poVirtualEmitter = (_VirtualEmitter_t *)flinklist_GetHead( poVirtualEmittersList );
		while( poVirtualEmitter ) {

			if( poVirtualEmitter->poRealEmitter ) {
				pDSBuffer = poVirtualEmitter->poRealEmitter->poDSBuffer;

				// Position, Velocity, Radius, Doppler.
				if( poVirtualEmitter->poVirtualListenerCurrent ) {

					if( ( ( _EMITTER_STATE_CHANGE_POSITION | _EMITTER_STATE_CHANGE_DOPPLER ) & poVirtualEmitter->uStateChanges ) ||
						( _LISTENER_STATE_CHANGE_ORIENTATION & poVirtualEmitter->poVirtualListenerCurrent->uStateChange ) ) {
						
						// Position.
						poVirtualEmitter->poVirtualListenerCurrent->poXfmCurrentOrientation_WS->TransformPointR( _oTempVec3A.v3, poVirtualEmitter->poVecCurrentPosition_WS->v3 );

						_oDSEmitterAttributes.vPosition.x = _oTempVec3A.x;
						_oDSEmitterAttributes.vPosition.y = _oTempVec3A.y;
						_oDSEmitterAttributes.vPosition.z = _oTempVec3A.z;
						
						// Velocity.
						// Emitter velocity.
						_oTempVec3A = *( poVirtualEmitter->poVecCurrentPosition_WS );
						_oTempVec3A.Sub( *( poVirtualEmitter->poVecPreviousPosition_WS ) );

						// Emitter and listener combined velocity.
						_oTempVec3A.Sub( *( poVirtualEmitter->poVirtualListenerCurrent->poVecVelocity_WS ) );
						_oTempVec3A.Mul( _fOOEmittersListenersWorkDelay );

						poVirtualEmitter->poVirtualListenerCurrent->poXfmCurrentOrientation_WS->TransformDirR( _oTempVec3A_Velocity.v3, _oTempVec3A.v3 );

						_oDSEmitterAttributes.vVelocity.x = _oTempVec3A_Velocity.x;
						_oDSEmitterAttributes.vVelocity.y = _oTempVec3A_Velocity.y;
						_oDSEmitterAttributes.vVelocity.z = _oTempVec3A_Velocity.z;

						// Radius, Doppler.						
//						_oDSEmitterAttributes.flMinDistance = poVirtualEmitter->fRadiusInner;
						_oDSEmitterAttributes.flMaxDistance = poVirtualEmitter->fRadiusOuter;

						if( poVirtualEmitter->poRealEmitter->poDS3DBuffer ) {
							poVirtualEmitter->poRealEmitter->poDS3DBuffer->SetAllParameters( &_oDSEmitterAttributes, DS3D_DEFERRED );
						}
					} else {
						if( poVirtualEmitter->poRealEmitter->poDS3DBuffer ) {
							poVirtualEmitter->poRealEmitter->poDS3DBuffer->SetVelocity( 0.0f, 0.0f, 0.0f, DS3D_DEFERRED );
						}
					}
				}

				// Volume.
				if( FAudio_bMasterSfxVolChanged ||
					poVirtualEmitter->uStateChanges & _EMITTER_STATE_CHANGE_VOLUME ) {
					fVolume = FAudio_fMasterSfxUnitVol * poVirtualEmitter->fVolumeDucked;
					pDSBuffer->SetVolume( _anVolumes[ fmath_FloatToU32( _UNIQUE_FLOAT_VOL_LEVEL_INDICES * fVolume ) ] );					
				}
				
				// Frequency.
				if( _EMITTER_STATE_CHANGE_FREQUENCY & poVirtualEmitter->uStateChanges ) {
					poWave = (FDataWvbFile_Wave_t *)poVirtualEmitter->oWaveHandle;
					uNewFreq = fmath_FloatToU32( poVirtualEmitter->fFrequencyFactor * poWave->fFreqHz );
					FMATH_CLAMP( uNewFreq, DSBFREQUENCY_MIN, DSBFREQUENCY_MAX );
					pDSBuffer->SetFrequency( uNewFreq );
				}

				// Pan.
				if( _EMITTER_STATE_CHANGE_PAN & poVirtualEmitter->uStateChanges ) {
					
					if( 0.0f <= poVirtualEmitter->fPanLeftRight ) {
						pDSBuffer->SetPan( - _anVolumes[ fmath_FloatToU32( _UNIQUE_FLOAT_VOL_LEVEL_INDICES * ( 1.0f - poVirtualEmitter->fPanLeftRight ) ) ] );
					} else {
						pDSBuffer->SetPan( + _anVolumes[ fmath_FloatToU32( _UNIQUE_FLOAT_VOL_LEVEL_INDICES * ( 1.0f + poVirtualEmitter->fPanLeftRight ) ) ] );
					}
				}

				// Play / unpause.
				if( ( _EMITTER_STATE_CHANGE_PLAY | _EMITTER_STATE_CHANGE_UNPAUSE ) & poVirtualEmitter->uStateChanges ) {
					poWave = (FDataWvbFile_Wave_t *)poVirtualEmitter->oWaveHandle;

					if( poVirtualEmitter->uProperties & _EMITTER_PROPERTIES_LOOPING ) {
						pDSBuffer->Play( 0, 0, DSBPLAY_LOOPING );
					} else {
						pDSBuffer->Play( 0, 0, 0 );
					}
				}
			}

			poVirtualEmitter = (_VirtualEmitter_t *)flinklist_GetNext( poVirtualEmittersList, poVirtualEmitter );
		}
	}
} // _ApplyRealEmittersChanges

void _InvokeEmittersEndofplayCallbacks( FLinkRoot_t *poVirtualEmittersListActive )
{
	FLinkRoot_t *poVirtualEmittersList;
	_VirtualEmitter_t *poVirtualEmitter, *poVirtualEmitterNext;
	u32 uList;

	for( uList = 0; uList < _uMaxPriorityLevels; ++uList )
	{
		poVirtualEmittersList = &( poVirtualEmittersListActive[ uList ] );

		poVirtualEmitter = (_VirtualEmitter_t *)flinklist_GetHead( poVirtualEmittersList );
		while( poVirtualEmitter )
		{
			if( ( _EMITTER_STATE_CHANGE_STOP & poVirtualEmitter->uStateChanges ) &&
				( !( poVirtualEmitter->uProperties & _EMITTER_PROPERTIES_LOOPING ) ) )
			{
				poVirtualEmitterNext = (_VirtualEmitter_t *)flinklist_GetNext( poVirtualEmittersList, poVirtualEmitter );

				poVirtualEmitter->uStateChanges = _EMITTER_STATE_CHANGE_NONE;

				if( poVirtualEmitter->pEndOfPlayCallback )
				{
					poVirtualEmitter->pEndOfPlayCallback( poVirtualEmitter->poAudioEmitter );
				}

				poVirtualEmitter = poVirtualEmitterNext;
				continue;
			}

			poVirtualEmitter = (_VirtualEmitter_t *)flinklist_GetNext( poVirtualEmittersList, poVirtualEmitter );
		}
	}

} // _InvokeEmittersEndofplayCallbacks

static void _DestroyAllEmittersFromABank( FAudio_BankHandle_t hBank ) {
	_VirtualEmitter_t *poVirtualEmitter, *pNextVEmitter;
	FLinkRoot_t *poVirtualEmittersList;
	FDataWvbFile_Wave_t *pWave;

	for( u32 uIndex = 0; uIndex < _uMaxPriorityLevels; ++uIndex ) {
		// 2D.
		poVirtualEmittersList = &_paoVirtualEmittersListActive2D[ uIndex ];

		poVirtualEmitter = (_VirtualEmitter_t *)flinklist_GetHead( poVirtualEmittersList );
		while( poVirtualEmitter ) {
			// grab the next virtual emitter
			pNextVEmitter = (_VirtualEmitter_t *)flinklist_GetNext( poVirtualEmittersList, poVirtualEmitter );

			pWave = (FDataWvbFile_Wave_t *)poVirtualEmitter->oWaveHandle;
			if( pWave->oBankHandle == hBank ) {
				poVirtualEmitter->poAudioEmitter->Destroy();
			}
			// move to the next emitter
			poVirtualEmitter = pNextVEmitter;
		}

		// 3D.
		poVirtualEmittersList = &_paoVirtualEmittersListActive3D[ uIndex ];

		poVirtualEmitter = (_VirtualEmitter_t *)flinklist_GetHead( poVirtualEmittersList );
		while( poVirtualEmitter ) {
			// grab the next virtual emitter
			pNextVEmitter = (_VirtualEmitter_t *)flinklist_GetNext( poVirtualEmittersList, poVirtualEmitter );

			pWave = (FDataWvbFile_Wave_t *)poVirtualEmitter->oWaveHandle;
			if( pWave->oBankHandle == hBank ) {
				poVirtualEmitter->poAudioEmitter->Destroy();
			}
			// move to the next emitter
			poVirtualEmitter = pNextVEmitter;
		}
	}
}


//////////////////////////////////////////////////////////////////////////////////
// CFAudioStream - Windows DirectSound PCM ring-buffer streaming of .wvs files.
// .wvs is an FDX8Data_WvsFile_Header_t followed by Xbox XADPCM (or PCM) blocks.
// DirectSound on Windows has no IDirectSoundStream; we decode to PCM on the
// fly and refill a looping CreateSoundBuffer from the play cursor.
////////////////////////////////////////////////////////////////////////////////

static void _StreamFinish( _Stream_t *poStream )
{
	if( poStream->poDSBuffer )
	{
		poStream->poDSBuffer->Stop();
	}

	poStream->oeState        = FAUDIO_STREAM_STATE_STOPPED;
	poStream->uStateChanges |= _STREAM_STATE_CHANGE_STOP;
	poStream->bIsLooping     = FALSE;
	poStream->bEOF           = TRUE;
}

static void _StreamSeekToStart( _Stream_t *poStream )
{
	if( FFILE_INVALID_HANDLE == poStream->ohFile )
	{
		return;
	}

	if( FFILE_ERROR_RET == ffile_Seek( poStream->ohFile, (s32)poStream->uDataStart, FFILE_SEEK_SET ) )
	{
		poStream->oeState = FAUDIO_STREAM_STATE_ERROR;
		return;
	}

	poStream->uBytesReadCompressed = 0;
	poStream->bEOF                 = FALSE;
}

static void _StreamWritePCM( _Stream_t *poStream, const u8 *pbyPCM, u32 uBytes )
{
	if( ( ! poStream->poDSBuffer ) || ( 0 == uBytes ) )
	{
		return;
	}

	LPVOID pData1 = NULL, pData2 = NULL;
	DWORD  dwBytes1 = 0, dwBytes2 = 0;

	u32 uFirst = poStream->uBufferSize - poStream->uWritePos;
	if( uFirst > uBytes )
	{
		uFirst = uBytes;
	}

	if( FAILED( poStream->poDSBuffer->Lock( poStream->uWritePos, uFirst, &pData1, &dwBytes1, &pData2, &dwBytes2, 0 ) ) )
	{
		poStream->oeState = FAUDIO_STREAM_STATE_ERROR;
		return;
	}

	fang_MemCopy( pData1, pbyPCM, dwBytes1 );

	if( pData2 && dwBytes2 )
	{
		fang_MemCopy( pData2, pbyPCM + dwBytes1, dwBytes2 );
	}

	poStream->poDSBuffer->Unlock( pData1, dwBytes1, pData2, dwBytes2 );

	poStream->uWritePos = ( poStream->uWritePos + uBytes ) % poStream->uBufferSize;
}

static void _StreamApplyChanges( _Stream_t *poStream )
{
	if( ( ! poStream->poDSBuffer ) || ( 0 == poStream->uStateChanges ) )
	{
		return;
	}

	if( poStream->uStateChanges & _STREAM_STATE_CHANGE_VOLUME )
	{
		f32 fVolume = poStream->fVolume;
		if( poStream->bTreatAsSfx )
		{
			fVolume *= FAudio_fMasterSfxUnitVol;
		}
		else
		{
			fVolume *= FAudio_fMasterMusicUnitVol;
		}

		FMATH_CLAMP_UNIT_FLOAT( fVolume );

		poStream->poDSBuffer->SetVolume( _anVolumes[ fmath_FloatToU32( _UNIQUE_FLOAT_VOL_LEVEL_INDICES * fVolume ) ] );
	}

	if( poStream->uStateChanges & _STREAM_STATE_CHANGE_PAN )
	{
		// Bi-polar unitfloat -> DirectSound pan (-10000 .. 10000).
		f32 fPan = poStream->fPanLeftRight;
		FMATH_BIPOLAR_CLAMP( fPan, -1.0f, 1.0f );
		poStream->poDSBuffer->SetPan( (LONG)( fPan * (f32)DSBPAN_RIGHT ) );
	}

	if( poStream->uStateChanges & _STREAM_STATE_CHANGE_FREQUENCY )
	{
		u32 uNewFreq = fmath_FloatToU32( poStream->fFrequencyFactor * poStream->fFrequencyInHz );
		FMATH_CLAMP( uNewFreq, DSBFREQUENCY_MIN, DSBFREQUENCY_MAX );
		poStream->poDSBuffer->SetFrequency( uNewFreq );
	}

	if( poStream->uStateChanges & _STREAM_STATE_CHANGE_PAUSE )
	{
		poStream->poDSBuffer->Stop();
	}

	if( poStream->uStateChanges & _STREAM_STATE_CHANGE_UNPAUSE )
	{
		poStream->poDSBuffer->Play( 0, 0, DSBPLAY_LOOPING );
	}

	if( poStream->uStateChanges & _STREAM_STATE_CHANGE_STOP )
	{
		poStream->poDSBuffer->Stop();
	}
}

static void _StreamRefill( _Stream_t *poStream )
{
	if( ( ! poStream->bIsActive ) ||
		( ! poStream->poDSBuffer ) ||
		( FAUDIO_STREAM_STATE_PLAYING != poStream->oeState ) )
	{
		return;
	}

	// Track seconds played from play-cursor advance.
	DWORD dwPlay = 0, dwWrite = 0;
	if( SUCCEEDED( poStream->poDSBuffer->GetCurrentPosition( &dwPlay, &dwWrite ) ) )
	{
		u32 uDelta;
		if( dwPlay >= poStream->uPlayPosLast )
		{
			uDelta = dwPlay - poStream->uPlayPosLast;
		}
		else
		{
			uDelta = ( poStream->uBufferSize - poStream->uPlayPosLast ) + dwPlay;
		}

		if( poStream->uBytesPerSecPCM )
		{
			poStream->fSecondsPlayed += ( (f32)uDelta / (f32)poStream->uBytesPerSecPCM );
		}

		poStream->uPlayPosLast = dwPlay;
	}

	// End of finite play (multi-loop count exhausted).
	if( ( FAUDIO_UNLIMITED_SECONDS != poStream->fSecondsToPlay ) &&
		( poStream->fSecondsPlayed >= poStream->fSecondsToPlay ) )
	{
		poStream->bIsLooping = FALSE;
		poStream->bEOF       = TRUE;
	}

	// Free space between write cursor and play cursor (leave a small margin).
	u32 uFree;
	if( dwPlay >= poStream->uWritePos )
	{
		uFree = dwPlay - poStream->uWritePos;
	}
	else
	{
		uFree = ( poStream->uBufferSize - poStream->uWritePos ) + dwPlay;
	}

	if( uFree > _STREAM_WRITE_MARGIN_BYTES )
	{
		uFree -= _STREAM_WRITE_MARGIN_BYTES;
	}
	else
	{
		uFree = 0;
	}

	// Fill free space with decoded PCM.
	while( ( uFree >= poStream->uPcmChunkBytes ) && ( ! poStream->bEOF ) && ( FAUDIO_STREAM_STATE_PLAYING == poStream->oeState ) )
	{
		u32 uToRead = poStream->uCompressChunkBytes;
		u32 uRemaining = ( poStream->uDataBytes > poStream->uBytesReadCompressed ) ?
			( poStream->uDataBytes - poStream->uBytesReadCompressed ) : 0;

		if( 0 == uRemaining )
		{
			if( poStream->bIsLooping )
			{
				_StreamSeekToStart( poStream );
				if( FAUDIO_STREAM_STATE_ERROR == poStream->oeState )
				{
					return;
				}
				uRemaining = poStream->uDataBytes;
			}
			else
			{
				poStream->bEOF = TRUE;
				break;
			}
		}

		if( uToRead > uRemaining )
		{
			uToRead = uRemaining;
		}

		// Keep ADPCM reads block-aligned.
		if( FDX8DATA_WAVEFORMATEX_ID_XBOX_ADPCM == poStream->oWavFormat.WaveFormatEx.wFormatTag )
		{
			u32 uBlockAlign = poStream->oWavFormat.WaveFormatEx.nBlockAlign;
			if( uBlockAlign )
			{
				uToRead = ( uToRead / uBlockAlign ) * uBlockAlign;
				if( 0 == uToRead )
				{
					uToRead = uBlockAlign;
				}
			}
		}

		s32 nRead = ffile_Read( poStream->ohFile, uToRead, poStream->pbyCompressScratch );
		if( nRead <= 0 )
		{
			if( poStream->bIsLooping )
			{
				_StreamSeekToStart( poStream );
				if( FAUDIO_STREAM_STATE_ERROR == poStream->oeState )
				{
					return;
				}
				continue;
			}
			poStream->bEOF = TRUE;
			break;
		}

		u32 uDecoded = _xadpcm_DecodedSize( &poStream->oWavFormat, (u32)nRead );
		if( ( 0 == uDecoded ) || ( uDecoded > poStream->uPcmChunkBytes ) || ( uDecoded > uFree ) )
		{
			// Leave the short read for the next tick (or treat as EOF below).
			if( (u32)nRead < uToRead )
			{
				if( poStream->bIsLooping )
				{
					_StreamSeekToStart( poStream );
					continue;
				}
				poStream->bEOF = TRUE;
			}
			break;
		}

		if( ! _xadpcm_DecodeWave( &poStream->oWavFormat, poStream->pbyCompressScratch, (u32)nRead, poStream->pbyPcmScratch ) )
		{
			poStream->oeState = FAUDIO_STREAM_STATE_ERROR;
			return;
		}

		_StreamWritePCM( poStream, poStream->pbyPcmScratch, uDecoded );
		if( FAUDIO_STREAM_STATE_ERROR == poStream->oeState )
		{
			return;
		}

		poStream->uBytesReadCompressed += (u32)nRead;
		uFree -= uDecoded;

		if( (u32)nRead < poStream->uCompressChunkBytes )
		{
			// Short read: either EOF or a partial tail.
			if( poStream->uBytesReadCompressed >= poStream->uDataBytes )
			{
				if( poStream->bIsLooping )
				{
					_StreamSeekToStart( poStream );
					if( FAUDIO_STREAM_STATE_ERROR == poStream->oeState )
					{
						return;
					}
				}
				else
				{
					poStream->bEOF = TRUE;
				}
			}
		}
	}

	// Natural end: play cursor has drained the buffer past EOF.
	if( poStream->bEOF && ( ! poStream->bIsLooping ) && ( FAUDIO_STREAM_STATE_PLAYING == poStream->oeState ) )
	{
		u32 uBuffered;
		if( poStream->uWritePos >= dwPlay )
		{
			uBuffered = poStream->uWritePos - dwPlay;
		}
		else
		{
			uBuffered = ( poStream->uBufferSize - dwPlay ) + poStream->uWritePos;
		}

		if( uBuffered <= _STREAM_WRITE_MARGIN_BYTES )
		{
			_StreamFinish( poStream );
		}
	}
}

////////////////////////////////////////////////////////////////////////////////
// CFAudioStream Methods
////////////////////////////////////////////////////////////////////////////////
CFAudioStream *CFAudioStream::Create( cchar *pszName, BOOL bWillBeUsedForMusic/*=TRUE*/ )
{
	if( ! FAudio_bModuleInstalled )
	{
		return NULL;
	}

	if( ! pszName )
	{
		DEVPRINTF( "[ FAUDIO ] Error %u: CFAudioStream::Create() failed !!!\n", __LINE__ );
		return NULL;
	}

	if( fclib_strlen( pszName ) > FAUDIO_MAX_ASSET_NAME_LENGTH )
	{
		DEVPRINTF( "[ FAUDIO ] Error %u: CFAudioStream::Create() failed. Stream name '%s' is longer than %u characters.\n", __LINE__, pszName, FAUDIO_MAX_ASSET_NAME_LENGTH );
		return NULL;
	}

	_Stream_t *poStream;

	u32 uIndex;

	for( uIndex = 0; uIndex < _uMaxStreams; ++uIndex )
	{
		poStream = &_paoStreams[ uIndex ];

		if( ! poStream->bIsActive )
		{
			break;
		}
	}

	if( uIndex >= _uMaxStreams )
	{
		DEVPRINTF( "[ FAUDIO ] Error %u: CFAudioStream::Create() failed !!!\n", __LINE__ );
		return NULL;
	}

	char szTemp[ FAUDIO_MAX_ASSET_NAME_LENGTH + 128 ];

	// Game root may be unset (Windows port historically never assigned it).
	// Fall back to a bare name so ffile_Open still finds a loose .wvs
	// relative to the process CWD rather than strcpy'ing a NULL source.
	szTemp[0] = '\0';
	if( Fang_ConfigDefs.pszFile_GameRootPathName )
	{
		fclib_strcpy( szTemp, Fang_ConfigDefs.pszFile_GameRootPathName );
	}
	fclib_strcat( szTemp, pszName );
	fclib_strcat( szTemp, ".wvs" );

	poStream->ohFile = ffile_Open( szTemp, FFILE_OPEN_RONLY, TRUE );
	if( FFILE_INVALID_HANDLE == poStream->ohFile )
	{
		DEVPRINTF( "[ FAUDIO ] Error %u: Could not open \"%s\" file !!!\n", __LINE__, szTemp );
		return NULL;
	}

	// Read the .wvs header synchronously (32 bytes).
	FDX8Data_WvsFile_Header_t oHeader;
	fang_MemZero( &oHeader, sizeof( oHeader ) );

	if( (s32)sizeof( oHeader ) != ffile_Read( poStream->ohFile, sizeof( oHeader ), &oHeader ) )
	{
		DEVPRINTF( "[ FAUDIO ] Error %u: Could not read header from \"%s\" !!!\n", __LINE__, szTemp );
		ffile_Close( poStream->ohFile );
		poStream->ohFile = FFILE_INVALID_HANDLE;
		return NULL;
	}

	if( ( FDX8DATA_WAVEFORMATEX_ID_XBOX_ADPCM != oHeader.WavFormat.WaveFormatEx.wFormatTag ) &&
		( FDX8DATA_WAVEFORMATEX_ID_PCM != oHeader.WavFormat.WaveFormatEx.wFormatTag ) )
	{
		DEVPRINTF( "[ FAUDIO ] Error %u: Unsupported wvs format tag %u in \"%s\" !!!\n", __LINE__, oHeader.WavFormat.WaveFormatEx.wFormatTag, szTemp );
		ffile_Close( poStream->ohFile );
		poStream->ohFile = FFILE_INVALID_HANDLE;
		return NULL;
	}

	if( ( 0 == oHeader.nNumBytes ) || ( 0.0f >= oHeader.fSecs ) || ( 0.0f >= oHeader.fFreqHz ) ||
		( 0 == oHeader.WavFormat.WaveFormatEx.nChannels ) || ( 2 < oHeader.WavFormat.WaveFormatEx.nChannels ) )
	{
		DEVPRINTF( "[ FAUDIO ] Error %u: Invalid wvs header in \"%s\" !!!\n", __LINE__, szTemp );
		ffile_Close( poStream->ohFile );
		poStream->ohFile = FFILE_INVALID_HANDLE;
		return NULL;
	}

	// Build a PCM DirectSound buffer matching the decoded stream format.
	WAVEFORMATEX oPCMFormat;
	fang_MemZero( &oPCMFormat, sizeof( oPCMFormat ) );
	oPCMFormat.wFormatTag      = WAVE_FORMAT_PCM;
	oPCMFormat.nChannels       = oHeader.WavFormat.WaveFormatEx.nChannels;
	oPCMFormat.nSamplesPerSec  = (DWORD)oHeader.fFreqHz;
	oPCMFormat.wBitsPerSample  = 16;
	oPCMFormat.nBlockAlign     = (WORD)( oPCMFormat.nChannels * sizeof( s16 ) );
	oPCMFormat.nAvgBytesPerSec = oPCMFormat.nSamplesPerSec * oPCMFormat.nBlockAlign;
	oPCMFormat.cbSize          = 0;

	u32 uCompressChunk = _STREAM_COMPRESS_CHUNK_BYTES;
	if( oHeader.WavFormat.WaveFormatEx.nBlockAlign )
	{
		uCompressChunk = ( uCompressChunk / oHeader.WavFormat.WaveFormatEx.nBlockAlign ) * oHeader.WavFormat.WaveFormatEx.nBlockAlign;
		if( 0 == uCompressChunk )
		{
			uCompressChunk = oHeader.WavFormat.WaveFormatEx.nBlockAlign;
		}
	}

	u32 uPcmChunk = _xadpcm_DecodedSize( &oHeader.WavFormat, uCompressChunk );
	if( 0 == uPcmChunk )
	{
		DEVPRINTF( "[ FAUDIO ] Error %u: Could not size PCM chunk for \"%s\" !!!\n", __LINE__, szTemp );
		ffile_Close( poStream->ohFile );
		poStream->ohFile = FFILE_INVALID_HANDLE;
		return NULL;
	}

	// Ring buffer: a couple of seconds of PCM, rounded up to whole chunks.
	u32 uBytesPerSec = (u32)( oPCMFormat.nAvgBytesPerSec );
	u32 uBufferSize  = (u32)( _STREAM_PCM_BUFFER_SECS * (f32)uBytesPerSec );
	uBufferSize      = ( ( uBufferSize + uPcmChunk - 1 ) / uPcmChunk ) * uPcmChunk;
	if( uBufferSize < ( uPcmChunk * 2 ) )
	{
		uBufferSize = uPcmChunk * 2;
	}

	DSBUFFERDESC oBufferDesc;
	fang_MemZero( &oBufferDesc, sizeof( oBufferDesc ) );
	oBufferDesc.dwSize        = sizeof( oBufferDesc );
	oBufferDesc.dwFlags       = ( DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLPAN | DSBCAPS_CTRLFREQUENCY | DSBCAPS_GLOBALFOCUS | DSBCAPS_GETCURRENTPOSITION2 );
	oBufferDesc.dwBufferBytes = uBufferSize;
	oBufferDesc.lpwfxFormat   = &oPCMFormat;

	LPDIRECTSOUNDBUFFER8 poDSBuffer = NULL;
	HRESULT hResult = _poDS->CreateSoundBuffer( &oBufferDesc, (LPDIRECTSOUNDBUFFER *)&poDSBuffer, NULL );
	if( FAILED( hResult ) || ( ! poDSBuffer ) )
	{
		DEVPRINTF( "[ FAUDIO ] Error %u: CreateSoundBuffer() failed for \"%s\" (hr=0x%08x) !!!\n", __LINE__, szTemp, (unsigned)hResult );
		ffile_Close( poStream->ohFile );
		poStream->ohFile = FFILE_INVALID_HANDLE;
		return NULL;
	}

	u8 *pbyCompressScratch = new u8[ uCompressChunk ];
	u8 *pbyPcmScratch      = new u8[ uPcmChunk ];
	if( ( ! pbyCompressScratch ) || ( ! pbyPcmScratch ) )
	{
		DEVPRINTF( "[ FAUDIO ] Error %u: new[] failed for stream scratch !!!\n", __LINE__ );
		delete[] pbyCompressScratch;
		delete[] pbyPcmScratch;
		FDX8_SAFE_RELEASE( poDSBuffer );
		ffile_Close( poStream->ohFile );
		poStream->ohFile = FFILE_INVALID_HANDLE;
		return NULL;
	}

	// Zero the ring buffer so playback starts silent until the first refill.
	{
		LPVOID pData1 = NULL, pData2 = NULL;
		DWORD  dwBytes1 = 0, dwBytes2 = 0;

		if( SUCCEEDED( poDSBuffer->Lock( 0, 0, &pData1, &dwBytes1, &pData2, &dwBytes2, DSBLOCK_ENTIREBUFFER ) ) )
		{
			fang_MemZero( pData1, dwBytes1 );
			if( pData2 && dwBytes2 )
			{
				fang_MemZero( pData2, dwBytes2 );
			}
			poDSBuffer->Unlock( pData1, dwBytes1, pData2, dwBytes2 );
		}
	}

	// poAudioStream was wired in faudio_Install; keep the CFAudioStream::m_uData back-pointer current.
	poStream->poAudioStream->m_uData = (u32)poStream;

	poStream->poDSBuffer             = poDSBuffer;
	poStream->pbyCompressScratch     = pbyCompressScratch;
	poStream->pbyPcmScratch          = pbyPcmScratch;
	poStream->uCompressChunkBytes    = uCompressChunk;
	poStream->uPcmChunkBytes         = uPcmChunk;
	poStream->uBufferSize            = uBufferSize;
	poStream->uWritePos              = 0;
	poStream->uPlayPosLast           = 0;
	poStream->uBytesPerSecPCM        = uBytesPerSec;
	poStream->uDataStart             = sizeof( FDX8Data_WvsFile_Header_t );
	poStream->uDataBytes             = oHeader.nNumBytes;
	poStream->uBytesReadCompressed   = 0;
	poStream->oWavFormat             = oHeader.WavFormat;
	poStream->fFrequencyInHz         = oHeader.fFreqHz;
	poStream->fLengthInSeconds       = oHeader.fSecs;
	poStream->fSecondsPlayed         = 0.0f;
	poStream->fSecondsToPlay         = oHeader.fSecs;
	poStream->fVolume                = 1.0f;
	poStream->fPanLeftRight          = 0.0f;
	poStream->fFrequencyFactor       = 1.0f;
	poStream->bIsActive              = TRUE;
	poStream->bIsLooping             = FALSE;
	poStream->bIgnoreInPauseMode     = FALSE;
	poStream->bTreatAsSfx            = ( bWillBeUsedForMusic ) ? FALSE : TRUE;
	poStream->bEOF                   = FALSE;
	poStream->uStateChanges          = ( _STREAM_STATE_CHANGE_VOLUME | _STREAM_STATE_CHANGE_PAN | _STREAM_STATE_CHANGE_FREQUENCY );
	poStream->oeState                = FAUDIO_STREAM_STATE_STOPPED;
	poStream->ePauseLevel            = FAUDIO_PAUSE_LEVEL_1;
	poStream->pEndOfPlayCallback     = NULL;

	fclib_strncpyz( poStream->szName, pszName, FAUDIO_MAX_ASSET_NAME_LENGTH + 1 );

	_bSkipStreamsWorkDelay = FALSE;

	return poStream->poAudioStream;

} // CFAudioStream::Create

void CFAudioStream::Destroy( void )
{
	if( ! FAudio_bModuleInstalled )
	{
		return;
	}

	_Stream_t *poStream = (_Stream_t *)m_uData;

	if( ! poStream->bIsActive )
	{
		return;
	}

	if( FAUDIO_STREAM_STATE_PLAYING == poStream->oeState )
	{
		if( poStream->pEndOfPlayCallback )
		{
			poStream->pEndOfPlayCallback( poStream->poAudioStream );
		}

		if( poStream->poDSBuffer )
		{
			poStream->poDSBuffer->Stop();
		}
	}

	if( poStream->poDSBuffer )
	{
		// Drop any pending STOP so the work loop won't double-fire the callback.
		poStream->uStateChanges &= ~( _STREAM_STATE_CHANGE_STOP | _STREAM_STATE_CHANGE_PLAY | _STREAM_STATE_CHANGE_PAUSE | _STREAM_STATE_CHANGE_UNPAUSE );
		FDX8_SAFE_RELEASE( poStream->poDSBuffer );
	}

	if( FFILE_INVALID_HANDLE != poStream->ohFile )
	{
		ffile_Close( poStream->ohFile );
		poStream->ohFile = FFILE_INVALID_HANDLE;
	}

	delete[] poStream->pbyCompressScratch;
	delete[] poStream->pbyPcmScratch;
	poStream->pbyCompressScratch = NULL;
	poStream->pbyPcmScratch      = NULL;

	poStream->bIsActive  = FALSE;
	poStream->oeState    = FAUDIO_STREAM_STATE_NONE;
	poStream->szName[0]  = 0;

} // CFAudioStream::Destroy

void CFAudioStream::DestroyAll( void )
{
	if( ! FAudio_bModuleInstalled )
	{
		return;
	}

	for( u32 uIndex = 0; uIndex < _uMaxStreams; ++uIndex )
	{
		_Stream_t *poStream = &( _paoStreams[ uIndex ] );

		if( ! poStream->bIsActive )
		{
			continue;
		}

		poStream->poAudioStream->Destroy();
	}

} // CFAudioStream::DestroyAll

void CFAudioStream::SetVolume( f32 fVolume )
{
	if( ! FAudio_bModuleInstalled )
	{
		return;
	}

	FASSERT_MSG( FMATH_IS_UNIT_FLOAT( fVolume ), "[ FAUDIO ] Error: Invalid fVolume !!!" );

	_Stream_t *poStream = (_Stream_t *)m_uData;

	FASSERT_MSG( poStream->bIsActive, "[ FAUDIO ] Error: Invalid stream !!!" );

	if( ( FAUDIO_STREAM_STATE_ERROR    == poStream->oeState ) ||
		( FAUDIO_STREAM_STATE_NONE     == poStream->oeState ) )
	{
		return;
	}

	if( fVolume != poStream->fVolume )
	{
		poStream->uStateChanges |= _STREAM_STATE_CHANGE_VOLUME;
		poStream->fVolume        = fVolume;
		_bSkipStreamsWorkDelay   = FALSE;
	}

} // CFAudioStream::SetVolume

void CFAudioStream::SetPan( f32 fPanLeftRight )
{
	if( ! FAudio_bModuleInstalled )
	{
		return;
	}

	FASSERT_MSG( FMATH_IS_BIPOLAR_UNIT_FLOAT( fPanLeftRight ), "[ FAUDIO ] Error: Invalid fPanLeftRight !!!" );

	_Stream_t *poStream = (_Stream_t *)m_uData;

	FASSERT_MSG( poStream->bIsActive, "[ FAUDIO ] Error: Invalid stream !!!" );

	if( ( FAUDIO_STREAM_STATE_ERROR == poStream->oeState ) ||
		( FAUDIO_STREAM_STATE_NONE  == poStream->oeState ) )
	{
		return;
	}

	if( fPanLeftRight != poStream->fPanLeftRight )
	{
		poStream->uStateChanges |= _STREAM_STATE_CHANGE_PAN;
		poStream->fPanLeftRight  = fPanLeftRight;
		_bSkipStreamsWorkDelay   = FALSE;
	}

} // CFAudioStream::SetPan

void CFAudioStream::SetEndOfPlayCallback( FAudio_StreamEndOfPlayCallback_t *pEndOfPlayCallback )
{
	if( ! FAudio_bModuleInstalled )
	{
		return;
	}

	_Stream_t *poStream = (_Stream_t *)m_uData;

	FASSERT_MSG( poStream->bIsActive, "[ FAUDIO ] Error: Invalid stream !!!" );

	if( ( FAUDIO_STREAM_STATE_ERROR == poStream->oeState ) ||
		( FAUDIO_STREAM_STATE_NONE  == poStream->oeState ) )
	{
		return;
	}

	poStream->pEndOfPlayCallback = pEndOfPlayCallback;

} // CFAudioStream::SetEndOfPlayCallback

void CFAudioStream::SetFrequencyFactor( f32 fFrequencyFactor )
{
	if( ! FAudio_bModuleInstalled )
	{
		return;
	}

	FASSERT_MSG( ( 0.0f <= fFrequencyFactor ), "[ FAUDIO ] Error: Invalid fFrequencyFactor !!!" );

	_Stream_t *poStream = (_Stream_t *)m_uData;

	FASSERT_MSG( poStream->bIsActive, "[ FAUDIO ] Error: Invalid stream !!!" );

	if( ( FAUDIO_STREAM_STATE_ERROR == poStream->oeState ) ||
		( FAUDIO_STREAM_STATE_NONE  == poStream->oeState ) )
	{
		return;
	}

	if( fFrequencyFactor != poStream->fFrequencyFactor )
	{
		poStream->uStateChanges   |= _STREAM_STATE_CHANGE_FREQUENCY;
		poStream->fFrequencyFactor = fFrequencyFactor;
		_bSkipStreamsWorkDelay     = FALSE;
	}

} // CFAudioStream::SetFrequencyFactor

void CFAudioStream::Play( u32 uLoops /* = 1 */ )
{
	if( ! FAudio_bModuleInstalled )
	{
		return;
	}

	_Stream_t *poStream = (_Stream_t *)m_uData;

	FASSERT_MSG( poStream->bIsActive, "[ FAUDIO ] Error: Invalid stream !!!" );

	if( ( FAUDIO_STREAM_STATE_PLAYING  == poStream->oeState ) ||
		( FAUDIO_STREAM_STATE_ERROR    == poStream->oeState ) ||
		( FAUDIO_STREAM_STATE_NONE     == poStream->oeState ) ||
		( FAUDIO_STREAM_STATE_CREATING == poStream->oeState ) )
	{
		return;
	}

	// Rewind and reset bookkeeping for a fresh (or re-) play.
	_StreamSeekToStart( poStream );
	if( FAUDIO_STREAM_STATE_ERROR == poStream->oeState )
	{
		return;
	}

	poStream->fSecondsPlayed = 0.0f;
	poStream->uWritePos      = 0;
	poStream->uPlayPosLast   = 0;

	if( uLoops )
	{
		poStream->bIsLooping     = ( 1 < uLoops );
		poStream->fSecondsToPlay = ( poStream->fLengthInSeconds * (f32)uLoops );
	}
	else
	{
		poStream->bIsLooping     = TRUE;
		poStream->fSecondsToPlay = FAUDIO_UNLIMITED_SECONDS;
	}

	poStream->oeState        = FAUDIO_STREAM_STATE_PLAYING;
	poStream->uStateChanges &= ( ~ ( _STREAM_STATE_CHANGE_STOP | _STREAM_STATE_CHANGE_PAUSE | _STREAM_STATE_CHANGE_UNPAUSE ) );
	poStream->uStateChanges |= ( _STREAM_STATE_CHANGE_PLAY | _STREAM_STATE_CHANGE_VOLUME | _STREAM_STATE_CHANGE_PAN | _STREAM_STATE_CHANGE_FREQUENCY );

	// Prime the ring buffer with the first stretch of audio so Play() has data.
	_StreamApplyChanges( poStream );
	if( poStream->poDSBuffer )
	{
		poStream->poDSBuffer->Play( 0, 0, DSBPLAY_LOOPING );
	}

	_StreamRefill( poStream );

	_bSkipStreamsWorkDelay = FALSE;

} // CFAudioStream::Play

void CFAudioStream::Pause( BOOL bEnabled )
{
	if( ! FAudio_bModuleInstalled )
	{
		return;
	}

	_Stream_t *poStream = (_Stream_t *)m_uData;

	FASSERT_MSG( poStream->bIsActive, "[ FAUDIO ] Error: Invalid stream !!!" );

	if( ( FAUDIO_STREAM_STATE_STOPPED  == poStream->oeState ) ||
		( FAUDIO_STREAM_STATE_ERROR    == poStream->oeState ) ||
		( FAUDIO_STREAM_STATE_NONE     == poStream->oeState ) ||
		( FAUDIO_STREAM_STATE_CREATING == poStream->oeState ) )
	{
		return;
	}

	if( bEnabled )
	{
		if( FAUDIO_STREAM_STATE_PLAYING == poStream->oeState )
		{
			poStream->oeState        = FAUDIO_STREAM_STATE_PAUSED;
			poStream->uStateChanges &= ( ~ ( _STREAM_STATE_CHANGE_PLAY | _STREAM_STATE_CHANGE_UNPAUSE ) );
			poStream->uStateChanges |= _STREAM_STATE_CHANGE_PAUSE;
			_bSkipStreamsWorkDelay   = FALSE;
		}
	}
	else
	{
		if( FAUDIO_STREAM_STATE_PAUSED == poStream->oeState )
		{
			poStream->oeState        = FAUDIO_STREAM_STATE_PLAYING;
			poStream->uStateChanges &= ( ~ ( _STREAM_STATE_CHANGE_PLAY | _STREAM_STATE_CHANGE_PAUSE ) );
			poStream->uStateChanges |= _STREAM_STATE_CHANGE_UNPAUSE;
			_bSkipStreamsWorkDelay   = FALSE;
		}
	}

} // CFAudioStream::Pause

void CFAudioStream::Stop( BOOL bAfterCurrentLoop )
{
	if( ! FAudio_bModuleInstalled )
	{
		return;
	}

	_Stream_t *poStream = (_Stream_t *)m_uData;

	FASSERT_MSG( poStream->bIsActive, "[ FAUDIO ] Error: Invalid stream !!!" );

	if( ( FAUDIO_STREAM_STATE_STOPPED  == poStream->oeState ) ||
		( FAUDIO_STREAM_STATE_ERROR    == poStream->oeState ) ||
		( FAUDIO_STREAM_STATE_NONE     == poStream->oeState ) ||
		( FAUDIO_STREAM_STATE_CREATING == poStream->oeState ) )
	{
		return;
	}

	poStream->bIsLooping = FALSE;

	if( ! bAfterCurrentLoop )
	{
		_StreamFinish( poStream );
		_bSkipStreamsWorkDelay = FALSE;
	}
	else
	{
		// Let the current loop finish, then stop.
		poStream->fSecondsToPlay = ( ( poStream->fLengthInSeconds * (f32)( (u32)( poStream->fSecondsPlayed / poStream->fLengthInSeconds ) ) ) + poStream->fLengthInSeconds );
	}

} // CFAudioStream::Stop

FAudio_StreamState_e CFAudioStream::GetState( void )
{
	if( ! FAudio_bModuleInstalled )
	{
		return FAUDIO_STREAM_STATE_ERROR;
	}

	_Stream_t *poStream = (_Stream_t *)m_uData;

	FASSERT_MSG( poStream->bIsActive, "[ FAUDIO ] Error: Invalid stream !!!" );

	return poStream->oeState;

} // CFAudioStream::GetState

f32 CFAudioStream::GetSecondsPlayed( void )
{
	if( ! FAudio_bModuleInstalled )
	{
		return 0.0f;
	}

	_Stream_t *poStream = (_Stream_t *)m_uData;

	FASSERT_MSG( poStream->bIsActive, "[ FAUDIO ] Error: Invalid stream !!!" );

	return poStream->fSecondsPlayed;

} // CFAudioStream::GetSecondsPlayed

f32 CFAudioStream::GetSecondsToPlay( void )
{
	if( ! FAudio_bModuleInstalled )
	{
		return 0.0f;
	}

	_Stream_t *poStream = (_Stream_t *)m_uData;

	FASSERT_MSG( poStream->bIsActive, "[ FAUDIO ] Error: Invalid stream !!!" );

	return poStream->fSecondsToPlay;

} // CFAudioStream::GetSecondsToPlay

FAudio_PauseLevel_e CFAudioStream::SetGlobalPauseLevel( FAudio_PauseLevel_e eNewPauseLevel )
{
	if( ! FAudio_bModuleInstalled )
	{
		return FAUDIO_PAUSE_LEVEL_NONE;
	}

	FAudio_PauseLevel_e eOldPauseLevel = _ePauseLevelStreams;

	for( u32 uIndex = 0; uIndex < _uMaxStreams; ++uIndex )
	{
		_Stream_t *poStream = &( _paoStreams[ uIndex ] );

		if( ! poStream->bIsActive )
		{
			continue;
		}

		if( poStream->ePauseLevel <= eNewPauseLevel )
		{
			poStream->poAudioStream->Pause( TRUE );
			poStream->bIgnoreInPauseMode = TRUE;
		}
		else if( poStream->bIgnoreInPauseMode )
		{
			poStream->poAudioStream->Pause( FALSE );
			poStream->bIgnoreInPauseMode = FALSE;
		}
	}

	_ePauseLevelStreams = eNewPauseLevel;

	return eOldPauseLevel;

} // CFAudioStream::SetGlobalPauseLevel

FAudio_PauseLevel_e CFAudioStream::GetGlobalPauseLevel( void )
{
	if( ! FAudio_bModuleInstalled )
	{
		return FAUDIO_PAUSE_LEVEL_NONE;
	}

	return ( _ePauseLevelStreams );

} // CFAudioStream::GetGlobalPauseLevel

FAudio_PauseLevel_e CFAudioStream::GetPauseLevel( void )
{
	if( ! FAudio_bModuleInstalled )
	{
		return FAUDIO_PAUSE_LEVEL_NONE;
	}

	_Stream_t *poStream = (_Stream_t *)m_uData;

	FASSERT_MSG( poStream->bIsActive, "[ FAUDIO ] Error: Invalid stream !!!" );

	return poStream->ePauseLevel;

} // CFAudioStream::GetPauseLevel

void CFAudioStream::SetPauseLevel( FAudio_PauseLevel_e eNewPauseLevel )
{
	if( ! FAudio_bModuleInstalled )
	{
		return;
	}

	_Stream_t *poStream = (_Stream_t *)m_uData;

	FASSERT_MSG( poStream->bIsActive, "[ FAUDIO ] Error: Invalid stream !!!" );

	poStream->ePauseLevel = eNewPauseLevel;

} // CFAudioStream::SetPauseLevel

u32 CFAudioStream::GetNumActive( void )
{
	if( ! FAudio_bModuleInstalled )
	{
		return 0;
	}

	u32 uCount = 0;

	for( u32 uIndex = 0; uIndex < _uMaxStreams; ++uIndex )
	{
		if( _paoStreams[ uIndex ].bIsActive )
		{
			++uCount;
		}
	}

	return uCount;

} // CFAudioStream::GetNumActive

cchar *CFAudioStream::GetName( void )
{
	if( ! FAudio_bModuleInstalled )
	{
		return NULL;
	}

	_Stream_t *poStream = (_Stream_t *)m_uData;

	FASSERT_MSG( poStream->bIsActive, "[ FAUDIO ] Error: Invalid stream !!!" );

	return poStream->szName;

} // CFAudioStream::GetName

cchar *CFAudioStream::GetName( u32 uIndex )
{
	if( ! FAudio_bModuleInstalled )
	{
		return NULL;
	}

	u32 uTemp, uCount = 0;

	for( uTemp = 0; uTemp < _uMaxStreams; ++uTemp )
	{
		if( _paoStreams[ uTemp ].bIsActive )
		{
			if( uCount == uIndex )
			{
				return _paoStreams[ uTemp ].szName;
			}
			++uCount;
		}
	}

	return NULL;

} // CFAudioStream::GetName
