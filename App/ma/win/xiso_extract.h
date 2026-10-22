#ifndef XISO_EXTRACT_H
#define XISO_EXTRACT_H

#include <windows.h>

// Progress callback: called for every file and periodically while copying a
// large one. pszName is the path relative to the destination root.
typedef void (*XisoProgressFn)( void *pUser, const char *pszName,
                                unsigned uFiles, unsigned long long uBytes );

// Extract an Xbox XDVDFS image (.iso / .xiso) into pszDestDir.
// Handles both game-only images (volume descriptor at 0x10000) and full-disc
// images (scans for the MICROSOFT*XBOX*MEDIA descriptor).
// Returns TRUE on success; on failure pszError receives a short reason.
BOOL Xiso_Extract( const char *pszIsoPath, const char *pszDestDir,
                   XisoProgressFn pFn, void *pUser,
                   char *pszError, int nErrorLen,
                   unsigned *puFiles, unsigned long long *puBytes );

#endif // XISO_EXTRACT_H
