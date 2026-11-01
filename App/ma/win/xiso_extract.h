#ifndef XISO_EXTRACT_H
#define XISO_EXTRACT_H

#include <windows.h>

// Progress callback: called for every file and periodically while copying a
// large one. pszName is the path relative to the destination root.
typedef void (*XisoProgressFn)( void *pUser, const char *pszName,
                                unsigned uFiles, unsigned long long uBytes );

// Result of probing the disc's root default.xbe without extracting.
typedef enum {
    XISO_PROBE_OK = 0,       // default.xbe found and its certificate parsed
    XISO_PROBE_NO_XBE,       // valid XDVDFS image, but no usable root default.xbe
    XISO_PROBE_BAD_IMAGE     // not a readable XDVDFS image
} XisoProbeResult_e;

// Probe the root default.xbe certificate without extracting the image.
// On XISO_PROBE_OK, *puTitleId and *puRegion are filled. On BAD_IMAGE,
// pszError receives a short reason. NO_XBE leaves pszError empty; the caller
// can still extract an asset-only image, but should treat it as untested.
XisoProbeResult_e Xiso_ProbeXbe( const char *pszIsoPath,
                                 unsigned *puTitleId, unsigned *puRegion,
                                 char *pszError, int nErrorLen );

// Extract an Xbox XDVDFS image (.iso / .xiso) into pszDestDir.
// Handles both game-only images (volume descriptor at 0x10000) and full-disc
// images (scans for the MICROSOFT*XBOX*MEDIA descriptor).
// Returns TRUE on success; on failure pszError receives a short reason.
BOOL Xiso_Extract( const char *pszIsoPath, const char *pszDestDir,
                   XisoProgressFn pFn, void *pUser,
                   char *pszError, int nErrorLen,
                   unsigned *puFiles, unsigned long long *puBytes );

#endif // XISO_EXTRACT_H
