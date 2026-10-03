// compat/Afxmt.h
//
// WinPrintf.cpp includes this for MFC's CCriticalSection, which it uses to
// guard the debug line buffer. MFC's CCriticalSection is a thin wrapper over a
// Win32 CRITICAL_SECTION, so this provides the same semantics -- in particular
// the recursive-per-thread ownership that CRITICAL_SECTION gives for free.
#pragma once

#include <windows.h>

class CCriticalSection
{
public:
    CCriticalSection()    { InitializeCriticalSection( &m_cs ); }
    ~CCriticalSection()   { DeleteCriticalSection( &m_cs ); }

    void Lock()   { EnterCriticalSection( &m_cs ); }
    void Unlock() { LeaveCriticalSection( &m_cs ); }

    // MFC also exposes this; provided for source compatibility.
    BOOL TryLock()    { return TryEnterCriticalSection( &m_cs ) ? TRUE : FALSE; }
    BOOL IsLocked()   { return TRUE; }

private:
    // Non-copyable, as MFC's is.
    CCriticalSection( const CCriticalSection & );
    CCriticalSection &operator = ( const CCriticalSection & );

    CRITICAL_SECTION m_cs;
};
