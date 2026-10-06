// compat/mfc.cpp -- see compat/mfc.h. Plain Win32 implementation of the small
// CWinThread subset the engine uses.

#include "mfc.h"

#include <cstdarg>

#include <cstdio>

CWinThread::CWinThread()
    : m_bAutoDelete( FALSE )
    , m_hThread( NULL )
    , m_nThreadID( 0 )
{
}

CWinThread::~CWinThread()
{
}

BOOL CWinThread::InitInstance()
{
    return TRUE;
}

int CWinThread::ExitInstance()
{
    return 0;
}

int CWinThread::Run()
{
    // Matches MFC: the default Run() body is ExitInstance(). Derived classes
    // that override Run() are responsible for calling it (CGameThread does).
    return ExitInstance();
}

BOOL CWinThread::ProcessThreadMessage( UINT nMessage, WPARAM wParam, LPARAM lParam )
{
    (void)nMessage;
    (void)wParam;
    (void)lParam;
    return FALSE;
}

// Set from main_win.cpp once the engine's DEVPRINTF sink is installed, so the
// thread lifecycle can be reported even if the failure is in virtual dispatch.
extern "C" void ma_win_Trace( const char *pszFormat, ... );

// Report which Run() this thread would actually call, to catch virtual
// dispatch landing on the base implementation.
static void _ReportRunTarget( CWinThread *pThis )
{
    typedef int (CWinThread::*RunFn)();
    RunFn pBase = &CWinThread::Run;
    union { RunFn m; void *p; } uBase, uObj;
    uBase.m = pBase;
    uObj.m  = pThis->Run;
    void *pViaObject = uObj.p;
    void *pViaBase   = uBase.p;
    ma_win_Trace( "[ma] game thread: tid 0x%08lX  Run via object=%p  base Run=%p  %s\n",
                  (unsigned long)GetCurrentThreadId(), pViaObject, pViaBase,
                  ( pViaObject == pViaBase ) ? "SAME -- virtual dispatch is NOT overriding Run"
                                             : "DIFFERENT -- virtual override is in effect" );
}

DWORD WINAPI CWinThread::_ThreadEntryProc( CWinThread *pThis )
{
    // Deliberately does NOT call ExitInstance(): Run() is the overridable body
    // and CGameThread::Run() already returns ExitInstance(). Calling it twice
    // would run the engine's teardown twice.
    ma_win_Trace( "[ma] game thread entered (this=%p)\n", (void *)pThis );
    _ReportRunTarget( pThis );

    const BOOL bInit = pThis->InitInstance();
    ma_win_Trace( "[ma] game thread: InitInstance returned %d\n", (int)bInit );

    if ( bInit )
    {
        // Only call Run() if the derived InitInstance did not already drive the
        // loop. CGameThread::InitInstance runs it itself with a qualified call,
        // because virtual dispatch on Run does not reach that override here --
        // see the comment there. A derived class that leaves Run() alone gets the
        // normal MFC behaviour.
        ma_win_Trace( "[ma] game thread: calling Run()\n" );
        if ( !pThis->bInitInstanceRanLoop )
        {
            pThis->Run();
            ma_win_Trace( "[ma] game thread: Run() returned\n" );
        }
        else
        {
            ma_win_Trace( "[ma] game thread: loop was run by InitInstance\n" );
        }
    }

    // Read the flag before deleting, then never touch pThis again.
    const BOOL bAutoDelete = pThis->m_bAutoDelete;
    if ( bAutoDelete )
    {
        delete pThis;
    }
    return 0;
}

BOOL CWinThread::CreateThread()
{
    m_hThread = ::CreateThread( NULL, 0, _ThreadEntryProc, this, 0, &m_nThreadID );
    return m_hThread != NULL;
}

BOOL CWinThread::PumpMessage()
{
    MSG Msg;

    // fdx8loop.cpp checks with PeekMessage(PM_NOREMOVE) before calling us, so
    // messages are still queued here; consume them with PM_REMOVE.
    while ( PeekMessage( &Msg, NULL, 0, 0, PM_REMOVE ) )
    {
        if ( Msg.message == WM_QUIT )
        {
            // MFC signals WM_QUIT by returning FALSE from PumpMessage; the
            // engine's loop breaks out when that happens.
            return FALSE;
        }

        if ( Msg.hwnd == NULL )
        {
            // A thread message rather than a window message. Offer it to the
            // class's message map (fdx8loop.cpp hangs _SETUP_LOOP_EXIT off this).
            if ( ProcessThreadMessage( Msg.message, Msg.wParam, Msg.lParam ) )
            {
                continue;
            }
        }

        TranslateMessage( &Msg );
        DispatchMessage( &Msg );
    }

    return TRUE;
}

BOOL CWinThread::PostThreadMessage( UINT nMessage, WPARAM wParam, LPARAM lParam )
{
    if ( m_nThreadID == 0 )
    {
        return FALSE;
    }
    return ::PostThreadMessage( m_nThreadID, nMessage, wParam, lParam ) != FALSE;
}

// ---------------------------------------------------------------------------
// CString
// ---------------------------------------------------------------------------

CString::CString()
{
}

CString::CString( const char *pszSrc )
    : m_str( pszSrc ? pszSrc : "" )
{
}

CString::CString( const CString &rSrc )
    : m_str( rSrc.m_str )
{
}

CString::~CString()
{
}

CString &CString::operator = ( const char *pszSrc )
{
    m_str = pszSrc ? pszSrc : "";
    return *this;
}

CString &CString::operator = ( const CString &rSrc )
{
    if ( this != &rSrc )
    {
        m_str = rSrc.m_str;
    }
    return *this;
}

void CString::Format( const char *pszFmt, ... )
{
    va_list args;
    va_start( args, pszFmt );
    va_list argsCopy;
    va_copy( argsCopy, args );
    const int nNeeded = vsnprintf( NULL, 0, pszFmt, argsCopy );
    va_end( argsCopy );
    if ( nNeeded < 0 )
    {
        va_end( args );
        m_str.clear();
        return;
    }
    m_str.resize( (size_t)nNeeded );
    vsnprintf( &m_str[0], (size_t)nNeeded + 1, pszFmt, args );
    va_end( args );
}

void CString::Empty()
{
    m_str.clear();
}

int CString::GetLength() const
{
    return (int)m_str.size();
}

BOOL CString::IsEmpty() const
{
    return m_str.empty() ? TRUE : FALSE;
}

CString::operator const char *( void ) const
{
    return m_str.c_str();
}

BOOL CString::operator == ( const char *pszOther ) const
{
    return m_str.compare( pszOther ? pszOther : "" ) == 0;
}

BOOL CString::operator != ( const char *pszOther ) const
{
    return !( *this == pszOther );
}
