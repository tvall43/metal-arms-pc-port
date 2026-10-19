// main_win.cpp - Win32 entry point for the PC (fang2) build of Metal Arms.
//
// The retail PC build used MFC: App/ma/win/ma_win.cpp declared a CWinApp whose
// InitInstance() created CMa_winDlg, a modal dialog that collected the master
// file, video mode and a pile of debug toggles, then called gameloop_Start()
// from its Run button. mingw has no MFC, so this replaces that shell.
//
// What it keeps:
//   - the same fang_Init() / Fang_ConfigDefs / fang_Startup() sequence,
//   - the same gameloop_Start() parameters,
//   - settings persistence through the existing CSettings,
//   - the same video-mode defaults the engine would have picked.
//
// What it drops:
//   - the settings/asset-picker dialogs themselves (CMa_winDlg, CPickAsset,
//     CVidMode, CInputEmulation are all CDialog subclasses),
//   - the debug-only toggles, which only mattered to the developer's shell.
//
// The game itself is untouched: the engine registers its own window class in
// fdx8vid.cpp and creates the rendering window itself.

#include "fang.h"
#include "ffile.h"
#include "fvid.h"

#include "../gameloop.h"
#include "../gamepad.h"	// Gamepad_fMouseSensitivity
#include "faudio.h"		// FAudio_f2DVolumeScale, FAudio_fMinDistanceFraction
#include "fpadio.h"		// fpadio_SetSynthKeyOverrides, FPADIO_INPUT_*
#include "Settings.h"
#include "floop.h"
#include "ffile.h"

#include <windows.h>
#include <tlhelp32.h>
#include <commdlg.h>
#include <shlobj.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

// Matches the retail PC defaults (App/ma/win/ma_winDlg.cpp).
// The retail Xbox main hardcodes "d:\\MOVIES\\". Derive it from the master
// file instead, so an install on any other drive still finds its movies.
static char _szMoviePath[512] = "MOVIES\\";

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------
//
// The engine routes every DEVPRINTF through Fang_ConfigDefs.pFang_FcnPrintf.
// Leaving that NULL, as this file originally did, throws all of it away, so a
// failure during startup produced no output at all.
//
// Destinations, split by urgency (2026-10-09): stdout carries only urgent
// lines -- startup status, the crash dump, and engine lines that look like
// errors (_FangPrintf). Debug volume goes to ma_win_log.txt beside the
// executable instead: each line to an unredirected Wine console is a console
// round-trip, which is what made loads crawl.
//
// The file is gated on the Debug/LogFile ini flag, read before the launch
// dialog and re-applied after it, so the dialog's "Debug log file" checkbox
// wins for the session. Default OFF: a normal launch writes nothing, and the
// engine's output is then the error-looking lines on stdout only. Tick the
// box for a run you intend to read. The crash handler flushes whichever is
// open.
static FILE *_pLogFile = NULL;
//R69: bump this every round; it is printed in the log header (see the banner).
#define MA_PORT_ROUND	"R69"

static BOOL _bLogFileEnabled = FALSE;

//R62: defined in ma_winDlg.cpp (next to the _DevPrintf choke point).
extern BOOL FWin_bTEMPDebugProbes;
//R68: separate diagnostic for the wire/light-pass question (see fdx8mesh.cpp).
extern BOOL FWin_bTEMPDebugLightForce;
#define _LOGFILE_DEFAULT 0

// PC port: development mode (-dev / autorun) vs retail mode (default / play).
// Retail hides the launch dialog's dev rows, keeps the debug log off no matter
// what the ini says, and sets Gameloop_bRetailMode before the game starts.
static BOOL _bDevMode = FALSE;


static void _TraceV( const char *pszFormat, va_list Args )
{
	char szBuf[2048];
	vsnprintf( szBuf, sizeof(szBuf), pszFormat, Args );
	printf( "%s", szBuf );
	fflush( stdout );
	if ( _pLogFile ) { fputs( szBuf, _pLogFile ); fflush( _pLogFile ); }
}

static void _Trace( const char *pszFormat, ... )
{
	char szBuf[2048];
	va_list args;
	va_start( args, pszFormat );
	vsnprintf( szBuf, sizeof(szBuf), pszFormat, args );
	va_end( args );

	printf( "%s", szBuf );
	fflush( stdout );
	if ( _pLogFile ) {
		fputs( szBuf, _pLogFile );
		fflush( _pLogFile );
	}
}

// <exe dir>\ma_win_log.txt -- beside the executable, like every other file
// this port writes, so nothing lands in the Windows directory.
static void _LogFileBuildPath( char *pszOut, int nOutLen )
{
	pszOut[0] = '\0';
	char szExe[MAX_PATH] = "";
	DWORD nLen = GetModuleFileName( NULL, szExe, MAX_PATH );
	if ( nLen == 0 || nLen >= MAX_PATH ) return;

	memcpy( pszOut, szExe, nLen + 1 );
	char *pSlash = strrchr( pszOut, '\\' );
	if ( pSlash ) { *(pSlash+1) = '\0'; } else { pszOut[0] = '\0'; }
	strncat( pszOut, "ma_win_log.txt", nOutLen - strlen(pszOut) - 1 );
}

// Open or close the file log so it matches _bLogFileEnabled. Split out of
// _OpenLog because the launch dialog changes the flag after WinMain's first
// call: tick the box and the file opens for that session, untick and it closes.
static void _ApplyLogFileState( void )
{
	if( _bLogFileEnabled && !_pLogFile ) {
		char szLog[MAX_PATH];
		_LogFileBuildPath( szLog, MAX_PATH );
		if( szLog[0] ) {
			_pLogFile = fopen( szLog, "w" );
			if( _pLogFile ) {
				_Trace( "[ma] log file    : %s\n", szLog );
			}
		}
	}
	else if( !_bLogFileEnabled && _pLogFile ) {
		fclose( _pLogFile );
		_pLogFile = NULL;
	}
}

static void _OpenLog( void )
{
	// First lines of the run. These used to print before _LoadConfig ran (and
	// before the file could open), which is why _OpenLog moved after the config
	// read in WinMain.
	_ApplyLogFileState();

	char szExe[MAX_PATH] = "";
	if( GetModuleFileName( NULL, szExe, MAX_PATH ) ) {
		_Trace( "[ma] WinMain, exe = %s\n", szExe );
	}
	_Trace( "[ma] audio mix   : Volume2D=%.3f (%.1f dB) MinDistanceFraction=%.3f\n",
		FAudio_f2DVolumeScale,
		20.0f * (float)log10( (double)( FAudio_f2DVolumeScale > 0.0001f ? FAudio_f2DVolumeScale : 0.0001f ) ),
		FAudio_fMinDistanceFraction );
}

// Also used by compat/mfc.cpp to report the game thread's lifecycle, which has
// to happen before and independently of the engine's own logging.
extern "C" void ma_win_Trace( const char *pszFormat, ... )
{
	va_list args;
	va_start( args, pszFormat );
	_TraceV( pszFormat, args );
	va_end( args );
}

// The engine's own logging: same signature as FangFcnPrintf_t. Debug-level --
// goes to the file log only, never to stdout, except lines that look like real
// errors: those reach stdout even with the file log off, so a broken run still
// says why it broke.
static void _FangPrintf( const char *pszFormat, FANG_VA_LIST Args )
{
	char szBuf[2048];
	vsnprintf( szBuf, sizeof(szBuf), pszFormat, Args );

	if( _pLogFile ) {
		fputs( szBuf, _pLogFile );
		fflush( _pLogFile );
	}

	if( strstr( szBuf, "Error" ) || strstr( szBuf, "error" ) || strstr( szBuf, "ERROR" )
		|| strstr( szBuf, "failed" ) || strstr( szBuf, "Failed" ) || strstr( szBuf, "FAILED" )
		|| strstr( szBuf, "Could not" ) || strstr( szBuf, "could not" ) ) {
		printf( "%s", szBuf );
		fflush( stdout );
	}
}

// ---------------------------------------------------------------------------
// Crash logging
// ---------------------------------------------------------------------------
//
// A vectored exception handler installed before anything else runs, so a crash
// is recorded with its faulting address even when Windows Error Reporting
// swallows the process into a dialog. The address is what matters: ma_win.exe is
// built with DWARF, so it can be resolved to a source line afterwards.
//
// This deliberately logs only. Handling the fault would turn a hard crash into
// undefined behaviour.
static LONG WINAPI _CrashLogger( EXCEPTION_POINTERS *pEP )
{
	if( !pEP || !pEP->ExceptionRecord ) return EXCEPTION_CONTINUE_SEARCH;

	const EXCEPTION_RECORD *pRec = pEP->ExceptionRecord;

	// 0x40010005 and 0x40010006 are the OutputDebugString family (the ANSI and
	// Unicode variants), raised inside kernel32/kernelbase when nothing is
	// attached to read the string. They are not faults -- execution continues --
	// and they arrive in bursts, including from the console Ctrl-C handler during
	// shutdown, so logging them only buries a real one.
	if( pRec->ExceptionCode == 0x40010005u || pRec->ExceptionCode == 0x40010006u ) {
		return EXCEPTION_CONTINUE_SEARCH;
	}

	_Trace( "\n[ma] *** CRASH ***\n" );
	_Trace( "[ma] exception code : 0x%08lX\n", (unsigned long)pRec->ExceptionCode );
	_Trace( "[ma] faulting addr  : 0x%08lX\n", (unsigned long)pRec->ExceptionAddress );
	// ExceptionInformation is not a register dump: on an AV it holds only
	// [0]=read/write and [1]=target address, and the slots past that are stale
	// garbage.  The real register state lives in ContextRecord.
	const CONTEXT *pCtx = pEP->ContextRecord;
	if( pCtx )
	{
		_Trace( "[ma] EIP=%08lX ESP=%08lX EBP=%08lX\n",
			(unsigned long)pCtx->Eip, (unsigned long)pCtx->Esp, (unsigned long)pCtx->Ebp );
		_Trace( "[ma] EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX ESI=%08lX EDI=%08lX\n",
			(unsigned long)pCtx->Eax, (unsigned long)pCtx->Ebx,
			(unsigned long)pCtx->Ecx, (unsigned long)pCtx->Edx,
			(unsigned long)pCtx->Esi, (unsigned long)pCtx->Edi );
	}

	if( pRec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && pRec->NumberParameters >= 2 )
	{
		_Trace( "[ma] access %s   target 0x%08lX\n",
			pRec->ExceptionInformation[0] ? "WRITE" : "READ",
			(unsigned long)pRec->ExceptionInformation[1] );
	}

	// Name the module the address belongs to, so a fault inside a DLL is
	// distinguishable from one in our own code.
	HMODULE hMod = NULL;
	if( GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
	                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                        (LPCTSTR)pRec->ExceptionAddress, &hMod ) )
	{
		char szMod[MAX_PATH] = "";
		if( GetModuleFileNameA( hMod, szMod, MAX_PATH ) )
		{
			_Trace( "[ma] in module      : %s\n", szMod );
		}
		_Trace( "[ma] module base    : 0x%08lX  (offset 0x%08lX)\n",
			(unsigned long)(ULONG_PTR)hMod,
			(unsigned long)( (ULONG_PTR)pRec->ExceptionAddress - (ULONG_PTR)hMod ) );
	}
	// Return addresses of the faulting call chain.  This runs on the faulting
	// thread, so the range from ESP upward is already-used, committed stack.
	if( pCtx )
	{
		const unsigned int *pStack = (const unsigned int *)(ULONG_PTR)pCtx->Esp;
		int i;
		_Trace( "[ma] stack:" );
		for( i = 0; i < 24; i++ ) _Trace( " %08X", pStack[i] );
		_Trace( "\n" );
	}

	// Module bases, so stack words and the fault address can be attributed to
	// a module offline (GetModuleHandleEx above only covers the fault address).
	HANDLE hSnap = CreateToolhelp32Snapshot( TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId() );
	if( hSnap != INVALID_HANDLE_VALUE )
	{
		MODULEENTRY32 Me;
		int nMods = 0;
		memset( &Me, 0, sizeof( Me ) );
		Me.dwSize = sizeof( Me );
		if( Module32First( hSnap, &Me ) )
		{
			do
			{
				_Trace( "[ma] module %08lX %s\n", (unsigned long)(ULONG_PTR)Me.modBaseAddr, Me.szModule );
			} while( ++nMods < 96 && Module32Next( hSnap, &Me ) );
		}
		CloseHandle( hSnap );
	}

	fflush( stdout );
	if( _pLogFile ) fflush( _pLogFile );
	return EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------------------
// Global startup configuration, filled by the dialog and read by _RunGame.
// ---------------------------------------------------------------------------

static char _szMasterFile[512]	= "";
static char _szGameRootPath[512]	= "";  // dir of master file, trailing slash; loose .wvs live here
static char _szLevelFile[256]	= "";
static char _szScreenshotDir[512]	= "";
static u32  _nScreenWidth			= 640;
static u32  _nScreenHeight		= 480;
static u32  _nColorBits			= 32;
static BOOL _bFullscreen		= FALSE;

// Control identifiers.
#define _ID_MASTEREDIT		1000
#define _ID_BROWSE			1001
#define _ID_LEVELEDIT		1002
#define _ID_SSHOTEDIT		1003
#define _ID_WIDTH			1004
#define _ID_HEIGHT			1005
#define _ID_COLORBITS		1006
#define _ID_START			1007
#define _ID_CANCEL			1008
#define _ID_BROWSE_SSHOT	1010
#define _ID_STATUS			1009
#define _ID_FULLSCREEN		1011
#define _ID_DEBUGLOG		1012

static HWND _hwndStatus = NULL;

static void _SetStatus( const char *pszText )
{
	if( _hwndStatus ) {
		SetWindowText( _hwndStatus, pszText );
	}
}

// ---------------------------------------------------------------------------
// Persistence

// [KEYMAP] key-name parsing (phase 2): a [KEYMAP] value is a single key
// name -- a letter or digit, "0xNN" for a raw virtual-key code, or one of the
// names below. Returns 0 for anything unrecognized.
static u32 _ParseKeyName( const char *pszName )
{
	static const struct { const char *psz; u32 vk; } _aNames[] = {
		{ "Space",     VK_SPACE },   { "Tab",       VK_TAB },
		{ "Return",    VK_RETURN },  { "Enter",     VK_RETURN },
		{ "Escape",    VK_ESCAPE },  { "Esc",       VK_ESCAPE },
		{ "Backspace", VK_BACK },    { "LShift",    VK_LSHIFT },
		{ "RShift",    VK_RSHIFT },  { "LCtrl",     VK_LCONTROL },
		{ "RCtrl",     VK_RCONTROL },{ "LAlt",      VK_LMENU },
		{ "RAlt",      VK_RMENU },   { "Up",        VK_UP },
		{ "Down",      VK_DOWN },    { "Left",      VK_LEFT },
		{ "Right",     VK_RIGHT },   { "LButton",   VK_LBUTTON },
		{ "RButton",   VK_RBUTTON }, { "MButton",   VK_MBUTTON },
		{ "Insert",    VK_INSERT },  { "Delete",    VK_DELETE },
		{ "Home",      VK_HOME },    { "End",       VK_END },
		{ "PageUp",    VK_PRIOR },   { "PageDown",  VK_NEXT },
		{ "CapsLock",  VK_CAPITAL }, { "Comma",     VK_OEM_COMMA },
		{ "Period",    VK_OEM_PERIOD },{ "Slash",    VK_OEM_2 },
		{ "Semicolon", VK_OEM_1 },   { "Quote",     VK_OEM_7 },
		{ "LBracket",  VK_OEM_4 },   { "RBracket",  VK_OEM_6 },
		{ "Minus",     VK_OEM_MINUS },{ "Equals",    VK_OEM_PLUS },
		{ "Grave",     VK_OEM_3 },   { "Backslash", VK_OEM_5 },
		{ "F1",  VK_F1 },  { "F2",  VK_F2 },  { "F3",  VK_F3 },  { "F4",  VK_F4 },
		{ "F5",  VK_F5 },  { "F6",  VK_F6 },  { "F7",  VK_F7 },  { "F8",  VK_F8 },
		{ "F9",  VK_F9 },  { "F10", VK_F10 }, { "F11", VK_F11 }, { "F12", VK_F12 },
	};
	u32 i;

	// Single letter or digit: the character itself (upper-cased).
	if( ( 1 == strlen( pszName ) ) &&
		( ( ( pszName[0] >= 'a' ) && ( pszName[0] <= 'z' ) ) || ( ( pszName[0] >= 'A' ) && ( pszName[0] <= 'Z' ) ) ||
		  ( ( pszName[0] >= '0' ) && ( pszName[0] <= '9' ) ) ) )
	{
		char c = pszName[0];
		if( ( c >= 'a' ) && ( c <= 'z' ) ) { c -= 32; }
		return (u32)(unsigned char)c;
	}

	// Explicit virtual-key code.
	if( ( '0' == pszName[0] ) && ( ( 'x' == pszName[1] ) || ( 'X' == pszName[1] ) ) ) {
		return (u32)strtoul( pszName + 2, NULL, 16 );
	}

	for( i = 0; i < sizeof( _aNames ) / sizeof( _aNames[0] ); ++i ) {
		if( 0 == _stricmp( pszName, _aNames[i].psz ) ) {
			return _aNames[i].vk;
		}
	}
	return 0;
}
// ---------------------------------------------------------------------------

static void _LoadConfig( void )
{
	CSettings& oSettings = CSettings::GetCurrent();

	oSettings.ReadCustomString( "Paths", "MasterFile", "", _szMasterFile, sizeof(_szMasterFile) );
	oSettings.ReadCustomString( "Paths", "Level", "", _szLevelFile, sizeof(_szLevelFile) );
	oSettings.ReadCustomString( "Paths", "ScreenshotDir", ".", _szScreenshotDir, sizeof(_szScreenshotDir) );
	_nScreenWidth  = oSettings.ReadCustomInt( "Video", "Width",  640 );
	_nScreenHeight = oSettings.ReadCustomInt( "Video", "Height", 480 );
	_nColorBits    = oSettings.ReadCustomInt( "Video", "ColorBits", 32 );
	_bFullscreen   = oSettings.ReadCustomInt( "Video", "Fullscreen", 0 ) ? TRUE : FALSE;
	_bLogFileEnabled = oSettings.ReadCustomInt( "Debug", "LogFile", _LOGFILE_DEFAULT ) ? TRUE : FALSE;
	//R62: the R45-R61 invisibility-hunt probes are opt-in (see ma_winDlg.cpp).
	FWin_bTEMPDebugProbes = oSettings.ReadCustomInt( "Debug", "TempProbes", 0 ) ? TRUE : FALSE;
	//R68: forces only the diffuse (light) pass of the watched meshes to flat white.
	//Independent of TempProbes on purpose: that one also enables the pin force,
	//which paints the very meshes this diagnostic is meant to observe.
	FWin_bTEMPDebugLightForce = oSettings.ReadCustomInt( "Debug", "WireLightForce", 0 ) ? TRUE : FALSE;

	// Retail mode disables debug: the file log stays off regardless of the ini.
	// The stored value survives for dev runs - _SaveConfig only writes it there.
	if( !_bDevMode ) {
		_bLogFileEnabled = FALSE;
	}


	// [Audio] mix tuning. Whether a mix sounds right is judged by ear, not read
	// off the code, so both knobs that decide the 2D-vs-3D balance are editable
	// here rather than baked into fdx8audio -- see faudio.h for what each one
	// means. _SaveConfig writes them back so they show up in the file.
	{
		f32 f2D  = oSettings.ReadCustomFloat( "Audio", "Volume2D", FAUDIO_DEFAULT_2D_VOLUME_SCALE );
		f32 fMin = oSettings.ReadCustomFloat( "Audio", "MinDistanceFraction", FAUDIO_DEFAULT_MIN_DISTANCE_FRACTION );
		// Clamp: 0 silences every 2D sound, a fat-fingered 10 would blast.
		if( f2D  < 0.0f )  { f2D  = 0.0f; }
		if( f2D  > 1.0f )  { f2D  = 1.0f; }
		if( fMin < 0.05f ) { fMin = 0.05f; }
		if( fMin > 1.0f )  { fMin = 1.0f; }
		FAudio_f2DVolumeScale       = f2D;
		FAudio_fMinDistanceFraction = fMin;
	}

	// Mouse look sensitivity (phase 1): scales only the mouse-look deltas in
	// CPlayer::Work, not the sticks. Clamp to a sane band so a typo cannot
	// freeze or rocket the view.
	Gamepad_fMouseSensitivity = oSettings.ReadCustomFloat( "Mouse", "Sensitivity", 1.0f );
	if( Gamepad_fMouseSensitivity < 0.1f ) {
		Gamepad_fMouseSensitivity = 0.1f;
	} else if( Gamepad_fMouseSensitivity > 10.0f ) {
		Gamepad_fMouseSensitivity = 10.0f;
	}

	// [KEYMAP] button overrides (phase 2): optional per-action bindings for
	// the keyboard synth. Absent entry = default binding (see fdx8padio.cpp;
	// documented in HANDOFF.md). Buttons only; WASD / IJKL / arrows are fixed.
	{
		static const struct { const char *pszName; int nInputID; } _aKeyActions[] = {
			{ "Jump",           FPADIO_INPUT_CROSS_BOTTOM },
			{ "SelectPrimary",  FPADIO_INPUT_CROSS_RIGHT },
			{ "SelectSecondary",FPADIO_INPUT_CROSS_LEFT },
			{ "Action",         FPADIO_INPUT_CROSS_TOP },
			{ "Start",          FPADIO_INPUT_START },
			{ "Back",           FPADIO_INPUT_XB_DBUTTON_BACK },
			{ "Fire",           FPADIO_INPUT_TRIGGER_RIGHT },
			{ "Aim",            FPADIO_INPUT_TRIGGER_LEFT },
			{ "Black",          FPADIO_INPUT_XB_ABUTTON_BLACK },
			{ "White",          FPADIO_INPUT_XB_ABUTTON_WHITE },
			{ "Crouch",         FPADIO_INPUT_XB_DBUTTON_STICK_LEFT },
			{ "Melee",          FPADIO_INPUT_XB_DBUTTON_STICK_RIGHT },
		};
		u32 auKeys[ FPADIO_MAX_INPUTS ];
		u32 i;

		fang_MemZero( auKeys, sizeof( auKeys ) );
		for( i = 0; i < sizeof( _aKeyActions ) / sizeof( _aKeyActions[0] ); ++i ) {
			char szKey[64];
			oSettings.ReadCustomString( "KEYMAP", _aKeyActions[i].pszName, "", szKey, sizeof( szKey ) );
			if( szKey[0] ) {
				u32 uVK = _ParseKeyName( szKey );
				if( uVK ) {
					auKeys[ _aKeyActions[i].nInputID - 1 ] = uVK;
				} else {
					DEVPRINTF( "[ma] [KEYMAP] %s=%s: unknown key name; using the default binding.\n",
						_aKeyActions[i].pszName, szKey );
				}
			}
		}
		fpadio_SetSynthKeyOverrides( auKeys );
	}
}

static void _SaveConfig( void )
{
	CSettings& oSettings = CSettings::GetCurrent();

	oSettings.WriteCustomString( "Paths", "MasterFile", _szMasterFile );
	oSettings.WriteCustomString( "Paths", "Level", _szLevelFile );
	oSettings.WriteCustomString( "Paths", "ScreenshotDir", _szScreenshotDir );
	oSettings.WriteCustomInt( "Video", "Width", _nScreenWidth );
	oSettings.WriteCustomInt( "Video", "Height", _nScreenHeight );
	oSettings.WriteCustomInt( "Video", "ColorBits", _nColorBits );
	oSettings.WriteCustomInt( "Video", "Fullscreen", _bFullscreen ? 1 : 0 );
	if( _bDevMode ) {
		oSettings.WriteCustomInt( "Debug", "LogFile", _bLogFileEnabled ? 1 : 0 );
		oSettings.WriteCustomInt( "Debug", "TempProbes", FWin_bTEMPDebugProbes ? 1 : 0 );
		oSettings.WriteCustomInt( "Debug", "WireLightForce", FWin_bTEMPDebugLightForce ? 1 : 0 );
	}
	oSettings.WriteCustomFloat( "Audio", "Volume2D", FAudio_f2DVolumeScale );
	oSettings.WriteCustomFloat( "Audio", "MinDistanceFraction", FAudio_fMinDistanceFraction );
}

// ---------------------------------------------------------------------------
// Video modes
// ---------------------------------------------------------------------------

// A modest list of modes the engine is known to handle. The retail build
// enumerated the hardware through CVidMode; that is a CDialog and so not
// available here, so offer a fixed list instead. The engine can also run
// windowed, which is what happens when no explicit mode is chosen.
struct _ModeEntry_t {
	u32 nWidth;
	u32 nHeight;
};

static const _ModeEntry_t _aModes[] = {
	{ 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1280, 960 },
	{ 1280, 1024 }, { 1600, 1200 }
};
static const u32 _cModes = sizeof(_aModes) / sizeof(_aModes[0]);

// Returns the index of the stored mode, or -1.
static s32 _FindMode( u32 nWidth, u32 nHeight )
{
	for( u32 i = 0; i < _cModes; i++ ) {
		if( _aModes[i].nWidth == nWidth && _aModes[i].nHeight == nHeight ) {
			return (s32)i;
		}
	}
	return -1;
}

// ---------------------------------------------------------------------------
// Dialog
// ---------------------------------------------------------------------------

static HINSTANCE _hInstance = NULL;

static void _AddLabel( HWND hwndParent, int nX, int nY, int nW, int nH, const char *pszText )
{
	HWND hwnd = CreateWindowEx( 0, "STATIC", pszText,
		WS_CHILD | WS_VISIBLE | SS_LEFT,
		nX, nY, nW, nH, hwndParent, NULL, _hInstance, NULL );
	if( hwnd ) {
		SetWindowPos( hwnd, HWND_TOP, nX, nY, nW, nH, SWP_NOZORDER | SWP_NOACTIVATE );
	}
}

static void _GetCtrlText( HWND hwndDlg, int nID, char *pszDest, int nDestLen )
{
	HWND hwnd = GetDlgItem( hwndDlg, nID );
	if( hwnd ) {
		GetWindowText( hwnd, pszDest, nDestLen );
	}
}

static void _OnBrowseMasterFile( HWND hwndDlg )
{
	char szFile[512] = "";

	OPENFILENAME ofn;
	memset( &ofn, 0, sizeof(ofn) );
	ofn.lStructSize		= sizeof(ofn);
	ofn.hwndOwner		= hwndDlg;
	ofn.lpstrFilter		= "Master Files (*.mst)\0*.mst\0All Files (*.*)\0*.*\0\0";
	ofn.lpstrFile		= szFile;
	ofn.nMaxFile		= sizeof(szFile);
	ofn.lpstrTitle		= "Select the Metal Arms master file";
	ofn.Flags			= OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;

	if( GetOpenFileName( &ofn ) ) {
		SetDlgItemText( hwndDlg, _ID_MASTEREDIT, szFile );
	}
}

static void _OnBrowseScreenshotDir( HWND hwndDlg )
{
	char szDir[512] = "";

	BROWSEINFOA bi;
	memset( &bi, 0, sizeof(bi) );
	bi.hwndOwner	= hwndDlg;
	bi.pszDisplayName= szDir;
	bi.lpszTitle	= "Select the screenshot directory";
	bi.ulFlags		= BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

	LPITEMIDLIST pidl = SHBrowseForFolderA( &bi );
	if( pidl ) {
		SHGetPathFromIDListA( pidl, szDir );
		CoTaskMemFree( pidl );
		SetDlgItemText( hwndDlg, _ID_SSHOTEDIT, szDir );
	}
}

// Pull the current control state back into the globals.
static void _ReadControls( HWND hwndDlg )
{
	char szTmp[64] = "";

	_GetCtrlText( hwndDlg, _ID_MASTEREDIT, _szMasterFile, sizeof(_szMasterFile) );
	_GetCtrlText( hwndDlg, _ID_LEVELEDIT, _szLevelFile, sizeof(_szLevelFile) );
	_GetCtrlText( hwndDlg, _ID_SSHOTEDIT, _szScreenshotDir, sizeof(_szScreenshotDir) );

	// The resolution and colour depth are combo boxes, so read the selection
	// rather than the text.
	HWND hwndWidth = GetDlgItem( hwndDlg, _ID_WIDTH );
	if( hwndWidth ) {
		const LRESULT nSel = SendMessage( hwndWidth, CB_GETCURSEL, 0, 0 );
		if( nSel != CB_ERR && (u32)nSel < _cModes ) {
			_nScreenWidth  = _aModes[nSel].nWidth;
			_nScreenHeight = _aModes[nSel].nHeight;
		}
	}

	HWND hwndBits = GetDlgItem( hwndDlg, _ID_COLORBITS );
	if( hwndBits ) {
		const LRESULT nSel = SendMessage( hwndBits, CB_GETCURSEL, 0, 0 );
		static const u32 aBits[] = { 32, 24, 16 };
		if( nSel != CB_ERR && nSel >= 0 && nSel < 3 ) {
			_nColorBits = aBits[nSel];
		}
	}

	HWND hwndFS = GetDlgItem( hwndDlg, _ID_FULLSCREEN );
	if( hwndFS ) {
		_bFullscreen = ( SendMessage( hwndFS, BM_GETCHECK, 0, 0 ) == BST_CHECKED );
	}

	HWND hwndLog = GetDlgItem( hwndDlg, _ID_DEBUGLOG );
	if( hwndLog ) {
		_bLogFileEnabled = ( SendMessage( hwndLog, BM_GETCHECK, 0, 0 ) == BST_CHECKED );
	}
}

static BOOL _OnStart( HWND hwndDlg )
{
	_ReadControls( hwndDlg );

	if( _szMasterFile[0] == '\0' ) {
		_SetStatus( "Select the master file (.mst) before starting." );
		MessageBox( hwndDlg, "Select the Metal Arms master file (.mst) first.",
			"Metal Arms", MB_OK | MB_ICONWARNING );
		return FALSE;
	}

	// The master file holds the asset container; fang_Startup() reads it.
	if( GetFileAttributesA( _szMasterFile ) == INVALID_FILE_ATTRIBUTES ) {
		char szMsg[640];
		snprintf( szMsg, sizeof(szMsg), "Cannot open master file:\n%s", _szMasterFile );
		_SetStatus( szMsg );
		MessageBox( hwndDlg, szMsg, "Metal Arms", MB_OK | MB_ICONERROR );
		return FALSE;
	}

	_SaveConfig();
	PostMessage( hwndDlg, WM_CLOSE, 0, 0 );
	return TRUE;
}

static LRESULT CALLBACK _DlgProc( HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam )
{
	switch( uMsg ) {
	case WM_COMMAND:
		switch( LOWORD( wParam ) ) {
		case _ID_BROWSE:		_OnBrowseMasterFile( hwndDlg ); return 0;
		case _ID_BROWSE_SSHOT:	_OnBrowseScreenshotDir( hwndDlg ); return 0;
		case _ID_START:		if( _OnStart( hwndDlg ) ) { DestroyWindow( hwndDlg ); } return 0;
		case _ID_CANCEL:		DestroyWindow( hwndDlg ); return 0;
		case IDCANCEL:			DestroyWindow( hwndDlg ); return 0;
		case IDOK:				if( _OnStart( hwndDlg ) ) { DestroyWindow( hwndDlg ); } return 0;
		}
		return 0;

	case WM_CLOSE:
		DestroyWindow( hwndDlg );
		return 0;

	case WM_DESTROY:
		PostQuitMessage( 0 );
		return 0;
	}
	return DefWindowProcA( hwndDlg, uMsg, wParam, lParam );
}

static void _AddCombo( HWND hwndDlg, int nID, int nX, int nY, int nW )
{
	// The height here is the height of the dropped list; the control's own
	// height is set below. Leaving it at 200 pushed every following control
	// 176px too far down, which put the Start and Cancel buttons outside a
	// window that was not tall enough to contain them.
	HWND hwnd = CreateWindowEx( 0, "COMBOBOX", "",
		WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
		nX, nY, nW, 200, hwndDlg, (HMENU)(INT_PTR)nID, _hInstance, NULL );
	if( hwnd ) {
		SetWindowPos( hwnd, HWND_TOP, nX, nY, nW, 24, SWP_NOZORDER | SWP_NOACTIVATE );
		for( u32 i = 0; i < _cModes; i++ ) {
			char szText[32];
			snprintf( szText, sizeof(szText), "%u x %u", _aModes[i].nWidth, _aModes[i].nHeight );
			SendMessage( hwnd, CB_ADDSTRING, 0, (LPARAM)szText );
		}
		const s32 nMode = _FindMode( _nScreenWidth, _nScreenHeight );
		SendMessage( hwnd, CB_SETCURSEL, ( nMode >= 0 ? nMode : 0 ), 0 );
	}
}

static void _AddButton( HWND hwndDlg, int nID, const char *pszText, int nX, int nY, int nW, int nH )
{
	HWND hwnd = CreateWindowEx( 0, "BUTTON", pszText,
		WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
		nX, nY, nW, nH, hwndDlg, (HMENU)(INT_PTR)nID, _hInstance, NULL );
	if( hwnd ) {
		SetWindowPos( hwnd, HWND_TOP, nX, nY, nW, nH, SWP_NOZORDER | SWP_NOACTIVATE );
	}
}

static void _AddEdit( HWND hwndDlg, int nID, const char *pszText, int nX, int nY, int nW )
{
	HWND hwnd = CreateWindowEx( 0, "EDIT", pszText,
		WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
		nX, nY, nW, 24, hwndDlg, (HMENU)(INT_PTR)nID, _hInstance, NULL );
	if( hwnd ) {
		SetWindowPos( hwnd, HWND_TOP, nX, nY, nW, 24, SWP_NOZORDER | SWP_NOACTIVATE );
		SendMessage( hwnd, EM_SETSEL, 0, -1 );
	}
}

static void _CreateDialogControls( HWND hwndDlg )
{
	const int nX = 12, nLabelW = 96, nCtrlX = 112, nCtrlW = 250;
	int nY = 12;

	_AddLabel( hwndDlg, nX, nY + 4, nLabelW, 20, "Master file:" );
	_AddEdit( hwndDlg, _ID_MASTEREDIT, _szMasterFile, nCtrlX, nY, nCtrlW );
	_AddButton( hwndDlg, _ID_BROWSE, "Browse...", nCtrlX + nCtrlW + 8, nY, 80, 24 );
	nY += 32;

	// Dev rows (with the Debug log checkbox below): only in development mode
	// (-dev / autorun). Retail boots clean; _ReadControls keeps the stored
	// values for the hidden controls.
	if( _bDevMode ) {
		_AddLabel( hwndDlg, nX, nY + 4, nLabelW, 20, "Level / file:" );
		_AddEdit( hwndDlg, _ID_LEVELEDIT, _szLevelFile, nCtrlX, nY, nCtrlW );
		nY += 32;
	}

	_AddLabel( hwndDlg, nX, nY + 4, nLabelW, 20, "Screenshots:" );
	_AddEdit( hwndDlg, _ID_SSHOTEDIT, _szScreenshotDir, nCtrlX, nY, nCtrlW );
	_AddButton( hwndDlg, _ID_BROWSE_SSHOT, "Browse...", nCtrlX + nCtrlW + 8, nY, 80, 24 );
	nY += 32;

	_AddLabel( hwndDlg, nX, nY + 4, nLabelW, 20, "Resolution:" );
	_AddCombo( hwndDlg, _ID_WIDTH, nCtrlX, nY, 120 );
	{
		const int nFSX = nCtrlX + 136;
		HWND hwnd = CreateWindowEx( 0, "BUTTON", "Fullscreen",
			WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
			nFSX, nY + 2, 120, 20, hwndDlg, (HMENU)(INT_PTR)_ID_FULLSCREEN, _hInstance, NULL );
		if( hwnd ) {
			SendMessage( hwnd, BM_SETCHECK, _bFullscreen ? BST_CHECKED : BST_UNCHECKED, 0 );
		}
	}
	nY += 32;

	_AddLabel( hwndDlg, nX, nY + 4, nLabelW, 20, "Colour depth:" );
	{
		HWND hwnd = CreateWindowEx( 0, "COMBOBOX", "",
			WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
			nCtrlX, nY, 100, 120, hwndDlg, (HMENU)(INT_PTR)_ID_COLORBITS, _hInstance, NULL );
		SetWindowPos( hwnd, HWND_TOP, nCtrlX, nY, 100, 24, SWP_NOZORDER | SWP_NOACTIVATE );
		static const u32 aBits[] = { 32, 24, 16 };
		for( int i = 0; i < 3; i++ ) {
			char szText[16];
			snprintf( szText, sizeof(szText), "%u-bit", aBits[i] );
			SendMessage( hwnd, CB_ADDSTRING, 0, (LPARAM)szText );
			if( aBits[i] == _nColorBits ) {
				SendMessage( hwnd, CB_SETCURSEL, i, 0 );
			}
		}
	}
	if( _bDevMode ) {
		// Debug log: the file beside the exe (see _LOGFILE_DEFAULT). The saved
		// Debug/LogFile value seeds it; the choice is re-applied right after the
		// dialog closes, so ticking it here turns logging on for this session.
		HWND hwnd = CreateWindowEx( 0, "BUTTON", "Debug log file",
			WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
			nCtrlX + 112, nY + 2, 130, 20, hwndDlg, (HMENU)(INT_PTR)_ID_DEBUGLOG, _hInstance, NULL );
		if( hwnd ) {
			SendMessage( hwnd, BM_SETCHECK, _bLogFileEnabled ? BST_CHECKED : BST_UNCHECKED, 0 );
		}
	}
	nY += 32;

	_AddLabel( hwndDlg, nX, nY, 420, 20, "" );
	_hwndStatus = CreateWindowEx( 0, "STATIC", "",
		WS_CHILD | WS_VISIBLE | SS_LEFT,
		nX, nY, 420, 20, hwndDlg, (HMENU)(INT_PTR)_ID_STATUS, _hInstance, NULL );
	nY += 28;

	_AddButton( hwndDlg, _ID_START, "Start",  nX + 250, nY, 80, 26 );
	_AddButton( hwndDlg, _ID_CANCEL, "Cancel", nX + 340, nY, 80, 26 );
}

// Returns TRUE if the user asked to start the game.
static BOOL _ShowStartupDialog( void )
{
	WNDCLASSA wc;
	memset( &wc, 0, sizeof(wc) );
	wc.lpfnWndProc		= _DlgProc;
	wc.hInstance		= _hInstance;
	wc.hCursor			= LoadCursorA( NULL, IDC_ARROW );
	wc.hbrBackground	= (HBRUSH)(COLOR_BTNFACE + 1);
	wc.lpszClassName	= "MetalArmsStartup";
	if( !RegisterClassA( &wc ) ) {
		return FALSE;
	}

	HWND hwndDlg = CreateWindowExA(
		WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
		"MetalArmsStartup", "Metal Arms",
		WS_POPUPWINDOW | WS_CAPTION | WS_VISIBLE,
		CW_USEDEFAULT, CW_USEDEFAULT, 470, 250,
		NULL, NULL, _hInstance, NULL );
	if( !hwndDlg ) {
		return FALSE;
	}
	_CreateDialogControls( hwndDlg );

	BOOL bRun = FALSE;
	MSG msg;
	while( GetMessageA( &msg, NULL, 0, 0 ) > 0 ) {
		TranslateMessage( &msg );
		DispatchMessageA( &msg );
		if( msg.message == WM_QUIT ) {
			break;
		}
	}

	// WM_QUIT is posted on destroy, so "quit" means either the user cancelled or
	// they pressed Start. Start removes the master-file requirement and closes
	// the window; a cancel leaves _szMasterFile possibly set, so use an explicit
	// flag written by _OnStart instead.
	bRun = (_szMasterFile[0] != '\0');
	return bRun;
}

// ---------------------------------------------------------------------------
// Game startup
// ---------------------------------------------------------------------------

static void _GameloopExitFunction( void )
{
	gameloop_End();
}

static void _GameloopMinimizeFunction( void )
{
	// Retail passed NULL here; nothing to do.
}

static BOOL _RunGame( void )
{
	{
		// Derive the game root (directory of the master file, trailing slash)
		// for loose assets that sit beside the .mst on disc - notably the
		// streaming .wvs files. Xbox set this from the DVD root; the Windows
		// port never assigned it, so CFAudioStream::Create() strcpy'd a NULL
		// source and crashed on the first title-screen music Create.
		strncpy( _szGameRootPath, _szMasterFile, sizeof(_szGameRootPath) - 1 );
		_szGameRootPath[sizeof(_szGameRootPath) - 1] = '\0';
		{
			char *pLast = strrchr( _szGameRootPath, '\\' );
			char *pFwd  = strrchr( _szGameRootPath, '/' );
			if ( pFwd && ( !pLast || pFwd > pLast ) ) pLast = pFwd;
			if ( pLast ) {
				*(pLast+1) = '\0';
			} else {
				_szGameRootPath[0] = '\0';
			}
		}
		_Trace( "[ma] game root   : %s\n", _szGameRootPath );

		char szDir[512] = "";
		strncpy( szDir, _szMasterFile, sizeof(szDir) - 1 );
		char *pSlash = strrchr( szDir, '\\' );
		if ( pSlash ) {
			*(pSlash+1) = '\0';
			strncat( szDir, "MOVIES\\", sizeof(szDir) - strlen(szDir) - 1 );
		} else {
			strncpy( szDir, "MOVIES\\", sizeof(szDir) - 1 );
		}
		strncpy( _szMoviePath, szDir, sizeof(_szMoviePath) - 1 );
	}

	//R69: the log now identifies the build and the state of the diagnostic switches.
	//Two rides were spent on "is the new exe actually running" and "did the ini key get
	//read"; this answers both in the first three lines.
	_Trace( "[ma] port build  : %s\n", MA_PORT_ROUND );
	_Trace( "[ma] diagnostics : TempProbes=%d WireLightForce=%d\n",
		FWin_bTEMPDebugProbes ? 1 : 0, FWin_bTEMPDebugLightForce ? 1 : 0 );
	_Trace( "[ma] ---- startup ----\n" );
	_Trace( "[ma] master file : %s\n", _szMasterFile );
	_Trace( "[ma] level       : %s\n", _szLevelFile[0] ? _szLevelFile : "(none)" );
	_Trace( "[ma] video       : %ux%u %u-bit %s\n", _nScreenWidth, _nScreenHeight, _nColorBits,
		_bFullscreen ? "fullscreen" : "windowed" );

	fang_Init();
	_Trace( "[ma] fang_Init done\n" );

	// Same configuration the retail PC dialog used
	// (App/ma/win/ma_winDlg.cpp, CMa_winDlg::OnInitDialog / OnButtonRun).
	Fang_ConfigDefs.nRes_HeapBytes			= (64) * 1024 * 1024;
	Fang_ConfigDefs.pszFile_MasterFilePathName	= _szMasterFile;
	// Must be after fang_Init(): it NULLs every config pointer on entry.
	Fang_ConfigDefs.pszFile_GameRootPathName	= _szGameRootPath;
	Fang_ConfigDefs.pszMovie_BasePathName		= _szMoviePath;
	Fang_ConfigDefs.pFang_FcnPrintf			= _FangPrintf;
	Fang_ConfigDefs.nAudio_MaxSoundBytes		= (1024 * (36 * 1024));
	Fang_ConfigDefs.nAMem_FastAuxiliaryMemoryBytes	= 4048000;
	Fang_ConfigDefs.bCheckPoint_StartupSystem	= TRUE;
	Fang_ConfigDefs.nText_MaxCharsPerFrame		= 550;
	Fang_ConfigDefs.nText_MaxCharsPerPrintf	= 190;
	Fang_ConfigDefs.nMaxParticleEmitters		= 500;
	Fang_ConfigDefs.nMaxParticles				= 3000;
	Fang_ConfigDefs.nMaxParticleEmitterSprites	= 1000;

	if( !fang_Startup() ) {
		_Trace( "[ma] fang_Startup FAILED\n" );
		MessageBoxA( NULL, "The Fang engine failed to start.\n\n"
						   "See ma_win_log.txt next to the executable for details.",
						"Metal Arms", MB_OK | MB_ICONERROR );
		return FALSE;
	}
	_Trace( "[ma] fang_Startup ok\n" );

	// Name the asset log once the file module is up. ffile_LogStart() only sets
	// _bLogSystemOK if it can open this file, and ffile_LogStop() asserts on that
	// flag -- gameloop.cpp:641 calls LogStart and :826/:839 call LogStop. With no
	// filename set the open fails and the flag stays false.
	//
	// This has to happen after fang_Startup(), not before it: ffile_OSDir_Open()
	// uses the file module's cookie table and its _bModuleInitialized flag, and
	// neither exists until the module has started. The retail dialog did the same thing (ma_winDlg.cpp:431), appending
	// "_asset_log.txt" to the executable's own path.
	{
		char szLogPath[MAX_PATH] = "";
		if( GetModuleFileNameA( NULL, szLogPath, MAX_PATH ) ) {
			char *pDot = strrchr( szLogPath, '.' );
			if( pDot ) *pDot = '\0';
			strncat( szLogPath, "_asset_log.txt", MAX_PATH - strlen(szLogPath) - 1 );
			_Trace( "[ma] asset log    : %s\n", szLogPath );
			if( !ffile_LogSetFilename( szLogPath ) ) {
				_Trace( "[ma] could not open the asset log; logging will be disabled\n" );
			}
		}
	}



	// Re-read the master file in case it was rebuilt since launch.
	Fang_ConfigDefs.pszFile_MasterFilePathName	= _szMasterFile;
	Fang_ConfigDefs.nAMem_FastAuxiliaryMemoryBytes	= 4048000;
	ffile_SetupFileMode();

	GameloopInitParm_t GameloopParms;
	memset( &GameloopParms, 0, sizeof(GameloopParms) );

	GameloopParms.fTargetFPS				= GAMELOOP_DEFAULT_TARGET_FPS;
	GameloopParms.bSkipLevelSelect		= FALSE;
	GameloopParms.bViewBounds			= FALSE;
	GameloopParms.bShowFPS				= FALSE;
	GameloopParms.nAnimPlaybackRate		= gameloop_GetAnimPlaybackRate();
	GameloopParms.bDrawScreenSafeArea	= FALSE;
	GameloopParms.nMaxSoundMgrSounds		= GAMELOOP_DEFAULT_MAX_SOUNDS;
	GameloopParms.bPlayerDeath			= TRUE;
	GameloopParms.bDebugAI				= FALSE;
	GameloopParms.nPlatform				= GAMELOOP_PLATFORM_GC;
	GameloopParms.pszInputFilename		= _szLevelFile;
	GameloopParms.pszScreenShotDir		= _szScreenshotDir;
	GameloopParms.BGColorRGB				= NULL;
	GameloopParms.pExitFunc				= _GameloopExitFunction;
	GameloopParms.pMinFunc				= _GameloopMinimizeFunction;
	GameloopParms.nMemCardUsageFlags		= GAMELOOP_MEMCARD_NONE;
	GameloopParms.pszMemCardDir			= "";
	GameloopParms.pauInputEmulationMap	= NULL;
	GameloopParms.pszInputEmulationDevName	= NULL;
	GameloopParms.bInstallAudio			= TRUE;
	GameloopParms.nLegServoLevel			= 0;
	GameloopParms.nArmServoLevel			= 0;
	GameloopParms.bShowEntityInfo		= FALSE;
	GameloopParms.bDemoLaunched			= FALSE;
	GameloopParms.uTimeoutInterval		= 0;

	// Video window: the engine creates the window itself from this description.
	FVidWin_t* pVidWin = &GameloopParms.VidWin;
	memset( pVidWin, 0, sizeof(*pVidWin) );
	pVidWin->VidDev.nFlags		= FVID_DEVFLAG_NONE;
	pVidWin->VidDev.nOrdinal		= 0;
	pVidWin->VidDev.nRenderer	= FVID_RENDERER_HARDWARE;
	strcpy( pVidWin->VidDev.szName, "Default" );

	// Windowed unless the launch dialog's Fullscreen box is ticked (Video/Fullscreen
	// in ma_win.ini). Fullscreen = FVID_MODEFLAG_NONE, which is what the retail
	// mode list uses for exclusive modes (fdx8vid.cpp tags only windowed entries
	// with FVID_MODEFLAG_WINDOWED). This was hardwired windowed while the port
	// still had no visible output and was never retested.
	pVidWin->VidMode.nFlags		= _bFullscreen ? FVID_MODEFLAG_NONE : FVID_MODEFLAG_WINDOWED;
	pVidWin->VidMode.nColorBits	= (u8)_nColorBits;
	pVidWin->VidMode.nDepthBits	= 24;
	pVidWin->VidMode.nStencilBits	= 8;
	pVidWin->VidMode.nPixelsAcross	= (u16)_nScreenWidth;
	pVidWin->VidMode.nPixelsDown	= (u16)_nScreenHeight;

	pVidWin->nSwapInterval		= 1;
	pVidWin->fUnitFSAA			= 0.0f;
	pVidWin->hInstance			= _hInstance;
	pVidWin->hWnd				= NULL;
	pVidWin->nIconIDI			= 0;
	pVidWin->bAllowPowerSuspend	= TRUE;
	pVidWin->pFcnSuspend		= NULL;
	strcpy( pVidWin->szWindowTitle, "Mettle Arms" );

	// PC port: retail mode unless -dev/autorun asked for the development shell.
	// Sets Gameloop_bRetailMode + Fang_bDevOverlays before the game thread
	// starts: no dev cheats, no debug overlays, no debug pad holds.
	gameloop_EnterRetailMode( _bDevMode ? FALSE : TRUE );
	_Trace( "[ma] mode       : %s\n", _bDevMode ? "development (-dev)" : "retail" );


	_Trace( "[ma] calling gameloop_Start...\n" );
	const BOOL bStarted = gameloop_Start( &GameloopParms );
	_Trace( "[ma] gameloop_Start returned %d\n", bStarted );

	if( bStarted ) {
		// On Windows the gameloop does not run here. The Windows build of
		// floop_InstallGameloop only installs it -- it spawns a CGameThread and
		// returns TRUE immediately (the while(TRUE) version at fdx8loop.cpp:825
		// is the #else, the console path). The main thread's job is to pump
		// messages for the render window, which is what MFC's CWinApp::Run used
		// to do. Returning from WinMain without doing so tore the process down,
		// and the game thread with it, which is why the game appeared to do
		// nothing at all.
		//
		// Do NOT pump messages here.
		//
		// The Windows floop runs the gameloop on CGameThread, and that thread has
		// its own message loop (fdx8loop.cpp:577):
		//
		//     while( TRUE ) {
		//         if( PeekMessage( &Msg, NULL, 0, 0, PM_NOREMOVE ) ) {
		//             if( !PumpMessage() ) break;      // WM_QUIT
		//         } else {
		//             bCallGameloop = _ExecOneLoop();   // <-- the actual frame
		//         }
		//     }
		//
		// The gameloop only advances when the queue is *empty*. Draining it from
		// the main thread means it is never empty when the game thread looks, so
		// _ExecOneLoop() is never called and not a single frame is drawn -- which
		// is exactly the symptom: the window is created and shown, then nothing
		// renders and the process winds down.
		//
		// The main thread's only job is to stay alive until the gameloop stops.
		_Trace( "[ma] waiting for the gameloop thread\n" );
		while( floop_IsGameloopRunning() ) {
			Sleep( 10 );
		}
		_Trace( "[ma] gameloop no longer running, returning from WinMain\n" );
		return TRUE;
	}

	if( !bStarted ) {
		MessageBoxA( NULL, "The game loop failed to start.\n\n"
						   "See ma_win_log.txt next to the executable for details.",
						"Metal Arms", MB_OK | MB_ICONERROR );
	}
	return bStarted;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int WINAPI WinMain( HINSTANCE hInstance, HINSTANCE hPrevInstance,
					PSTR pszCmdLine, int nCmdShow )
{
	(void)hPrevInstance;
	(void)nCmdShow;

	_hInstance = hInstance;

	AddVectoredExceptionHandler( 1, _CrashLogger );

	CoInitialize( NULL );

	CSettings::SetSettingsFilename( "ma_win.ini" );
	CSettings::SetApplicationName( "ma_win" );

	// Command line, case-insensitive tokens; a leading '-' or '/' is ignored:
	//   autorun  boot straight in, development mode (what it always did)
	//   dev      development mode: boot-mode chooser, dev menu, dev cheats,
	//            and the launch dialog's Level jump + Debug log checkbox
	//   play     boot straight in, retail mode
	//   (none)   retail-mode launch dialog: dev rows hidden, debug log off
	BOOL bAutoRun = FALSE;
	if( pszCmdLine ) {
		for( const char* p = pszCmdLine; *p; p++ ) {
			if( *p == 'a' || *p == 'A' ) {
				const char* q = p;
				while( *q == 'a' || *q == 'A' ) { q++; }
				if( strncmp( q, "utorun", 6 ) == 0 ) {
					bAutoRun = TRUE;
					_bDevMode = TRUE;
				}
			}
		}
		for( const char* p = pszCmdLine; *p; ) {
			while( *p == ' ' || *p == '\t' ) { p++; }
			const char* pTok = p;
			while( *p && *p != ' ' && *p != '\t' ) { p++; }
			int nTok = (int)( p - pTok );
			while( nTok > 0 && ( *pTok == '-' || *pTok == '/' ) ) { pTok++; nTok--; }
			if( nTok == 3
				&& ( pTok[0] | 0x20 ) == 'd' && ( pTok[1] | 0x20 ) == 'e' && ( pTok[2] | 0x20 ) == 'v' ) {
				_bDevMode = TRUE;
			} else if( nTok == 4
				&& ( pTok[0] | 0x20 ) == 'p' && ( pTok[1] | 0x20 ) == 'l'
				&& ( pTok[2] | 0x20 ) == 'a' && ( pTok[3] | 0x20 ) == 'y' ) {
				bAutoRun = TRUE;
				_bDevMode = FALSE;
			}
		}
	}

	// Reads Debug/LogFile; retail forces it off inside (_bDevMode already set).
	_LoadConfig();

	// After _LoadConfig: gated on the Debug/LogFile flag. Also emits the first
	// trace lines, so the crash handler must already be installed above.
	_OpenLog();

	if( bAutoRun && _szMasterFile[0] != '\0' ) {
		if( !_RunGame() ) {
			CoUninitialize();
			return 1;
		}
		CoUninitialize();
		return 0;
	}

	if( !_ShowStartupDialog() ) {
		CoUninitialize();
		return 0;
	}

	// The launch dialog can change Debug/LogFile; _OpenLog above ran before it.
	_ApplyLogFileState();

	if( !_RunGame() ) {
		CoUninitialize();
		return 1;
	}

	CoUninitialize();
	return 0;
}
