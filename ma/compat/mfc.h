// compat/mfc.h
//
// Minimal stand-in for the MFC pieces the engine actually uses.
//
// Only two translation units include <Afxwin.h>: fdx8loop.cpp, which genuinely
// uses MFC, and fdx8padio.cpp, which has a stale include and uses nothing.
// Real MFC is not redistributable and mingw has no equivalent, so this supplies
// the small subset that is needed, implemented directly over Win32.
//
// What is provided:
//   CWinThread      the thread base class, including a working thread-message
//                   pump (PumpMessage) and PostThreadMessage
//   the message-map macros, expanded into a real dispatch switch
//   afx_msg         a marker macro; expands to nothing
//   DECLARE_DYNCREATE / IMPLEMENT_DYNCREATE
//                   declared for source compatibility. Nothing in the tree
//                   calls the generated CreateObject() (the CreateObject*
//                   symbols in FCheckPoint/gamesave are unrelated game
//                   functions), so these only need to compile.
//
// Behaviour that matters to fdx8loop.cpp:
//   * CGameThread::Run() ends with 'return ExitInstance();', so the thread
//     entry point must NOT call ExitInstance() itself or it would run twice.
//     CWinThread::Run() has the MFC behaviour of calling ExitInstance(), which
//     makes both the overridden and the default case correct.
//   * m_bAutoDelete is honoured: the thread deletes its own object when it
//     finishes, which floop_UninstallGameloop relies on (it sets
//     _pGameloopThread = NULL without touching the object after posting the
//     exit message).

#pragma once

#ifndef _COMPAT_MFC_H_
#define _COMPAT_MFC_H_ 1

#include <windows.h>

#include <string>

// MFC's message handler marker. Purely for tooling/readability.
#define afx_msg

// MFC's ASSERT/VERIFY. Settings.cpp uses ASSERT in a reference-counted
// singleton; route it to the CRT assert so a failed assertion traps the same
// way it would under MFC in a debug build.
#include <assert.h>
#undef  ASSERT
#define ASSERT(expr)     assert(expr)
#undef  VERIFY
#define VERIFY(expr)     ((void)(!!(expr)))

// ---------------------------------------------------------------------------
// Message maps
// ---------------------------------------------------------------------------
//
// MFC builds a table of (message -> member function) pairs and walks it. These
// expand to a switch instead: same dispatch, but type safe, and no
// pointer-to-member-function casts.

#define DECLARE_MESSAGE_MAP()                                       \
    protected:                                                      \
        virtual BOOL ProcessThreadMessage( UINT nMessage,           \
                                            WPARAM wParam,          \
                                            LPARAM lParam );        \
    public:

// theClass is not in scope inside ON_THREAD_MESSAGE (each macro has its own
// parameter scope), so publish it under a fixed name first. That keeps
// ON_THREAD_MESSAGE's signature identical to MFC's, so the engine's call sites
// need no changes.
#define BEGIN_MESSAGE_MAP( theClass, baseClass )                    \
    typedef theClass msgmap_class_t_;                              \
    BOOL theClass::ProcessThreadMessage( UINT nMessage,             \
                                         WPARAM wParam,            \
                                         LPARAM lParam )           \
    {                                                               \
        switch (nMessage)                                           \
        {

// MFC requires the handler to have signature
//   void afx_msg Class::Handler(WPARAM, LPARAM)
#define ON_THREAD_MESSAGE( msg, fn )                                \
        case (msg):                                                \
            ( this->*(&msgmap_class_t_::fn) )( wParam, lParam );     \
            return TRUE;

#define END_MESSAGE_MAP()                                           \
        default:                                                    \
            break;                                                  \
        }                                                           \
        return FALSE;                                               \
    }

// ---------------------------------------------------------------------------
// Dynamic creation (compile-time only; see the note above)
// ---------------------------------------------------------------------------
#define DECLARE_DYNCREATE( className )                              \
    public:                                                         \
        static className* CreateObject();                           \
        static void*      _CreateObject();                          \
    private:

#define IMPLEMENT_DYNCREATE( className, baseClass )                 \
    className* className::CreateObject() { return new className; }  \
    void*      className::_CreateObject() { return new className; } \
    typedef baseClass className##_super;

// ---------------------------------------------------------------------------
// CString
// ---------------------------------------------------------------------------
//
// ScreenGrab.cpp and Settings.cpp use MFC's CString, but only for default
// construction, printf-style Format(), and a C-style cast to 'const char *'
// (they hand the result straight to WritePrivateProfileString and friends).
// That subset is implemented here over std::string. MFC's CString is
// TCHAR-based; this build is not UNICODE, so TCHAR is char.
class CString
{
public:
    CString();
    CString( const char *pszSrc );
    CString( const CString &rSrc );
    ~CString();

    CString &operator = ( const char *pszSrc );
    CString &operator = ( const CString &rSrc );

    // printf-style, as MFC's. Uses vsnprintf so a long result is truncated
    // rather than overflowing.
    void Format( const char *pszFmt, ... );

    void   Empty();
    int    GetLength() const;
    BOOL   IsEmpty() const;

    operator const char *( void ) const;

    BOOL operator == ( const char *pszOther ) const;
    BOOL operator != ( const char *pszOther ) const;

private:
    std::string m_str;
};

// ---------------------------------------------------------------------------
// CWinThread
// ---------------------------------------------------------------------------
class CWinThread
{
public:
    CWinThread();
    virtual ~CWinThread();

    // TRUE when the thread should delete its own object on exit. fdx8loop.cpp
    // sets this and then relies on the self-delete ("Note that the thread
    // auto-deletes its own object.").
    BOOL m_bAutoDelete;

    HANDLE m_hThread;
    DWORD  m_nThreadID;

    // Overridable thread body. InitInstance returning FALSE aborts the thread
    // without calling Run().
    virtual BOOL InitInstance();

    // Set by a derived InitInstance that drives its own loop, so the thread
    // entry point knows not to call Run() as well. See mfc.cpp.
    BOOL bInitInstanceRanLoop;
    virtual int  ExitInstance();

    // Default Run() calls ExitInstance(), as MFC's does. CGameThread overrides
    // it and calls ExitInstance() itself before returning.
    virtual int  Run();

    // MFC's CWinThread::CreateThread takes a start address and a parameter;
    // this engine calls it with no arguments, meaning "run this object's
    // virtual Run() on a new thread".
    BOOL CreateThread();

    // Drains the pending thread messages, dispatching mapped handlers.
    // Returns FALSE when WM_QUIT was seen, which is how the engine's game loop
    // learns to stop.
    BOOL PumpMessage();

    BOOL PostThreadMessage( UINT nMessage, WPARAM wParam, LPARAM lParam );

    DWORD GetThreadID() const { return m_nThreadID; }

protected:
    // Overridden by DECLARE_MESSAGE_MAP/END_MESSAGE_MAP in a derived class.
    virtual BOOL ProcessThreadMessage( UINT nMessage, WPARAM wParam, LPARAM lParam );

    static DWORD WINAPI _ThreadEntryProc( CWinThread *pThis );
};

#endif // _COMPAT_MFC_H_
