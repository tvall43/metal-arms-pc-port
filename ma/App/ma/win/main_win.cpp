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
#include "Settings.h"
#include "floop.h"

#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <stdio.h>
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
// failure during startup produced no output at all. Send it to stdout and to a
// log file beside the executable, which survives being launched from a file
// manager where stdout is lost.
static FILE *_pLogFile = NULL;

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

static void _OpenLog( void )
{
	char szExe[MAX_PATH] = "";
	DWORD nLen = GetModuleFileName( NULL, szExe, MAX_PATH );
	if ( nLen == 0 || nLen >= MAX_PATH ) return;

	char szDir[MAX_PATH] = "";
	memcpy( szDir, szExe, nLen + 1 );
	char *pSlash = strrchr( szDir, '\\' );
	if ( pSlash ) { *(pSlash+1) = '\0'; } else { szDir[0] = '\0'; }
	strncat( szDir, "ma_win_log.txt", MAX_PATH - strlen(szDir) - 1 );
	_pLogFile = fopen( szDir, "w" );
}

// The engine's own logging: same signature as FangFcnPrintf_t.
static void _FangPrintf( const char *pszFormat, FANG_VA_LIST Args )
{
	char szBuf[2048];
	vsnprintf( szBuf, sizeof(szBuf), pszFormat, Args );
	_Trace( "%s", szBuf );
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

	// 0x40010006 is DBG_PRINTEXCEPTION_C, which kernel32 raises internally for
	// OutputDebugString when nothing is attached to read it. It is not a fault
	// and execution continues, so logging it only buries the real one.
	if( pRec->ExceptionCode == 0x40010006u ) {
		return EXCEPTION_CONTINUE_SEARCH;
	}

	_Trace( "\n[ma] *** CRASH ***\n" );
	_Trace( "[ma] exception code : 0x%08lX\n", (unsigned long)pRec->ExceptionCode );
	_Trace( "[ma] faulting addr  : 0x%08lX\n", (unsigned long)pRec->ExceptionAddress );
	_Trace( "[ma] EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX\n",
		(unsigned long)pRec->ExceptionInformation[0], (unsigned long)pRec->ExceptionInformation[1],
		(unsigned long)pRec->ExceptionInformation[2], (unsigned long)pRec->ExceptionInformation[3] );

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
	_Trace( "[ma] ma_win.exe base: 0x%08lX\n", (unsigned long)(ULONG_PTR)GetModuleHandleA( "ma_win.exe" ) );

	fflush( stdout );
	if( _pLogFile ) fflush( _pLogFile );
	return EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------------------
// Global startup configuration, filled by the dialog and read by _RunGame.
// ---------------------------------------------------------------------------

static char _szMasterFile[512]	= "";
static char _szLevelFile[256]	= "";
static char _szScreenshotDir[512]	= "";
static u32  _nScreenWidth			= 640;
static u32  _nScreenHeight		= 480;
static u32  _nColorBits			= 32;

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

static HWND _hwndStatus = NULL;

static void _SetStatus( const char *pszText )
{
	if( _hwndStatus ) {
		SetWindowText( _hwndStatus, pszText );
	}
}

// ---------------------------------------------------------------------------
// Persistence
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

	_AddLabel( hwndDlg, nX, nY + 4, nLabelW, 20, "Level / file:" );
	_AddEdit( hwndDlg, _ID_LEVELEDIT, _szLevelFile, nCtrlX, nY, nCtrlW );
	nY += 32;

	_AddLabel( hwndDlg, nX, nY + 4, nLabelW, 20, "Screenshots:" );
	_AddEdit( hwndDlg, _ID_SSHOTEDIT, _szScreenshotDir, nCtrlX, nY, nCtrlW );
	_AddButton( hwndDlg, _ID_BROWSE_SSHOT, "Browse...", nCtrlX + nCtrlW + 8, nY, 80, 24 );
	nY += 32;

	_AddLabel( hwndDlg, nX, nY + 4, nLabelW, 20, "Resolution:" );
	_AddCombo( hwndDlg, _ID_WIDTH, nCtrlX, nY, 120 );
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

	_Trace( "[ma] ---- startup ----\n" );
	_Trace( "[ma] master file : %s\n", _szMasterFile );
	_Trace( "[ma] level       : %s\n", _szLevelFile[0] ? _szLevelFile : "(none)" );
	_Trace( "[ma] video       : %ux%u %u-bit\n", _nScreenWidth, _nScreenHeight, _nColorBits );

	fang_Init();
	_Trace( "[ma] fang_Init done\n" );

	// Same configuration the retail PC dialog used
	// (App/ma/win/ma_winDlg.cpp, CMa_winDlg::OnInitDialog / OnButtonRun).
	Fang_ConfigDefs.nRes_HeapBytes			= (64) * 1024 * 1024;
	Fang_ConfigDefs.pszFile_MasterFilePathName	= _szMasterFile;
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
	GameloopParms.bPlayerDeath			= FALSE;
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

	pVidWin->VidMode.nFlags		= FVID_MODEFLAG_NONE;
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
		_Trace( "[ma] pumping messages\n" );
		BOOL bQuit = FALSE;
		MSG msg;
		while( !bQuit && floop_IsGameloopRunning() ) {
			while( PeekMessage( &msg, NULL, 0, 0, PM_REMOVE ) ) {
				if( msg.message == WM_QUIT ) {
					_Trace( "[ma] WM_QUIT -- ending the gameloop\n" );
					gameloop_End();
					bQuit = TRUE;
					break;
				}
				TranslateMessage( &msg );
				DispatchMessage( &msg );
			}
			if( !bQuit ) Sleep( 1 );
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

	_OpenLog();
	AddVectoredExceptionHandler( 1, _CrashLogger );
	{
		char szExe[MAX_PATH] = "";
		GetModuleFileNameA( NULL, szExe, MAX_PATH );
		_Trace( "[ma] WinMain, exe = %s\n", szExe );
	}

	CoInitialize( NULL );

	CSettings::SetSettingsFilename( "ma_win.ini" );
	CSettings::SetApplicationName( "ma_win" );
	_LoadConfig();

	BOOL bAutoRun = FALSE;
	if( pszCmdLine ) {
		for( const char* p = pszCmdLine; *p; p++ ) {
			if( *p == 'a' || *p == 'A' ) {
				const char* q = p;
				while( *q == 'a' || *q == 'A' ) { q++; }
				if( strncmp( q, "utorun", 6 ) == 0 ) {
					bAutoRun = TRUE;
					break;
				}
			}
		}
	}

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

	if( !_RunGame() ) {
		CoUninitialize();
		return 1;
	}

	CoUninitialize();
	return 0;
}
