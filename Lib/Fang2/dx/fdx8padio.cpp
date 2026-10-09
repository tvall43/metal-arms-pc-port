//////////////////////////////////////////////////////////////////////////////////////
// fdx8padio.cpp - 
//
// Author: Albert Yale
//////////////////////////////////////////////////////////////////////////////////////
// THIS CODE IS PROPRIETARY PROPERTY OF SWINGIN' APE STUDIOS, INC.
// Copyright (c) 2001
//
// The contents of this file may not be disclosed to third
// parties, copied or duplicated in any form, in whole or in part,
// without the prior written permission of Swingin' Ape Studios, Inc.
//////////////////////////////////////////////////////////////////////////////////////
// Modification History:
//
// Date     Who         Description
// -------- ----------  --------------------------------------------------------------
// 11/16/01 ayale       Created.
//////////////////////////////////////////////////////////////////////////////////////

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

#include <stdio.h>
#include "mfc.h"    // stale MFC include; this file uses nothing from MFC
#include <dinput.h>

#include "fang.h"
#include "fdx8.h"
#include "fdx8vid.h"	// fdx8vid_HaveFocus -- keyboard synth focus gate
#include "fmath.h"
#include "fres.h"
#include "fclib.h"
#include "fpadio.h"



#define _AXIS_RANGE_MAX						1000
#define _OO_AXIS_RANGE_MAX					( 1.0f / ( (f32)( _AXIS_RANGE_MAX ) ) )

#define _SLOWEST_SAMPLING_RATE_SUPPORTED	10
#define _FASTEST_SAMPLING_RATE_SUPPORTED	90

#define _DEADZONE_STICKS					0.2f
#define _OO_ONE_MINUS_DEADZONE				( 1.0f / ( 1.0f - _DEADZONE_STICKS ) )


typedef struct {
	BOOL bEnumerated, bAcquired;
	LPDIRECTINPUTDEVICE8 poDIDevice;

	// Input modernization phase 3 (decisions D4/D5): a slot can be filled by
	// an XInput controller instead of a DirectInput device, and slot 0 can
	// additionally merge the keyboard synth so kb+mouse always co-sources
	// port 0 (live kb<->pad swap with no mode change).
	BOOL bXInput;
	u32 uXInputSlot;
	BOOL bSynthMerged;

	FPadio_DeviceInfo_t oInfo;

	u32 uAxis, uButtons;

	u8 auAxesOffset[ 8 ]; // Can't have more than 8 axes.
} _Device_t;

typedef struct {
	u32 uCurrentDeviceIndex, uAxisIndex;
} _EnumAxesCallbackParam_t;

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

// Module.
static BOOL _bModuleStarted = FALSE;
static BOOL _bModuleInstalled = FALSE;

// Devices.
static _Device_t _aoDevices[ FPADIO_MAX_DEVICES ];

// Input Emulation.
static u32 _auInputEmulationPlatform[ FPADIO_MAX_DEVICES ], _auInputEmulationMap[ FPADIO_MAX_INPUTS ];

// Handles.
static LPDIRECTINPUT8 _pDInput;

//// XInput (input modernization phase 3, decision D4).
//
// Controllers are read through XInput rather than DirectInput: Xbox pads have
// a fixed layout, so translation is exact, and Wine exposes Xbox controllers
// through the same DLLs. Loaded at runtime (no import library, no xinput.h
// dependency) -- the few types and masks needed are declared here with the
// values every Windows/Wine version has shipped since Vista.
//
typedef struct {
	WORD wButtons;
	BYTE bLeftTrigger;
	BYTE bRightTrigger;
	SHORT sThumbLX;
	SHORT sThumbLY;
	SHORT sThumbRX;
	SHORT sThumbRY;
} _XInputGamepad_t;

typedef struct {
	DWORD dwPacketNumber;
	_XInputGamepad_t Gamepad;
} _XInputState_t;

typedef struct {
	WORD wLeftMotorSpeed;
	WORD wRightMotorSpeed;
} _XInputVibration_t;

typedef DWORD (WINAPI *_PFN_XInputGetState)( DWORD dwUserIndex, _XInputState_t *pState );
typedef DWORD (WINAPI *_PFN_XInputSetState)( DWORD dwUserIndex, const _XInputVibration_t *pVibration );

#define _XINPUT_OK                          0L
#define _XINPUT_ERROR_DEVICE_NOT_CONNECTED 1167L

#define _XINPUT_GAMEPAD_DPAD_UP        0x0001
#define _XINPUT_GAMEPAD_DPAD_DOWN      0x0002
#define _XINPUT_GAMEPAD_DPAD_LEFT      0x0004
#define _XINPUT_GAMEPAD_DPAD_RIGHT     0x0008
#define _XINPUT_GAMEPAD_START          0x0010
#define _XINPUT_GAMEPAD_BACK           0x0020
#define _XINPUT_GAMEPAD_LEFT_THUMB     0x0040
#define _XINPUT_GAMEPAD_RIGHT_THUMB    0x0080
#define _XINPUT_GAMEPAD_LEFT_SHOULDER  0x0100
#define _XINPUT_GAMEPAD_RIGHT_SHOULDER 0x0200
#define _XINPUT_GAMEPAD_A              0x1000
#define _XINPUT_GAMEPAD_B              0x2000
#define _XINPUT_GAMEPAD_X              0x4000
#define _XINPUT_GAMEPAD_Y              0x8000

static HMODULE _hXInputModule;
static _PFN_XInputGetState _pfnXInputGetState;
static _PFN_XInputSetState _pfnXInputSetState;
static BOOL _abXInputPrevConnected[ FPADIO_MAX_DEVICES ];

// Motor request per device: written by fpadio_SetForcefeedback_XB (called
// from fforce_Work on the game thread), dispatched by
// fpadio_ApplyAllForcefeedbacks once per frame (decision D6).
static f32 _afMotorCoarse[ FPADIO_MAX_DEVICES ], _afMotorFine[ FPADIO_MAX_DEVICES ];

static BOOL _XInputLoad( void )
{
	static const char *const _apszModules[] = { "xinput1_4.dll", "xinput9_1_0.dll", "xinput1_3.dll" };
	u32 uIndex;

	if( _pfnXInputGetState )
	{
		return TRUE;
	}

	for( uIndex = 0; uIndex < ( sizeof( _apszModules ) / sizeof( _apszModules[ 0 ] ) ); ++uIndex )
	{
		_hXInputModule = LoadLibraryA( _apszModules[ uIndex ] );
		if( !_hXInputModule )
		{
			continue;
		}

		_pfnXInputGetState = (_PFN_XInputGetState)GetProcAddress( _hXInputModule, "XInputGetState" );
		_pfnXInputSetState = (_PFN_XInputSetState)GetProcAddress( _hXInputModule, "XInputSetState" );
		if( _pfnXInputGetState && _pfnXInputSetState )
		{
			DEVPRINTF( "[ma] fpadio: XInput loaded from %s.\n", _apszModules[ uIndex ] );
			return TRUE;
		}

		_pfnXInputGetState = NULL;
		_pfnXInputSetState = NULL;
		FreeLibrary( _hXInputModule );
		_hXInputModule = NULL;
	}

	DEVPRINTF( "[ma] fpadio: no XInput DLL found; DirectInput/keyboard only.\n" );
	return FALSE;
}

static void _XInputUnload( void )
{
	if( _hXInputModule )
	{
		FreeLibrary( _hXInputModule );
		_hXInputModule = NULL;
	}
	_pfnXInputGetState = NULL;
	_pfnXInputSetState = NULL;
}

static u32 _XInputConnectedCount( void )
{
	u32 uIndex, uCount = 0;
	_XInputState_t oState;

	if( !_pfnXInputGetState )
	{
		return 0;
	}

	for( uIndex = 0; uIndex < FPADIO_MAX_DEVICES; ++uIndex )
	{
		if( _XINPUT_OK == _pfnXInputGetState( uIndex, &oState ) )
		{
			++uCount;
		}
	}

	return uCount;
}

// Double sample buffers.
static FPadio_Sample_t **_papaoSamplesA;
static FPadio_Sample_t **_papaoSamplesB;
static FPadio_Sample_t **_papaoCurrentSamples;
static u32 _uValidSamples;
static u32 _uMiliSecBetweenSamples, _uSamplesPerBuffer;

// Thread.
static HANDLE _hPollingThread, _hPollingThreadMutex, _hSamplesAndDevicesMutex;

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static DWORD WINAPI _fpadio_SamplingThread( LPVOID pParam );
static BOOL CALLBACK _EnumDevicesCallback( LPCDIDEVICEINSTANCE pDevInst, LPVOID pParam );
static BOOL CALLBACK _EnumAxesCallback( LPCDIDEVICEOBJECTINSTANCE pDevInst, LPVOID pParam );


static FINLINE void _RenormalizeStick( f32 &rfX, f32 &rfY ) {
	f32 fMag2, fAngle, fMultipler, fSin, fCos;

	// fixup the right x,y axis
	fMag2 = ( rfX * rfX ) + ( rfY * rfY );
	if( fMag2 > 0.0f ) {
		// Get the angle that x y represents.
		fAngle = fmath_Atan( rfY, rfX );

		fmath_SinCos( fAngle, &fSin, &fCos );
		fMultipler = fmath_Inv( FMATH_MAX( FMATH_FABS( fSin ), FMATH_FABS( fCos ) ) );
		fMultipler *= _OO_ONE_MINUS_DEADZONE;

		rfX *= fMultipler;
		rfY *= fMultipler;

		// see if we need to clamp the length
		fMag2 = ( rfX * rfX ) + ( rfY * rfY );
		if( fMag2 > 1.0f ) {
			fMultipler = fmath_InvSqrt( fMag2 );
			rfX *= fMultipler;
			rfY *= fMultipler;
		}							
	}
}

static FINLINE void _ApplyDeadzone( f32 &rfValue ) {
	
	if( rfValue < -_DEADZONE_STICKS ) {
		rfValue += _DEADZONE_STICKS;
	} else if( rfValue > _DEADZONE_STICKS ) {
		rfValue -= _DEADZONE_STICKS;
	} else {
		rfValue = 0.0f;
	}
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

//// Synthetic keyboard device.
//
// Metal Arms is a console title: it has no keyboard path of its own, every
// action is read through Gamepad_aapSample[][] and fpadio only ever enumerates
// DirectInput game controllers. With no controller attached (always true under
// Wine, common on a PC) fpadio_Install used to fail outright, fpad then had
// nothing to sample, and the game became permanently uncontrolable. Slot 0 is
// given a synthetic device instead, whose samples are read straight from the
// keyboard, so fpad, gamepad and every consumer downstream run unchanged.
//
static BOOL _KeyDown( int nVK ) {
	return ( 0 != ( GetAsyncKeyState( nVK ) & 0x8000 ) );
}

// Mouse wheel -> weapon cycle pulse (phase 1). fdx8vid.cpp accumulates the
// wheel notches; the synthetic device turns them into a short press of
// SELECT_PRIMARY (scroll) or SELECT_SECONDARY (Q held + scroll, decision D6).
// The press is held for _WHEEL_PULSE_MS so fpad's once-per-frame latch
// sampling always sees the edge no matter where in the frame it lands; the
// nonzero guard on the deadlines keeps GetTickCount's 49-day wrap from
// looking like an active pulse.
static LONG _nWheelPulsePrimaryUntil   = 0;
static LONG _nWheelPulseSecondaryUntil = 0;
#define _WHEEL_PULSE_MS 60

// [KEYMAP] overrides (phase 2): virtual-key codes indexed by
// (FPadio_InputID_e - 1); 0 keeps the default binding below. Populated by
// fpadio_SetSynthKeyOverrides() from ma_win.ini before the game loop starts,
// so the sampling thread only ever reads it. Buttons only -- sticks and the
// d-pad keep their fixed keys.
static u32 _auSynthKeyVK[ FPADIO_MAX_INPUTS ];

void fpadio_SetSynthKeyOverrides( const u32 *pauVKPerInput )
{
	if( NULL == pauVKPerInput ) {
		fang_MemZero( _auSynthKeyVK, sizeof( _auSynthKeyVK ) );
	} else {
		fang_MemCopy( _auSynthKeyVK, pauVKPerInput, sizeof( _auSynthKeyVK ) );
	}
}

// TRUE if the bound key for this input is down -- the [KEYMAP] override when
// one was supplied for nInputID, otherwise the compiled-in default.
static BOOL _KeyBound( int nInputID, int nDefaultVK )
{
	u32 uVK = _auSynthKeyVK[ nInputID - 1 ];
	return _KeyDown( ( 0 != uVK ) ? (int)uVK : nDefaultVK );
}

static void _KeyboardSyntheticSample( FPadio_Sample_t *poSample )
{
	static const f32 _fSqrtHalf = 0.70710678118654752440f;
	f32 fX, fY;
	f32 *paf = poSample->afInputValues;

	fang_MemZero( paf, sizeof( poSample->afInputValues ) );
	poSample->bValid = TRUE;

	//// Focus gate: the keyboard is global (GetAsyncKeyState reads the state of
	//// the physical keyboard no matter which window owns it), so without this
	//// gate every key held in another application -- Alt+Tabbed back to a
	//// terminal, a browser, the debugger -- would keep driving the game: W would
	//// walk, the mouse buttons would fire, Escape would open the pause menu.
	//// While the game window does not have focus, report an idle-but-connected
	//// controller instead (bValid stays TRUE so the port stays online).
	//
	if( !fdx8vid_HaveFocus() ) {
		return;
	}

	//// Left stick -- W/A/S/D.
	//
	// This alone drives the bootup menu: launcher.cpp reads
	// GAMEPAD_MENU_LEFT_ANALOG_Y first and only falls back to the d-pad.
	//
	fX = 0.0f;
	fY = 0.0f;
	if( _KeyDown( 'D' ) ) { fX += 1.0f; }
	if( _KeyDown( 'A' ) ) { fX -= 1.0f; }
	if( _KeyDown( 'W' ) ) { fY += 1.0f; }
	if( _KeyDown( 'S' ) ) { fY -= 1.0f; }
	if( ( 0.0f != fX ) && ( 0.0f != fY ) ) { fX *= _fSqrtHalf; fY *= _fSqrtHalf; }
	paf[ FPADIO_INPUT_STICK_LEFT_X - 1 ] = fX;
	paf[ FPADIO_INPUT_STICK_LEFT_Y - 1 ] = fY;

	//// Right stick -- I/J/K/L, look up/down and turn.
	//
	fX = 0.0f;
	fY = 0.0f;
	if( _KeyDown( 'L' ) ) { fX += 1.0f; }
	if( _KeyDown( 'J' ) ) { fX -= 1.0f; }
	if( _KeyDown( 'I' ) ) { fY += 1.0f; }
	if( _KeyDown( 'K' ) ) { fY -= 1.0f; }
	if( ( 0.0f != fX ) && ( 0.0f != fY ) ) { fX *= _fSqrtHalf; fY *= _fSqrtHalf; }
	paf[ FPADIO_INPUT_STICK_RIGHT_X - 1 ] = fX;
	paf[ FPADIO_INPUT_STICK_RIGHT_Y - 1 ] = fY;

	//// D-pad -- the arrow keys, which is also how the bootup menu is moved
	//// when the left stick is idle (launcher.cpp falls back to DPAD_Y).
	//
	fX = 0.0f;
	fY = 0.0f;
	if( _KeyDown( VK_RIGHT ) ) { fX += 1.0f; }
	if( _KeyDown( VK_LEFT )  ) { fX -= 1.0f; }
	if( _KeyDown( VK_UP )    ) { fY += 1.0f; }
	if( _KeyDown( VK_DOWN )  ) { fY -= 1.0f; }
	if( ( 0.0f != fX ) && ( 0.0f != fY ) ) { fX *= _fSqrtHalf; fY *= _fSqrtHalf; }
	paf[ FPADIO_INPUT_DPAD_X - 1 ] = fX;
	paf[ FPADIO_INPUT_DPAD_Y - 1 ] = fY;

	//// Face buttons and shoulders.
	//
	paf[ FPADIO_INPUT_CROSS_BOTTOM          - 1 ] = _KeyBound( FPADIO_INPUT_CROSS_BOTTOM, VK_SPACE ) ? 1.0f : 0.0f; // A: accept, jump, select
	paf[ FPADIO_INPUT_CROSS_RIGHT           - 1 ] = _KeyBound( FPADIO_INPUT_CROSS_RIGHT,  VK_TAB )    ? 1.0f : 0.0f; // B: back, cancel, primary weapon select
	paf[ FPADIO_INPUT_CROSS_LEFT            - 1 ] = _KeyBound( FPADIO_INPUT_CROSS_LEFT,   'Q' )      ? 1.0f : 0.0f; // X
	paf[ FPADIO_INPUT_CROSS_TOP             - 1 ] = _KeyBound( FPADIO_INPUT_CROSS_TOP,    'E' )      ? 1.0f : 0.0f; // Y: action, interact
	// Start: pause (Escape opens the pause menu instead of quitting, gameloop.cpp).
	// Defaults are a key OR-set; a [KEYMAP] override replaces the whole set with one key.
	paf[ FPADIO_INPUT_START                 - 1 ] = ( ( 0 != _auSynthKeyVK[ FPADIO_INPUT_START - 1 ] )
		? _KeyBound( FPADIO_INPUT_START, 0 )
		: ( _KeyDown( VK_RETURN ) || _KeyDown( 'P' ) || _KeyDown( VK_ESCAPE ) ) ) ? 1.0f : 0.0f;
	paf[ FPADIO_INPUT_XB_DBUTTON_BACK       - 1 ] = _KeyBound( FPADIO_INPUT_XB_DBUTTON_BACK, VK_BACK ) ? 1.0f : 0.0f; // Back
	paf[ FPADIO_INPUT_TRIGGER_RIGHT         - 1 ] = ( ( 0 != _auSynthKeyVK[ FPADIO_INPUT_TRIGGER_RIGHT - 1 ] )
		? _KeyBound( FPADIO_INPUT_TRIGGER_RIGHT, 0 )
		: ( _KeyDown( VK_LBUTTON ) || _KeyDown( VK_LCONTROL ) ) ) ? 1.0f : 0.0f; // RT: fire
	paf[ FPADIO_INPUT_TRIGGER_LEFT          - 1 ] = ( ( 0 != _auSynthKeyVK[ FPADIO_INPUT_TRIGGER_LEFT - 1 ] )
		? _KeyBound( FPADIO_INPUT_TRIGGER_LEFT, 0 )
		: ( _KeyDown( VK_RBUTTON ) || _KeyDown( VK_LSHIFT ) ) ) ? 1.0f : 0.0f;    // LT: aim
	paf[ FPADIO_INPUT_XB_ABUTTON_BLACK      - 1 ] = _KeyBound( FPADIO_INPUT_XB_ABUTTON_BLACK, 'X' ) ? 1.0f : 0.0f;
	paf[ FPADIO_INPUT_XB_ABUTTON_WHITE      - 1 ] = _KeyBound( FPADIO_INPUT_XB_ABUTTON_WHITE, 'C' ) ? 1.0f : 0.0f;
	paf[ FPADIO_INPUT_XB_DBUTTON_STICK_LEFT - 1 ] = _KeyBound( FPADIO_INPUT_XB_DBUTTON_STICK_LEFT,  VK_LMENU ) ? 1.0f : 0.0f; // LS: crouch
	paf[ FPADIO_INPUT_XB_DBUTTON_STICK_RIGHT- 1 ] = _KeyBound( FPADIO_INPUT_XB_DBUTTON_STICK_RIGHT, 'R' )      ? 1.0f : 0.0f; // RS: melee

	//// Mouse wheel weapon cycling (phase 1): scroll = primary, the secondary
	//// select key + scroll = secondary (decision D6 -- the key follows the
	//// [KEYMAP] override). Applied last so a pulse wins over a held key.
	//
	{
		LONG nNotch = fdx8vid_ConsumeMouseWheel();
		if( 0 != nNotch ) {
			if( _KeyBound( FPADIO_INPUT_CROSS_LEFT, 'Q' ) ) {
				_nWheelPulseSecondaryUntil = (LONG)GetTickCount() + _WHEEL_PULSE_MS;
			} else {
				_nWheelPulsePrimaryUntil = (LONG)GetTickCount() + _WHEEL_PULSE_MS;
			}
		}
		if( ( 0 != _nWheelPulsePrimaryUntil ) && ( (LONG)( GetTickCount() - (DWORD)_nWheelPulsePrimaryUntil ) < 0 ) ) {
			paf[ FPADIO_INPUT_CROSS_RIGHT - 1 ] = 1.0f;
		}
		if( ( 0 != _nWheelPulseSecondaryUntil ) && ( (LONG)( GetTickCount() - (DWORD)_nWheelPulseSecondaryUntil ) < 0 ) ) {
			paf[ FPADIO_INPUT_CROSS_LEFT - 1 ] = 1.0f;
		}
	}
}

// Decision D5 -- per-input precedence when the keyboard synth co-sources a
// port that also has a pad (slot 0): sticks and d-pad take the pad value once
// it moves beyond the deadzone (so a centered stick never fights the keys),
// buttons and triggers add together clamped to 1, and the port reports
// connected if either source is. Only port 0 ever gets this; pads in slots
// 1..3 are keyboard-free, which is what keeps local multiplayer honest.
static void _MergeKeyboardInto( FPadio_Sample_t *poPad, FPadio_Sample_t *poScratch )
{
	u32 uInput;
	f32 fPad, fKey;

	_KeyboardSyntheticSample( poScratch );

	for( uInput = 0; uInput < FPADIO_MAX_INPUTS; ++uInput )
	{
		fPad = poPad->afInputValues[ uInput ];
		fKey = poScratch->afInputValues[ uInput ];

		switch( (FPadio_InputID_e)( uInput + 1 ) )
		{
			case FPADIO_INPUT_STICK_LEFT_X:
			case FPADIO_INPUT_STICK_LEFT_Y:
			case FPADIO_INPUT_STICK_RIGHT_X:
			case FPADIO_INPUT_STICK_RIGHT_Y:
			case FPADIO_INPUT_DPAD_X:
			case FPADIO_INPUT_DPAD_Y:
				poPad->afInputValues[ uInput ] = ( FMATH_FABS( fPad ) > _DEADZONE_STICKS ) ? fPad : fKey;
				break;

			default:
				fPad += fKey;
				FMATH_CLAMP( fPad, 0.0f, 1.0f );
				poPad->afInputValues[ uInput ] = fPad;
				break;
		}
	}

	poPad->bValid = poPad->bValid || poScratch->bValid;
}

// Phase 3 -- translate one XInput controller into an fpadio sample. Sign and
// mapping conventions match the keyboard synth exactly: stick up = +Y, right
// = +X, d-pad likewise, A/B/X/Y -> CROSS_BOTTOM/RIGHT/LEFT/TOP, bumpers ->
// White/Black (the original Xbox face), stick clicks -> crouch/melee, Start/
// Back where they belong; triggers stay analog 0..1. The focus gate follows
// the phase-0 policy for the keyboard: while the window is inactive report an
// idle-but-connected controller instead of letting a stray shoulder button
// drive an unfocused game.
static void _XInputSample( u32 uSlot, FPadio_Sample_t *poSample )
{
	const _XInputGamepad_t *poPad;
	_XInputState_t oState;
	BOOL bConnected;
	f32 *paf = poSample->afInputValues;
	f32 fX, fY;

	fang_MemZero( paf, sizeof( poSample->afInputValues ) );

	bConnected = _pfnXInputGetState && ( _XINPUT_OK == _pfnXInputGetState( uSlot, &oState ) );

	// Hotplug: log the edges only (phase-3 acceptance -- unplug/replug live).
	if( bConnected != _abXInputPrevConnected[ uSlot ] )
	{
		_abXInputPrevConnected[ uSlot ] = bConnected;
		DEVPRINTF( "[ma] fpadio: xinput slot %u %s.\n", uSlot, bConnected ? "connected" : "disconnected" );
	}

	if( !bConnected )
	{
		poSample->bValid = FALSE;
		return;
	}

	poSample->bValid = TRUE;

	if( !fdx8vid_HaveFocus() )
	{
		return;
	}

	poPad = &( oState.Gamepad );

	//// Left stick -- deadzone + renormalize exactly like the DirectInput path
	//// so pads feel identical whichever way they are read. XInput already
	//// reports up as +Y, which is the game's convention (W and I both read
	//// +1 in the synth), so there is no Y negation here.
	//
	fX = (f32)poPad->sThumbLX * ( 1.0f / 32767.0f );
	fY = (f32)poPad->sThumbLY * ( 1.0f / 32767.0f );
	_ApplyDeadzone( fX );
	_ApplyDeadzone( fY );
	_RenormalizeStick( fX, fY );
	paf[ FPADIO_INPUT_STICK_LEFT_X - 1 ] = fX;
	paf[ FPADIO_INPUT_STICK_LEFT_Y - 1 ] = fY;

	//// Right stick.
	//
	fX = (f32)poPad->sThumbRX * ( 1.0f / 32767.0f );
	fY = (f32)poPad->sThumbRY * ( 1.0f / 32767.0f );
	_ApplyDeadzone( fX );
	_ApplyDeadzone( fY );
	_RenormalizeStick( fX, fY );
	paf[ FPADIO_INPUT_STICK_RIGHT_X - 1 ] = fX;
	paf[ FPADIO_INPUT_STICK_RIGHT_Y - 1 ] = fY;

	//// D-pad -- digital, same signs as the arrow keys in the synth.
	//
	fX = 0.0f;
	fY = 0.0f;
	if( poPad->wButtons & _XINPUT_GAMEPAD_DPAD_RIGHT ) { fX += 1.0f; }
	if( poPad->wButtons & _XINPUT_GAMEPAD_DPAD_LEFT )  { fX -= 1.0f; }
	if( poPad->wButtons & _XINPUT_GAMEPAD_DPAD_UP )    { fY += 1.0f; }
	if( poPad->wButtons & _XINPUT_GAMEPAD_DPAD_DOWN )  { fY -= 1.0f; }
	paf[ FPADIO_INPUT_DPAD_X - 1 ] = fX;
	paf[ FPADIO_INPUT_DPAD_Y - 1 ] = fY;

	//// Triggers -- analog (the retail DirectInput-Xbox emulation read them as
	//// digital buttons; XInput gives the real axis, which the game handles
	//// because that is what the console did).
	//
	paf[ FPADIO_INPUT_TRIGGER_LEFT  - 1 ] = (f32)poPad->bLeftTrigger  * ( 1.0f / 255.0f );
	paf[ FPADIO_INPUT_TRIGGER_RIGHT - 1 ] = (f32)poPad->bRightTrigger * ( 1.0f / 255.0f );

	//// Buttons.
	//
	paf[ FPADIO_INPUT_START                 - 1 ] = ( poPad->wButtons & _XINPUT_GAMEPAD_START          ) ? 1.0f : 0.0f;
	paf[ FPADIO_INPUT_XB_DBUTTON_BACK       - 1 ] = ( poPad->wButtons & _XINPUT_GAMEPAD_BACK           ) ? 1.0f : 0.0f;
	paf[ FPADIO_INPUT_CROSS_BOTTOM          - 1 ] = ( poPad->wButtons & _XINPUT_GAMEPAD_A              ) ? 1.0f : 0.0f;
	paf[ FPADIO_INPUT_CROSS_RIGHT           - 1 ] = ( poPad->wButtons & _XINPUT_GAMEPAD_B              ) ? 1.0f : 0.0f;
	paf[ FPADIO_INPUT_CROSS_LEFT            - 1 ] = ( poPad->wButtons & _XINPUT_GAMEPAD_X              ) ? 1.0f : 0.0f;
	paf[ FPADIO_INPUT_CROSS_TOP             - 1 ] = ( poPad->wButtons & _XINPUT_GAMEPAD_Y              ) ? 1.0f : 0.0f;
	paf[ FPADIO_INPUT_XB_ABUTTON_WHITE      - 1 ] = ( poPad->wButtons & _XINPUT_GAMEPAD_LEFT_SHOULDER  ) ? 1.0f : 0.0f;
	paf[ FPADIO_INPUT_XB_ABUTTON_BLACK      - 1 ] = ( poPad->wButtons & _XINPUT_GAMEPAD_RIGHT_SHOULDER ) ? 1.0f : 0.0f;
	paf[ FPADIO_INPUT_XB_DBUTTON_STICK_LEFT - 1 ] = ( poPad->wButtons & _XINPUT_GAMEPAD_LEFT_THUMB     ) ? 1.0f : 0.0f;
	paf[ FPADIO_INPUT_XB_DBUTTON_STICK_RIGHT- 1 ] = ( poPad->wButtons & _XINPUT_GAMEPAD_RIGHT_THUMB    ) ? 1.0f : 0.0f;
}

// Phase 3 -- fill all four fpadio slots with XInput devices. Every slot is
// registered whether or not a controller is attached right now: an invalid
// sample marks the port offline for as long as the pad is absent and the next
// XInputGetState brings it straight back (hotplug), while slot 0 additionally
// merges the keyboard synth so P1 can swap kb<->pad live (decision D5).
static void _RegisterXInputDevices( void )
{
	u32 uIndex, uInput;
	_Device_t *poDevice;
	char szName[ 64 ];

	for( uIndex = 0; uIndex < FPADIO_MAX_DEVICES; ++uIndex )
	{
		poDevice = &( _aoDevices[ uIndex ] );

		sprintf( szName, "Xbox Controller %u (XInput)", uIndex + 1 );

		poDevice->bEnumerated  = FALSE; // Skips the DirectInput setup loop below.
		poDevice->bAcquired    = TRUE;  // The sampling thread fills it.
		poDevice->poDIDevice   = NULL;  // No DirectInput object to poll or release.
		poDevice->bXInput      = TRUE;
		poDevice->uXInputSlot  = uIndex;
		poDevice->bSynthMerged = ( 0 == uIndex );
		poDevice->uAxis        = 0;
		poDevice->uButtons     = FPADIO_MAX_INPUTS;

		fclib_strcpy( poDevice->oInfo.szName, szName );
		poDevice->oInfo.oeID    = FPADIO_INPUT_XB_GAMEPAD;
		poDevice->oInfo.uInputs = FPADIO_MAX_INPUTS;
		for( uInput = 0; uInput < FPADIO_MAX_INPUTS; ++uInput )
		{
			poDevice->oInfo.aeInputIDs[ uInput ] = (FPadio_InputID_e)( uInput + 1 );
		}

		_abXInputPrevConnected[ uIndex ] = FALSE;
	}
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

BOOL CALLBACK _EnumDevicesCallback( LPCDIDEVICEINSTANCE pDevInst, LPVOID pParam )
{
	u32 *puDeviceIndex = (u32 *)pParam;
	_Device_t *poTempDevice = &( _aoDevices[ *puDeviceIndex ] );

	if( FPADIO_MAX_DEVICES <= *puDeviceIndex )
	{
		return DIENUM_STOP;
	}

	if( FAILED( _pDInput->CreateDevice( pDevInst->guidInstance, &( poTempDevice->poDIDevice ), NULL ) ) )
	{
		DEVPRINTF( "[ FPADIO ] Error %u: CreateDevice() failed for device #%u (\"%s\") !!!\n", __LINE__, *puDeviceIndex, pDevInst->tszInstanceName );
		return DIENUM_CONTINUE;
	}

	poTempDevice->bEnumerated = TRUE;

#ifdef _UNICODE
	WideCharToMultiByte( CP_THREAD_ACP, WC_NO_BEST_FIT_CHARS, pDevInst->tszInstanceName, _tcslen( pDevInst->tszInstanceName ), poTempDevice->oInfo.szName, sizeof( poTempDevice->oInfo.szName ), NULL, NULL );
#else
	fclib_strcpy( poTempDevice->oInfo.szName, pDevInst->tszInstanceName );
#endif

	poTempDevice->oInfo.oeID = FPADIO_INPUT_DX_GAMEPAD;

	++(*puDeviceIndex);

	return DIENUM_CONTINUE;

} // _EnumDevicesCallback

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

BOOL CALLBACK _EnumAxesCallback( LPCDIDEVICEOBJECTINSTANCE pDevInst, LPVOID pParam )
{
	_Device_t *poTempDevice = &( _aoDevices[ ((_EnumAxesCallbackParam_t *)pParam)->uCurrentDeviceIndex ] );
	u32 *puAxisIndex = &( ((_EnumAxesCallbackParam_t *)pParam)->uAxisIndex );

	//// Set axis range.
	//
	DIPROPRANGE oPropRange;

	oPropRange.diph.dwSize       = sizeof( DIPROPRANGE );
	oPropRange.diph.dwHeaderSize = sizeof( DIPROPHEADER );
	oPropRange.diph.dwHow        = DIPH_BYID;
	oPropRange.diph.dwObj        = pDevInst->dwType;
	oPropRange.lMin              = - _AXIS_RANGE_MAX;
	oPropRange.lMax              = + _AXIS_RANGE_MAX;

	if( FAILED( poTempDevice->poDIDevice->SetProperty( DIPROP_RANGE, &oPropRange.diph ) ) )
	{
		DEVPRINTF( "[ FPADIO ] Error %u: SetProperty() failed for axis #%u of device #%u !!!\n", __LINE__, *puAxisIndex, ((_EnumAxesCallbackParam_t *)pParam)->uCurrentDeviceIndex );
		return DIENUM_CONTINUE;
	}
	//
	////

	//// Get axis offset in DIJOYSTATE2.
	//
	if( pDevInst->guidType == GUID_XAxis )
	{
		poTempDevice->auAxesOffset[ *puAxisIndex ] = (u8)( (u32)( FANG_OFFSETOF( DIJOYSTATE2, lX ) ) >> 2 );
	}
	else if( pDevInst->guidType == GUID_YAxis )
	{
		poTempDevice->auAxesOffset[ *puAxisIndex ] = (u8)( (u32)( FANG_OFFSETOF( DIJOYSTATE2, lY ) ) >> 2 );
	}
	else if( pDevInst->guidType == GUID_ZAxis )
	{
		poTempDevice->auAxesOffset[ *puAxisIndex ] = (u8)( (u32)( FANG_OFFSETOF( DIJOYSTATE2, lZ ) ) >> 2 );
	}
	else if( pDevInst->guidType == GUID_RxAxis )
	{
		poTempDevice->auAxesOffset[ *puAxisIndex ] = (u8)( (u32)( FANG_OFFSETOF( DIJOYSTATE2, lRx ) ) >> 2 );
	}
	else if( pDevInst->guidType == GUID_RyAxis )
	{
		poTempDevice->auAxesOffset[ *puAxisIndex ] = (u8)( (u32)( FANG_OFFSETOF( DIJOYSTATE2, lRy ) ) >> 2 );
	}
	else if( pDevInst->guidType == GUID_RzAxis )
	{
		poTempDevice->auAxesOffset[ *puAxisIndex ] = (u8)( (u32)( FANG_OFFSETOF( DIJOYSTATE2, lRz ) ) >> 2 );
	}
	else if( pDevInst->guidType == GUID_Slider )
	{
		u32 uCount = 0;

		for( ; uCount < *puAxisIndex; ++uCount )
		{
			if( poTempDevice->auAxesOffset[ uCount ] == (u8)( (u32)( FANG_OFFSETOF( DIJOYSTATE2, rglSlider ) ) >> 2 ) )
			{
				poTempDevice->auAxesOffset[ *puAxisIndex ] = 1 + (u8)( (u32)( FANG_OFFSETOF( DIJOYSTATE2, rglSlider ) ) >> 2 );
				break;
			}
		}

		if( uCount == *puAxisIndex )
		{
			poTempDevice->auAxesOffset[ *puAxisIndex ] = (u8)( (u32)( FANG_OFFSETOF( DIJOYSTATE2, rglSlider ) ) >> 2 );
		}
	}
else
{
FASSERT_NOW_MSG( "THIS IS VERY BAD!!" );
}
	//
	////

	++( *puAxisIndex );

	return DIENUM_CONTINUE;

} // _EnumAxesCallback

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

BOOL fpadio_ModuleStartup( void )
{
	FASSERT_MSG( ! _bModuleStarted, "[ FPADIO ] Error: System already started !!!" );

	_bModuleStarted = TRUE;
	_bModuleInstalled = FALSE;

	return TRUE;

} // fpadio_ModuleStartup

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void fpadio_ModuleShutdown( void )
{
	fpadio_Uninstall();

	_bModuleStarted = FALSE;

} // fpadio_ModuleShutdown

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

FPadio_Error_e fpadio_Install( const FPadio_Init_t *poInit /* = NULL */ )
{
	FASSERT_MSG( _bModuleStarted,            "[ FPADIO ] Error: System not started !!!" );
	FASSERT_MSG( ! _bModuleInstalled,        "[ FPADIO ] Error: System already installed !!!" );
	FASSERT_MSG( poInit,                     "[ FPADIO ] Error: NULL pointer !!!" );
	FASSERT_MSG( poInit->DX8ONLY_ohInstance, "[ FPADIO ] Error: NULL handle !!!" );
	FASSERT_MSG( poInit->DX8ONLY_ohWnd,      "[ FPADIO ] Error: NULL handle !!!" );
	FASSERT_MSG( poInit->DX8ONLY_uInputEmulationPlatform <= FPADIO_INPUT_EMULATION_PLATFORM_GC,
											 "[ FPADIO ] Error: Platform ID invalid !!!" );

	//// Clamping Fang_ConfigDefs values.
	//
	FMATH_CLAMP( Fang_ConfigDefs.nPadio_MaxSamplesPerSec, _SLOWEST_SAMPLING_RATE_SUPPORTED, _FASTEST_SAMPLING_RATE_SUPPORTED );

	_uMiliSecBetweenSamples = ( 1000 / Fang_ConfigDefs.nPadio_MaxSamplesPerSec );
	//
	////

	////
	//
	_hPollingThreadMutex = CreateMutex( NULL, TRUE, NULL );
	if( ! _hPollingThreadMutex )
	{
		DEVPRINTF( "[ FPADIO ] Error %u: CreateMutex() failed !!!\n", __LINE__ );
		return FPADIO_ERROR;
	}

	_hSamplesAndDevicesMutex = CreateMutex( NULL, FALSE, NULL );
	if( ! _hSamplesAndDevicesMutex )
	{
		DEVPRINTF( "[ FPADIO ] Error %u: CreateMutex() failed !!!\n", __LINE__ );
		CloseHandle( _hPollingThreadMutex );
		return FPADIO_ERROR;
	}
	//
	////

	u32 uIndex;

	//// Allocate buffers memory.
	//
	_uSamplesPerBuffer = ( Fang_ConfigDefs.nPadio_MaxSamplesPerSec / _SLOWEST_SAMPLING_RATE_SUPPORTED );
	if( ( _uSamplesPerBuffer * _SLOWEST_SAMPLING_RATE_SUPPORTED ) != Fang_ConfigDefs.nPadio_MaxSamplesPerSec )
	{
		++_uSamplesPerBuffer;
	}

	FResFrame_t oTempResFrame = fres_GetFrame();

	////
	//
	_papaoSamplesA = (FPadio_Sample_t **)fres_Alloc( sizeof( FPadio_Sample_t * ) * FPADIO_MAX_DEVICES );
	_papaoSamplesB = (FPadio_Sample_t **)fres_Alloc( sizeof( FPadio_Sample_t * ) * FPADIO_MAX_DEVICES );
	if( ! ( _papaoSamplesA && _papaoSamplesB ) )
	{
		DEVPRINTF( "[ FPADIO ] Error %u: Can't allocate memory for buffers !!!\n", __LINE__ );
		CloseHandle( _hPollingThreadMutex );
		CloseHandle( _hSamplesAndDevicesMutex );
		return FPADIO_ERROR;
	}

	for( uIndex = 0; uIndex < FPADIO_MAX_DEVICES; ++uIndex )
	{
		_papaoSamplesA[ uIndex ] = (FPadio_Sample_t *)fres_AllocAndZero( sizeof( FPadio_Sample_t ) * _uSamplesPerBuffer );
		_papaoSamplesB[ uIndex ] = (FPadio_Sample_t *)fres_AllocAndZero( sizeof( FPadio_Sample_t ) * _uSamplesPerBuffer );

		if( ! ( _papaoSamplesA[ uIndex ] && _papaoSamplesB[ uIndex ] ) )
		{
			DEVPRINTF( "[ FPADIO ] Error %u: Can't allocate memory for buffers !!!\n", __LINE__ );
			fres_ReleaseFrame( oTempResFrame );
			CloseHandle( _hPollingThreadMutex );
			CloseHandle( _hSamplesAndDevicesMutex );
			return FPADIO_ERROR;
		}
	}
	//
	////

	_papaoCurrentSamples = _papaoSamplesA;
	_uValidSamples = 0;
	//
	//// Allocate buffers memory.

	////
	//
	DWORD dwThreadID;
	_hPollingThread = CreateThread( NULL, 0, _fpadio_SamplingThread, NULL, CREATE_SUSPENDED, &dwThreadID );
	if( ! _hPollingThread )
	{
		DEVPRINTF( "[ FPADIO ] Error %u: CreateThread() failed !!!\n", __LINE__ );
		fres_ReleaseFrame( oTempResFrame );
		CloseHandle( _hPollingThreadMutex );
		CloseHandle( _hSamplesAndDevicesMutex );
		return FPADIO_ERROR;
	}
	SetThreadPriority( _hPollingThread, THREAD_PRIORITY_ABOVE_NORMAL );
	//
	////

	fang_MemZero( _aoDevices, sizeof( _aoDevices ) );
	fang_MemZero( _afMotorCoarse, sizeof( _afMotorCoarse ) );
	fang_MemZero( _afMotorFine,   sizeof( _afMotorFine ) );

	DWORD dwExitCode;

	////
	//
	if( FAILED( DirectInput8Create( (HINSTANCE)poInit->DX8ONLY_ohInstance, DIRECTINPUT_VERSION, IID_IDirectInput8, (VOID**)&_pDInput, NULL ) ) )
	{
		DEVPRINTF( "[ FPADIO ] Error %u: DirectInput8Create() failed !!!\n", __LINE__ );
		ReleaseMutex( _hPollingThreadMutex );
		ResumeThread( _hPollingThread );
		do
		{
			Sleep( 10 );
			GetExitCodeThread( _hPollingThread, &dwExitCode );

		} while( STILL_ACTIVE == dwExitCode );
		CloseHandle( _hPollingThreadMutex );
		CloseHandle( _hSamplesAndDevicesMutex );
		fres_ReleaseFrame( oTempResFrame );
		return FPADIO_ERROR;
	}
	//
	////

	////
	//
	//// Device population -- phase 3, decisions D4/D5.
	//
	// Prefer XInput whenever its DLL loads and a controller is already
	// attached: Xbox pads then get exact, config-free translation and the
	// same physical pad is never registered twice (DirectInput sees Wine's
	// XInput pads too). If nothing is attached yet but DirectInput also finds
	// nothing, take the XInput slots anyway so a pad plugged in after launch
	// comes up live (hotplug). DirectInput keeps its legacy role only for
	// non-XInput HID pads. In every mode slot 0 can merge the keyboard synth,
	// so kb+mouse is always a co-source of port 0 and never drives 1..3.
	//
	u32 uCount = 0;
	BOOL bXInputMode = _XInputLoad() && ( _XInputConnectedCount() > 0 );

	if( ! bXInputMode )
	{
		if( FAILED( _pDInput->EnumDevices( DI8DEVCLASS_GAMECTRL, _EnumDevicesCallback, &uCount, DIEDFL_ATTACHEDONLY ) ) )
		{
			DEVPRINTF( "[ FPADIO ] Error %u: EnumDevices() failed !!!\n", __LINE__ );
			ReleaseMutex( _hPollingThreadMutex );
			ResumeThread( _hPollingThread );
			do
			{
				Sleep( 10 );
				GetExitCodeThread( _hPollingThread, &dwExitCode );

			} while( STILL_ACTIVE == dwExitCode );
			CloseHandle( _hPollingThreadMutex );
			CloseHandle( _hSamplesAndDevicesMutex );
			FDX8_SAFE_RELEASE( _pDInput );
			fres_ReleaseFrame( oTempResFrame );
			return FPADIO_ERROR;
		}

		bXInputMode = ( ( 0 == uCount ) && ( NULL != _pfnXInputGetState ) );
	}

	if( bXInputMode )
	{
		//// XInput mode: four pad slots, keyboard merged into slot 0.
		//
		_RegisterXInputDevices();
		DEVPRINTF( "[ma] fpadio: xinput mode, %u controller(s) attached at install; slot 0 also merges the keyboard.\n", _XInputConnectedCount() );
	}
	else if( ! uCount )
	{
		//// No controller attached -- register a keyboard instead of failing.
		//
		// DirectInput found nothing. That is always the case under Wine and the
		// normal one on a PC, but the old code returned FPADIO_ERROR here, which
		// left fpadio uninstalled, fpad_UpdateSamples() returning 0 every frame
		// and Gamepad_aapSample permanently reading zeros -- with no keyboard
		// path of its own the game could not be controlled at all. Slot 0 gets a
		// synthetic device whose samples come from the keyboard, and install
		// carries on, so the rest of this function and all of fpad/gamepad run
		// exactly as they do with a real pad.
		//
		static const char _szSynthName[] = "Keyboard (synthetic)";
		_Device_t *poSynth = &( _aoDevices[ 0 ] );
		u32 uSynthInput;

		poSynth->bEnumerated = FALSE;  // Skips the DirectInput setup loop below.
		poSynth->bAcquired   = TRUE;   // The sampling thread fills it.
		poSynth->poDIDevice  = NULL;   // No DirectInput object to poll or release.
		poSynth->uAxis       = 0;
		poSynth->uButtons    = FPADIO_MAX_INPUTS;
		fang_MemCopy( poSynth->oInfo.szName, _szSynthName, sizeof( _szSynthName ) );
		poSynth->oInfo.oeID    = FPADIO_INPUT_XB_GAMEPAD;
		poSynth->oInfo.uInputs = FPADIO_MAX_INPUTS;
		for( uSynthInput = 0; uSynthInput < FPADIO_MAX_INPUTS; ++uSynthInput )
		{
			// IDs 1..19 are contiguous and cover everything both gamepad maps ask
			// for; 15..19 are read as the Xbox-specific buttons because
			// Gameloop_nPlatform is GAMELOOP_PLATFORM_XB on this build.
			poSynth->oInfo.aeInputIDs[ uSynthInput ] = (FPadio_InputID_e)( uSynthInput + 1 );
		}
		DEVPRINTF( "[ma] fpadio: no DirectInput controller, keyboard registered as device 0.\n" );
	}
	else
	{
		//// DirectInput pad(s) present: the keyboard still co-sources port 0
		//// (decision D5) so a player can swap kb<->pad live; slots 1..3 stay
		//// pad-only for local multiplayer.
		//
		_aoDevices[ 0 ].bSynthMerged = TRUE;
		DEVPRINTF( "[ma] fpadio: %u DirectInput controller(s); keyboard merges into port 0.\n", uCount );
	}
	//
	////

	u32 uTemp;

	////
	//
	HWND hWnd = (HWND)poInit->DX8ONLY_ohWnd;

	DIDEVCAPS oDevCaps;
	oDevCaps.dwSize = sizeof( DIDEVCAPS );
	_EnumAxesCallbackParam_t oEnumAxesCallbackParam;

	for( uIndex = 0; uIndex < FPADIO_MAX_DEVICES; ++uIndex )
	{
		if( _aoDevices[ uIndex ].bEnumerated )
		{
			//// Gamepad data format.
			//
			if( FAILED( _aoDevices[ uIndex ].poDIDevice->SetDataFormat( &c_dfDIJoystick2 ) ) )
			{
				DEVPRINTF( "[ FPADIO ] Error %u: SetDataFormat() failed for device #%u !!!\n", __LINE__, uIndex );
				FDX8_SAFE_RELEASE( _aoDevices[ uIndex ].poDIDevice );
				_aoDevices[ uIndex ].bEnumerated = FALSE;
				continue;
			}
			//
			////

			//// Cooperative level.
			//
			if( FAILED( _aoDevices[ uIndex ].poDIDevice->SetCooperativeLevel( hWnd, DISCL_EXCLUSIVE | DISCL_BACKGROUND ) ) )
			{
				DEVPRINTF( "[ FPADIO ] Error %u: SetCooperativeLevel() failed for device #%u !!!\n", __LINE__, uIndex );
				FDX8_SAFE_RELEASE( _aoDevices[ uIndex ].poDIDevice );
				_aoDevices[ uIndex ].bEnumerated = FALSE;
				continue;
			}
			//
			////

			//// Capabilities.
			//
			if( FAILED( _aoDevices[ uIndex ].poDIDevice->GetCapabilities( &oDevCaps ) ) )
			{
				DEVPRINTF( "[ FPADIO ] Error %u: GetCapabilities() failed for device #%u !!!\n", __LINE__, uIndex );
				FDX8_SAFE_RELEASE( _aoDevices[ uIndex ].poDIDevice );
				_aoDevices[ uIndex ].bEnumerated = FALSE;
				continue;
			}

			_aoDevices[ uIndex ].uAxis    = FMATH_MIN( oDevCaps.dwAxes, 6 ); // Use, at the most, the first 6 axes.
			_aoDevices[ uIndex ].uButtons = FMATH_MIN( oDevCaps.dwButtons, ( FPADIO_MAX_INPUTS - oDevCaps.dwAxes ) ); // Use, at the most, the first ( FPADIO_MAX_INPUTS - oDevCaps.dwAxes ) buttons.

			////
			//
			_aoDevices[ uIndex ].oInfo.uInputs = _aoDevices[ uIndex ].uAxis + _aoDevices[ uIndex ].uButtons;

			for( uTemp = 0; uTemp < _aoDevices[ uIndex ].uAxis; ++uTemp )
			{
				_aoDevices[ uIndex ].oInfo.aeInputIDs[ uTemp ] = FPADIO_INPUT_DX_AXIS;
			}

			for( ; uTemp < _aoDevices[ uIndex ].oInfo.uInputs; ++uTemp )
			{
				_aoDevices[ uIndex ].oInfo.aeInputIDs[ uTemp ] = FPADIO_INPUT_DX_BUTTON;
			}
			//
			////
			//
			//// Capabilities.

			oEnumAxesCallbackParam.uAxisIndex = 0;
			oEnumAxesCallbackParam.uCurrentDeviceIndex = uIndex;
			if( FAILED( _aoDevices[ uIndex ].poDIDevice->EnumObjects( _EnumAxesCallback, &oEnumAxesCallbackParam, DIDFT_AXIS ) ) )
			{
				DEVPRINTF( "[ FPADIO ] Error %u: EnumObjects() failed for device #%u !!!\n", __LINE__, uIndex );
				FDX8_SAFE_RELEASE( _aoDevices[ uIndex ].poDIDevice );
				_aoDevices[ uIndex ].bEnumerated = FALSE;
				continue;
			}

			//// Acquire.
			//
			if( FAILED( _aoDevices[ uIndex ].poDIDevice->Acquire() ) )
			{
				DEVPRINTF( "[ FPADIO ] Error %u: Acquire() failed for device #%u !!!\n", __LINE__, uIndex );
				FDX8_SAFE_RELEASE( _aoDevices[ uIndex ].poDIDevice );
				_aoDevices[ uIndex ].bEnumerated = FALSE;
				continue;
			}
			_aoDevices[ uIndex ].bAcquired = TRUE;
			//
			////
		}
	}
	//
	////

	////
	//
	for( uIndex = 0; uIndex < FPADIO_MAX_DEVICES; ++uIndex )
	{
		// poDIDevice and the device-name pointer are both required: the synthetic
		// keyboard device has neither, and main_win.cpp passes a NULL device name,
		// so without these guards strstr() is handed NULL and the keyboard device
		// would be treated as an emulatable DirectInput controller.
		if( ( _aoDevices[ uIndex ].bAcquired ) &&
			( _aoDevices[ uIndex ].poDIDevice ) &&
			( FPADIO_INPUT_EMULATION_PLATFORM_NONE != poInit->DX8ONLY_uInputEmulationPlatform ) &&
			( poInit->DX8ONLY_pszInputEmulationDevName ) &&
			( strstr( _aoDevices[ uIndex ].oInfo.szName, (char *)poInit->DX8ONLY_pszInputEmulationDevName ) ) )
		{
			FASSERT_MSG( poInit->DX8ONLY_pauInputEmulationMap,          "[ FPADIO ] Error: NULL pointer !!!" );
			FASSERT_MSG( poInit->DX8ONLY_pszInputEmulationDevName,      "[ FPADIO ] Error: NULL pointer !!!" );
			FASSERT_MSG( *( poInit->DX8ONLY_pszInputEmulationDevName ), "[ FPADIO ] Error: Zero length string !!!" );

			_auInputEmulationPlatform[ uIndex ] = poInit->DX8ONLY_uInputEmulationPlatform;

			if( FPADIO_INPUT_EMULATION_PLATFORM_XB == _auInputEmulationPlatform[ uIndex ] ) // XB
			{
				for( uTemp = 0; uTemp < 6; ++uTemp )
				{
					_auInputEmulationMap[ uTemp ] = poInit->DX8ONLY_pauInputEmulationMap[ uTemp + 24 ];
				}

				for( uTemp = 0; uTemp < 13; ++uTemp )
				{
					_auInputEmulationMap[ uTemp + 6 ] = poInit->DX8ONLY_pauInputEmulationMap[ uTemp ];
				}

				_aoDevices[ uIndex ].oInfo.uInputs = FPADIO_MAX_INPUTS;
			}
			else if( FPADIO_INPUT_EMULATION_PLATFORM_GC == _auInputEmulationPlatform[ uIndex ] ) // GC
			{
				for( uTemp = 0; uTemp < 6; ++uTemp )
				{
					_auInputEmulationMap[ uTemp ] = poInit->DX8ONLY_pauInputEmulationMap[ uTemp + 30 ];
				}

				for( uTemp = 0; uTemp < 11; ++uTemp )
				{
					_auInputEmulationMap[ uTemp + 6 ] = poInit->DX8ONLY_pauInputEmulationMap[ uTemp + 13 ];
				}

				_aoDevices[ uIndex ].oInfo.uInputs = ( FPADIO_MAX_INPUTS - 2 );
			}
			else
			{
				FASSERT_NOW_MSG( "[ FPADIO ] Error: Unsupported platform emulation specified !!!" );
			}
		}
		else
		{
			_auInputEmulationPlatform[ uIndex ] = FPADIO_INPUT_EMULATION_PLATFORM_NONE;
		}
	}
	//
	////

	_bModuleInstalled = TRUE;

	ResumeThread( _hPollingThread );

	return FPADIO_NO_ERROR;

} // fpadio_Install

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void fpadio_Uninstall( void )
{
	if( ! _bModuleInstalled )
	{
		return;
	}

	ReleaseMutex( _hPollingThreadMutex );

	DWORD dwExitCode;
	do
	{
		Sleep( 10 );
		GetExitCodeThread( _hPollingThread, &dwExitCode );

	} while( STILL_ACTIVE == dwExitCode );

	for( u32 uIndex = 0; uIndex < FPADIO_MAX_DEVICES; ++uIndex )
	{
		if( ( _aoDevices[ uIndex ].bAcquired ) && ( _aoDevices[ uIndex ].poDIDevice ) )
		{
			_aoDevices[ uIndex ].poDIDevice->Unacquire();
			FDX8_SAFE_RELEASE( _aoDevices[ uIndex ].poDIDevice );
		}
	}

	FDX8_SAFE_RELEASE( _pDInput );
	_XInputUnload();

	CloseHandle( _hPollingThreadMutex );
	CloseHandle( _hSamplesAndDevicesMutex );

	_bModuleInstalled = FALSE;

} // fpadio_Uninstall

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

BOOL fpadio_IsInstalled( void )
{
	return _bModuleInstalled;

} // fpadio_IsInstalled

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void fpadio_SetForcefeedback_XB( u32 uDeviceIndex, f32 fUnitVibeCoarse, f32 fUnitVibeFine )
{
	FASSERT_MSG( _bModuleInstalled,                 "[ FPADIO ] Error: System not installed !!!" );
	FASSERT_MSG( FPADIO_MAX_DEVICES > uDeviceIndex, "[ FPADIO ] Error: Invalid device index !!!" );

	FMATH_CLAMP( fUnitVibeCoarse, 0.0f, 1.0f );
	FMATH_CLAMP( fUnitVibeFine,   0.0f, 1.0f );

	// Called from fforce_Work() each frame (envelope math lives in
	// fxbforce.cpp, decision D6); fpadio_ApplyAllForcefeedbacks() ships the
	// values to the pads below.
	_afMotorCoarse[ uDeviceIndex ] = fUnitVibeCoarse;
	_afMotorFine[   uDeviceIndex ] = fUnitVibeFine;

} // fpadio_SetForcefeedback_XB

void fpadio_ApplyAllForcefeedbacks( void )
{
	FASSERT_MSG( _bModuleInstalled, "[ FPADIO ] Error: System not installed !!!" );

	if( ! _pfnXInputSetState )
	{
		return;
	}

	// Stop rumbling while the window is unfocused: XInput keeps running while
	// the game sits in the background, and a buzzing pad in another app is a
	// bug, not a feature.
	BOOL bFocus = fdx8vid_HaveFocus();
	_XInputVibration_t oVibration;
	_Device_t *poDevice;
	u32 uIndex;

	for( uIndex = 0; uIndex < FPADIO_MAX_DEVICES; ++uIndex )
	{
		poDevice = &( _aoDevices[ uIndex ] );
		if( ! ( poDevice->bAcquired && poDevice->bXInput ) )
		{
			continue;
		}

		oVibration.wLeftMotorSpeed  = (WORD)( ( bFocus ? _afMotorCoarse[ uIndex ] : 0.0f ) * 65535.0f );
		oVibration.wRightMotorSpeed = (WORD)( ( bFocus ? _afMotorFine[   uIndex ] : 0.0f ) * 65535.0f );

		// Disconnected slots return _XINPUT_ERROR_DEVICE_NOT_CONNECTED -- a
		// no-op, which also keeps kb-only players' pads silent.
		_pfnXInputSetState( poDevice->uXInputSlot, &oVibration );
	}

} // fpadio_ApplyAllForcefeedbacks

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void fpadio_GetSamples( u32 *puValidSamples, FPadio_Sample_t ***ppapaoSamples )
{
	FASSERT_MSG( _bModuleInstalled, "[ FPADIO ] Error: System not installed !!!" );
	FASSERT_MSG( puValidSamples,    "[ FPADIO ] Error: NULL pointer !!!" );
	FASSERT_MSG( ppapaoSamples,     "[ FPADIO ] Error: NULL pointer !!!" );

	// When fpadio_Install failed -- no input device enumerated -- the module is
	// not installed, _hSamplesAndDevicesMutex is NULL and the sample arrays do
	// not exist. The FASSERTs above only report. Without this guard the code
	// waits on a NULL handle and hands back NULL samples, which the caller then
	// dereferences. Report 'no samples' instead.
	if( !_bModuleInstalled || !_hSamplesAndDevicesMutex ) {
		*puValidSamples = 0;
		*ppapaoSamples = NULL;
		return;
	}

	WaitForSingleObject( _hSamplesAndDevicesMutex, INFINITE );

	*puValidSamples = _uValidSamples;
	_uValidSamples = 0;
	*ppapaoSamples = _papaoCurrentSamples;
	_papaoCurrentSamples = ( _papaoSamplesA == _papaoCurrentSamples ) ? _papaoSamplesB : _papaoSamplesA;

	ReleaseMutex( _hSamplesAndDevicesMutex );

} // fpadio_GetSamples

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void fpadio_GetDeviceInfo( u32 uDeviceIndex, FPadio_DeviceInfo_t *poDeviceInfo )
{
	FASSERT_MSG( _bModuleInstalled,                 "[ FPADIO ] Error: System not installed !!!" );
	FASSERT_MSG( FPADIO_MAX_DEVICES > uDeviceIndex, "[ FPADIO ] Error: Invalid device index !!!" );
	FASSERT_MSG( poDeviceInfo,                      "[ FPADIO ] Error: NULL pointer !!!" );

	WaitForSingleObject( _hSamplesAndDevicesMutex, INFINITE );

	fang_MemCopy( poDeviceInfo, &( _aoDevices[ uDeviceIndex ].oInfo ), sizeof( _aoDevices[ uDeviceIndex ].oInfo ) );

	ReleaseMutex( _hSamplesAndDevicesMutex );

} // fpadio_GetDeviceInfo

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

DWORD WINAPI _fpadio_SamplingThread( LPVOID pParam ) {
	_Device_t *poTempDevice;
	FPadio_Sample_t *poTempSample;
	FPadio_Sample_t oTempSynth;
	DIJOYSTATE2 oTempState;
	u32 uIndex, uTemp, uCount;
	f32 *pafInputValues, fX, fY;

	do {
		WaitForSingleObject( _hSamplesAndDevicesMutex, INFINITE );

		for( uIndex = 0; uIndex < FPADIO_MAX_DEVICES; ++uIndex ) {
			poTempSample = (FPadio_Sample_t *)&( _papaoCurrentSamples[ uIndex ][ _uValidSamples ] );
			poTempDevice = &( _aoDevices[ uIndex ] );
			
			if( poTempDevice->bAcquired ) {
				//// XInput controller (phase 3): poll the pad, then -- slot 0
				//// only -- merge the keyboard synth over it (decision D5).
				if( poTempDevice->bXInput ) {
					_XInputSample( poTempDevice->uXInputSlot, poTempSample );
					if( poTempDevice->bSynthMerged ) {
						_MergeKeyboardInto( poTempSample, &oTempSynth );
					}
					continue;
				}

				//// Synthetic keyboard device: there is no DirectInput object to
				//// poll, so read the keys straight into this sample instead.
				if( ! poTempDevice->poDIDevice ) {
					_KeyboardSyntheticSample( poTempSample );
					continue;
				}

				//// Sample.
				if( FAILED( poTempDevice->poDIDevice->Poll() ) ) {
					DEVPRINTF( "[ FPADIO ] Error %u: Poll() failed !!!\n", __LINE__ );
					poTempDevice->poDIDevice->Unacquire();
					FDX8_SAFE_RELEASE( poTempDevice->poDIDevice );
					poTempDevice->bAcquired = FALSE;
					poTempSample->bValid = FALSE;
					continue;
				}
				
				//// Get values.
				if( FAILED( poTempDevice->poDIDevice->GetDeviceState( sizeof( DIJOYSTATE2 ), &oTempState ) ) ) {
					DEVPRINTF( "[ FPADIO ] Error %u: GetDeviceState() failed !!!\n", __LINE__ );
					poTempDevice->poDIDevice->Unacquire();
					FDX8_SAFE_RELEASE( poTempDevice->poDIDevice );
					poTempDevice->bAcquired = FALSE;
					poTempSample->bValid = FALSE;
					continue;
				}
				
				poTempSample->bValid = TRUE;
				pafInputValues = poTempSample->afInputValues;


				/////////////
				// LEFT STICK
				/////////////

				// Left X
				if( _auInputEmulationMap[0] ) {
					fX = ( (f32)( ((s32 *)&oTempState)[ poTempDevice->auAxesOffset[ _auInputEmulationMap[0] - 1 ] ] ) * _OO_AXIS_RANGE_MAX );
					_ApplyDeadzone( fX );						
				} else {
					fX = 0.0f;
				}
				// Left Y
				if( _auInputEmulationMap[1] ) {
					fY = ( (f32)( ((s32 *)&oTempState)[ poTempDevice->auAxesOffset[ _auInputEmulationMap[1] - 1 ] ] ) * _OO_AXIS_RANGE_MAX );
					fY = -fY;												
					_ApplyDeadzone( fY );
				} else {
					fY = 0.0f;
				}
				// Fixup the left x,y axis
				_RenormalizeStick( fX, fY );
				// Record the values
				pafInputValues[ ( FPADIO_INPUT_STICK_LEFT_X - 1 ) ] = fX;
				pafInputValues[ ( FPADIO_INPUT_STICK_LEFT_Y - 1 ) ] = fY;


				///////////////
				// RIGHT STICK
				///////////////

				// Right X
				if( _auInputEmulationMap[2] ) {
					fX = ( (f32)( ((s32 *)&oTempState)[ poTempDevice->auAxesOffset[ _auInputEmulationMap[2] - 1 ] ] ) * _OO_AXIS_RANGE_MAX );
					_ApplyDeadzone( fX );						
				} else {
					fX = 0.0f;
				}
				// Right Y
				if( _auInputEmulationMap[3] ) {
					fY = ( (f32)( ((s32 *)&oTempState)[ poTempDevice->auAxesOffset[ _auInputEmulationMap[3] - 1 ] ] ) * _OO_AXIS_RANGE_MAX );
					fY = -fY;												
					_ApplyDeadzone( fY );
				} else {
					fY = 0.0f;
				}
				// Fixup the right x,y axis
				_RenormalizeStick( fX, fY );
				// Record the values
				pafInputValues[ ( FPADIO_INPUT_STICK_RIGHT_X - 1 ) ] = fX;
				pafInputValues[ ( FPADIO_INPUT_STICK_RIGHT_Y - 1 ) ] = fY;
			
				
				////////////
				// D PAD
				////////////
				
				// x
				if( _auInputEmulationMap[4] ) {
					fX = ( (f32)( ((s32 *)&oTempState)[ poTempDevice->auAxesOffset[ _auInputEmulationMap[4] - 1 ] ] ) * _OO_AXIS_RANGE_MAX );
					_ApplyDeadzone( fX );
					pafInputValues[ ( FPADIO_INPUT_DPAD_X - 1 ) ] = fX * _OO_ONE_MINUS_DEADZONE;
				} else {
					pafInputValues[ ( FPADIO_INPUT_DPAD_X - 1 ) ] = 0.0f;
				}
				// y
				if( _auInputEmulationMap[5] ) {
					fY = ( (f32)( ((s32 *)&oTempState)[ poTempDevice->auAxesOffset[ _auInputEmulationMap[5] - 1 ] ] ) * _OO_AXIS_RANGE_MAX );
					fY = -fY;
					_ApplyDeadzone( fY );
					pafInputValues[ ( FPADIO_INPUT_DPAD_Y - 1 ) ] = fY * _OO_ONE_MINUS_DEADZONE;
				}
				else
				{
					pafInputValues[ ( FPADIO_INPUT_DPAD_Y - 1 ) ] = 0.0f;
				}

				if( FPADIO_INPUT_EMULATION_PLATFORM_XB == _auInputEmulationPlatform[ uIndex ] ) {
					// XB

					// handle the buttons
					pafInputValues[ ( FPADIO_INPUT_RESET                  - 1 ) ] = _auInputEmulationMap[  6 ] ? ( ( oTempState.rgbButtons[ _auInputEmulationMap[  6 ] - 1 ] & 0x80 ) ? 1.0f : 0.0f ) : 0.0f;
					pafInputValues[ ( FPADIO_INPUT_START                  - 1 ) ] = _auInputEmulationMap[  7 ] ? ( ( oTempState.rgbButtons[ _auInputEmulationMap[  7 ] - 1 ] & 0x80 ) ? 1.0f : 0.0f ) : 0.0f;
					pafInputValues[ ( FPADIO_INPUT_XB_DBUTTON_BACK        - 1 ) ] = _auInputEmulationMap[  8 ] ? ( ( oTempState.rgbButtons[ _auInputEmulationMap[  8 ] - 1 ] & 0x80 ) ? 1.0f : 0.0f ) : 0.0f;
					pafInputValues[ ( FPADIO_INPUT_XB_ABUTTON_BLACK       - 1 ) ] = _auInputEmulationMap[  9 ] ? ( ( oTempState.rgbButtons[ _auInputEmulationMap[  9 ] - 1 ] & 0x80 ) ? 1.0f : 0.0f ) : 0.0f;
					pafInputValues[ ( FPADIO_INPUT_XB_ABUTTON_WHITE       - 1 ) ] = _auInputEmulationMap[ 10 ] ? ( ( oTempState.rgbButtons[ _auInputEmulationMap[ 10 ] - 1 ] & 0x80 ) ? 1.0f : 0.0f ) : 0.0f;
					pafInputValues[ ( FPADIO_INPUT_CROSS_BOTTOM           - 1 ) ] = _auInputEmulationMap[ 11 ] ? ( ( oTempState.rgbButtons[ _auInputEmulationMap[ 11 ] - 1 ] & 0x80 ) ? 1.0f : 0.0f ) : 0.0f;
					pafInputValues[ ( FPADIO_INPUT_CROSS_RIGHT            - 1 ) ] = _auInputEmulationMap[ 12 ] ? ( ( oTempState.rgbButtons[ _auInputEmulationMap[ 12 ] - 1 ] & 0x80 ) ? 1.0f : 0.0f ) : 0.0f;
					pafInputValues[ ( FPADIO_INPUT_CROSS_LEFT             - 1 ) ] = _auInputEmulationMap[ 13 ] ? ( ( oTempState.rgbButtons[ _auInputEmulationMap[ 13 ] - 1 ] & 0x80 ) ? 1.0f : 0.0f ) : 0.0f;
					pafInputValues[ ( FPADIO_INPUT_CROSS_TOP              - 1 ) ] = _auInputEmulationMap[ 14 ] ? ( ( oTempState.rgbButtons[ _auInputEmulationMap[ 14 ] - 1 ] & 0x80 ) ? 1.0f : 0.0f ) : 0.0f;
					pafInputValues[ ( FPADIO_INPUT_TRIGGER_LEFT           - 1 ) ] = _auInputEmulationMap[ 15 ] ? ( ( oTempState.rgbButtons[ _auInputEmulationMap[ 15 ] - 1 ] & 0x80 ) ? 1.0f : 0.0f ) : 0.0f;
					pafInputValues[ ( FPADIO_INPUT_TRIGGER_RIGHT          - 1 ) ] = _auInputEmulationMap[ 16 ] ? ( ( oTempState.rgbButtons[ _auInputEmulationMap[ 16 ] - 1 ] & 0x80 ) ? 1.0f : 0.0f ) : 0.0f;
					pafInputValues[ ( FPADIO_INPUT_XB_DBUTTON_STICK_LEFT  - 1 ) ] = _auInputEmulationMap[ 17 ] ? ( ( oTempState.rgbButtons[ _auInputEmulationMap[ 17 ] - 1 ] & 0x80 ) ? 1.0f : 0.0f ) : 0.0f;
					pafInputValues[ ( FPADIO_INPUT_XB_DBUTTON_STICK_RIGHT - 1 ) ] = _auInputEmulationMap[ 18 ] ? ( ( oTempState.rgbButtons[ _auInputEmulationMap[ 18 ] - 1 ] & 0x80 ) ? 1.0f : 0.0f ) : 0.0f;

				} else if( FPADIO_INPUT_EMULATION_PLATFORM_GC == _auInputEmulationPlatform[ uIndex ] ) {
					// GC

					// handle the buttons
					pafInputValues[ ( FPADIO_INPUT_RESET                    - 1 ) ] = _auInputEmulationMap[  6 ] ? ( ( oTempState.rgbButtons[ _auInputEmulationMap[  6 ] - 1 ] & 0x80 ) ? 1.0f : 0.0f ) : 0.0f;
					pafInputValues[ ( FPADIO_INPUT_START                    - 1 ) ] = _auInputEmulationMap[  7 ] ? ( ( oTempState.rgbButtons[ _auInputEmulationMap[  7 ] - 1 ] & 0x80 ) ? 1.0f : 0.0f ) : 0.0f;
					pafInputValues[ ( FPADIO_INPUT_CROSS_BOTTOM             - 1 ) ] = _auInputEmulationMap[  8 ] ? ( ( oTempState.rgbButtons[ _auInputEmulationMap[  8 ] - 1 ] & 0x80 ) ? 1.0f : 0.0f ) : 0.0f;
					pafInputValues[ ( FPADIO_INPUT_CROSS_LEFT               - 1 ) ] = _auInputEmulationMap[  9 ] ? ( ( oTempState.rgbButtons[ _auInputEmulationMap[  9 ] - 1 ] & 0x80 ) ? 1.0f : 0.0f ) : 0.0f;
					pafInputValues[ ( FPADIO_INPUT_CROSS_RIGHT              - 1 ) ] = _auInputEmulationMap[ 10 ] ? ( ( oTempState.rgbButtons[ _auInputEmulationMap[ 10 ] - 1 ] & 0x80 ) ? 1.0f : 0.0f ) : 0.0f;
					pafInputValues[ ( FPADIO_INPUT_CROSS_TOP                - 1 ) ] = _auInputEmulationMap[ 11 ] ? ( ( oTempState.rgbButtons[ _auInputEmulationMap[ 11 ] - 1 ] & 0x80 ) ? 1.0f : 0.0f ) : 0.0f;
					pafInputValues[ ( FPADIO_INPUT_GC_DBUTTON_TRIGGER_Z     - 1 ) ] = _auInputEmulationMap[ 12 ] ? ( ( oTempState.rgbButtons[ _auInputEmulationMap[ 12 ] - 1 ] & 0x80 ) ? 1.0f : 0.0f ) : 0.0f;
					pafInputValues[ ( FPADIO_INPUT_TRIGGER_LEFT             - 1 ) ] = _auInputEmulationMap[ 13 ] ? ( ( oTempState.rgbButtons[ _auInputEmulationMap[ 13 ] - 1 ] & 0x80 ) ? 1.0f : 0.0f ) : 0.0f;
					pafInputValues[ ( FPADIO_INPUT_TRIGGER_RIGHT            - 1 ) ] = _auInputEmulationMap[ 14 ] ? ( ( oTempState.rgbButtons[ _auInputEmulationMap[ 14 ] - 1 ] & 0x80 ) ? 1.0f : 0.0f ) : 0.0f;
					pafInputValues[ ( FPADIO_INPUT_GC_DBUTTON_TRIGGER_RIGHT - 1 ) ] = _auInputEmulationMap[ 15 ] ? ( ( oTempState.rgbButtons[ _auInputEmulationMap[ 15 ] - 1 ] & 0x80 ) ? 1.0f : 0.0f ) : 0.0f;
					pafInputValues[ ( FPADIO_INPUT_GC_DBUTTON_TRIGGER_LEFT  - 1 ) ] = _auInputEmulationMap[ 16 ] ? ( ( oTempState.rgbButtons[ _auInputEmulationMap[ 16 ] - 1 ] & 0x80 ) ? 1.0f : 0.0f ) : 0.0f;

				} else {
					// UNKNOWN PLATFORM

					//// Axes.
					for( uTemp = 0; uTemp < poTempDevice->uAxis; ++uTemp ) {
						pafInputValues[ uTemp ] = ( (f32)( ((s32 *)&oTempState)[ poTempDevice->auAxesOffset[ uTemp ] ] ) * _OO_AXIS_RANGE_MAX );
					}
					
					uCount = uTemp;

					//// Buttons.
					for( uTemp = 0; uTemp < poTempDevice->uButtons; ++uTemp ) {
						pafInputValues[ uCount + uTemp ] = ( oTempState.rgbButtons[ uTemp ] & 0x80 ) ? 1.0f : 0.0f;
					}
				}

				//// Legacy DirectInput pad co-sourcing port 0 with the keyboard
				//// (decision D5) -- the same merge XInput slot 0 uses.
				if( poTempDevice->bSynthMerged ) {
					_MergeKeyboardInto( poTempSample, &oTempSynth );
				}
			} else {
				poTempSample->bValid = FALSE;
			}
		}

		if( _uValidSamples < ( _uSamplesPerBuffer - 1 ) ) {
			++_uValidSamples;
		}

		ReleaseMutex( _hSamplesAndDevicesMutex );

	} while( WAIT_TIMEOUT == WaitForSingleObject( _hPollingThreadMutex, _uMiliSecBetweenSamples ) );

	return 0; // no error.

} // _fpadio_SamplingThread

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -


