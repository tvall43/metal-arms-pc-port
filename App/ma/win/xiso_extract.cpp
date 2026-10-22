// xiso_extract.cpp -- self-contained XDVDFS (.iso/.xiso) extractor for the
// launcher. No extract-xiso dependency, so there is no GPL/linkage question.
//
// XDVDFS layout: a 2048-byte volume descriptor at sector 32 (or anywhere in a
// full-disc image) beginning "MICROSOFT*XBOX*MEDIA", root directory sector at
// +0x14, root directory size at +0x18. Directory tables are binary trees of
// 4-byte-aligned entries: left/right child offsets (in 4-byte units), start
// sector, size, attributes (0x10 = directory), name length, name.

#include "xiso_extract.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

#include <string>
#include <vector>

#define XISO_SECTOR_SIZE   2048u
#define XISO_MAGIC         "MICROSOFT*XBOX*MEDIA"
#define XISO_MAGIC_LEN     20
#define XISO_MAX_DIR_BYTES (64u * 1024u * 1024u)
#define XISO_MAX_DEPTH     32

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;

static void SetErr( char *pszError, int nErrorLen, const char *pszFormat, ... )
{
	if( !pszError || nErrorLen <= 0 ) return;
	va_list args;
	va_start( args, pszFormat );
	vsnprintf( pszError, (size_t)nErrorLen, pszFormat, args );
	va_end( args );
	pszError[nErrorLen - 1] = 0;
}

static std::string AbsPath( const char *pszPath )
{
	if( !pszPath || !pszPath[0] ) return std::string();
	// Already absolute (drive letter or UNC).
	if( ( pszPath[0] && pszPath[1] == ':' ) ||
		( pszPath[0] == '\\' && pszPath[1] == '\\' ) ) {
		return std::string( pszPath );
	}
	char szFull[2048];
	DWORD n = GetFullPathNameA( pszPath, sizeof(szFull), szFull, NULL );
	if( n > 0 && n < sizeof(szFull) ) return std::string( szFull, n );
	return std::string( pszPath );
}

static std::string LongPath( const std::string &s )
{
	if( s.compare( 0, 4, "\\\\?\\" ) == 0 ) return s;
	if( s.size() >= 2 && s[0] == '\\' && s[1] == '\\' ) return "\\\\?\\UNC\\" + s.substr( 2 );
	return "\\\\?\\" + s;
}

static BOOL ReadAt( HANDLE hFile, u64 uOffset, void *pBuffer, u32 uBytes )
{
	LARGE_INTEGER li;
	li.QuadPart = (LONGLONG)uOffset;
	if( !SetFilePointerEx( hFile, li, NULL, FILE_BEGIN ) ) return FALSE;
	u8 *p = (u8 *)pBuffer;
	while( uBytes ) {
		DWORD nRead = 0;
		DWORD nWant = ( uBytes > 0x10000000u ) ? 0x10000000u : uBytes;
		if( !ReadFile( hFile, p, nWant, &nRead, NULL ) || nRead == 0 ) return FALSE;
		p += nRead;
		uBytes -= nRead;
	}
	return TRUE;
}

static BOOL WriteAll( HANDLE hFile, const void *pBuffer, u32 uBytes )
{
	const u8 *p = (const u8 *)pBuffer;
	while( uBytes ) {
		DWORD nWritten = 0;
		DWORD nWant = ( uBytes > 0x10000000u ) ? 0x10000000u : uBytes;
		if( !WriteFile( hFile, p, nWant, &nWritten, NULL ) || nWritten == 0 ) return FALSE;
		p += nWritten;
		uBytes -= nWritten;
	}
	return TRUE;
}

static void EnsureDir( const std::string &sDir )
{
	std::string cur;
	for( size_t i = 0; i < sDir.size(); ++i ) {
		cur += sDir[i];
		if( ( sDir[i] == '\\' || sDir[i] == '/' ) && cur.size() > 3 ) {
			std::string part = cur;
			while( !part.empty() && ( part[part.size() - 1] == '\\' || part[part.size() - 1] == '/' ) ) {
				part.erase( part.size() - 1 );
			}
			if( part.size() > 2 && part[1] != ':' ) {
				CreateDirectoryA( LongPath( part ).c_str(), NULL );
			}
		}
	}
	std::string part = sDir;
	while( !part.empty() && ( part[part.size() - 1] == '\\' || part[part.size() - 1] == '/' ) ) {
		part.erase( part.size() - 1 );
	}
	if( !part.empty() ) CreateDirectoryA( LongPath( part ).c_str(), NULL );
}

struct XisoCtx {
	HANDLE			hIso;
	u64				uVolumeBase;	// byte offset of the XDVDFS partition
	std::string		sDest;
	XisoProgressFn	pFn;
	void			*pUser;
	unsigned		uFiles;
	u64				uBytes;
	char			*pszError;
	int				nErrorLen;
};

static BOOL WalkDir( XisoCtx &c, u64 uDirOffset, u32 uDirSize, const std::string &sRel, int nDepth );

static BOOL ExtractOne( XisoCtx &c, u64 uOffset, u32 uSize, const std::string &sRel, const std::string &sName )
{
	std::string sPath = c.sDest + "\\" + sRel + sName;
	HANDLE hOut = CreateFileA( LongPath( sPath ).c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, NULL );
	if( hOut == INVALID_HANDLE_VALUE ) {
		SetErr( c.pszError, c.nErrorLen, "Cannot create '%s' (error %lu)", sPath.c_str(), GetLastError() );
		return FALSE;
	}

	BOOL bOk = TRUE;
	static u8 aBuffer[1024 * 1024];
	u32 uLeft = uSize;
	u64 uReadPos = uOffset;
	while( uLeft && bOk ) {
		u32 nChunk = ( uLeft > sizeof(aBuffer) ) ? sizeof(aBuffer) : uLeft;
		if( !ReadAt( c.hIso, uReadPos, aBuffer, nChunk ) || !WriteAll( hOut, aBuffer, nChunk ) ) {
			bOk = FALSE;
			break;
		}
		uReadPos += nChunk;
		uLeft -= nChunk;
		c.uBytes += nChunk;
		if( c.pFn && ( ( c.uBytes & 0xFFFFFF ) < nChunk ) ) {
			c.pFn( c.pUser, ( sRel + sName ).c_str(), c.uFiles, c.uBytes );
		}
	}
	CloseHandle( hOut );
	if( !bOk ) {
		SetErr( c.pszError, c.nErrorLen, "Read/write failed for '%s' (error %lu)", sPath.c_str(), GetLastError() );
		return FALSE;
	}
	c.uFiles++;
	if( c.pFn ) c.pFn( c.pUser, ( sRel + sName ).c_str(), c.uFiles, c.uBytes );
	return TRUE;
}

static BOOL WalkTable( XisoCtx &c, const std::vector<u8> &db, u32 uOff, const std::string &sRel,
					   std::vector<u8> &seen, int nDepth )
{
	if( nDepth > XISO_MAX_DEPTH ) return TRUE;
	if( uOff + 14 > db.size() ) return TRUE;
	if( ( uOff & 3 ) != 0 ) return TRUE;
	const u32 uSlot = uOff / 4;
	if( uSlot >= seen.size() ) return TRUE;
	if( seen[uSlot] ) return TRUE;
	seen[uSlot] = 1;

	u16 uLeft, uRight;
	u32 uSector, uSize;
	u8  uAttr, uNameLen;
	memcpy( &uLeft,   &db[uOff + 0], 2 );
	memcpy( &uRight,  &db[uOff + 2], 2 );
	memcpy( &uSector, &db[uOff + 4], 4 );
	memcpy( &uSize,   &db[uOff + 8], 4 );
	uAttr    = db[uOff + 12];
	uNameLen = db[uOff + 13];
	if( uOff + 14 + uNameLen > db.size() ) return TRUE;

	std::string sName( (const char *)&db[uOff + 14], uNameLen );
	bool bBad = sName.empty() || sName == "." || sName == ".." ||
		sName.find( '\\' ) != std::string::npos || sName.find( '/' ) != std::string::npos ||
		sName.find( ':' ) != std::string::npos;
	if( !bBad ) {
		if( uAttr & 0x10 ) {
			if( uSize > 0 && uSize <= XISO_MAX_DIR_BYTES ) {
				u64 uSub = c.uVolumeBase + (u64)uSector * XISO_SECTOR_SIZE;
				if( !WalkDir( c, uSub, uSize, sRel + sName + "\\", nDepth + 1 ) ) return FALSE;
			}
		} else {
			u64 uFile = c.uVolumeBase + (u64)uSector * XISO_SECTOR_SIZE;
			if( !ExtractOne( c, uFile, uSize, sRel, sName ) ) return FALSE;
		}
	}

	if( uLeft ) {
		u32 uChild = (u32)uLeft * 4;
		if( uChild < db.size() && !WalkTable( c, db, uChild, sRel, seen, nDepth ) ) return FALSE;
	}
	if( uRight ) {
		u32 uChild = (u32)uRight * 4;
		if( uChild < db.size() && !WalkTable( c, db, uChild, sRel, seen, nDepth ) ) return FALSE;
	}
	return TRUE;
}

static BOOL WalkDir( XisoCtx &c, u64 uDirOffset, u32 uDirSize, const std::string &sRel, int nDepth )
{
	if( uDirSize == 0 || uDirSize > XISO_MAX_DIR_BYTES ) return TRUE;
	std::vector<u8> db( uDirSize );
	if( !ReadAt( c.hIso, uDirOffset, &db[0], uDirSize ) ) {
		SetErr( c.pszError, c.nErrorLen, "Cannot read directory table at offset %llu", uDirOffset );
		return FALSE;
	}
	std::vector<u8> seen( uDirSize / 4 + 1, 0 );
	return WalkTable( c, db, 0, sRel, seen, nDepth );
}

static BOOL FindVolume( HANDLE hIso, u64 uFileSize, u64 *puVolumeOffset )
{
	u8 vd[XISO_SECTOR_SIZE];
	if( uFileSize >= 0x10000 + XISO_SECTOR_SIZE && ReadAt( hIso, 0x10000, vd, sizeof(vd) ) &&
		memcmp( vd, XISO_MAGIC, XISO_MAGIC_LEN ) == 0 ) {
		*puVolumeOffset = 0x10000;
		return TRUE;
	}

	// Full-disc images put the game partition later. Scan 4 MB chunks at
	// sector alignment; this also covers images with a video partition.
	const u32 uChunk = 4u * 1024u * 1024u;
	std::vector<u8> buf( uChunk );
	for( u64 uOff = 0; uOff + XISO_MAGIC_LEN <= uFileSize; uOff += uChunk ) {
		u32 uWant = ( uFileSize - uOff > uChunk ) ? uChunk : (u32)( uFileSize - uOff );
		if( !ReadAt( hIso, uOff, &buf[0], uWant ) ) break;
		for( u32 p = 0; p + XISO_MAGIC_LEN <= uWant; p += XISO_SECTOR_SIZE ) {
			if( memcmp( &buf[p], XISO_MAGIC, XISO_MAGIC_LEN ) == 0 ) {
				*puVolumeOffset = uOff + p;
				return TRUE;
			}
		}
	}
	return FALSE;
}

BOOL Xiso_Extract( const char *pszIsoPath, const char *pszDestDir,
				   XisoProgressFn pFn, void *pUser,
				   char *pszError, int nErrorLen,
				   unsigned *puFiles, unsigned long long *puBytes )
{
	if( puFiles ) *puFiles = 0;
	if( puBytes ) *puBytes = 0;
	if( pszError && nErrorLen > 0 ) pszError[0] = 0;

	if( !pszIsoPath || !pszIsoPath[0] ) {
		SetErr( pszError, nErrorLen, "No ISO selected." );
		return FALSE;
	}
	if( !pszDestDir || !pszDestDir[0] ) {
		SetErr( pszError, nErrorLen, "No destination folder." );
		return FALSE;
	}

	std::string sIso  = AbsPath( pszIsoPath );
	std::string sDest = AbsPath( pszDestDir );
	HANDLE hIso = CreateFileA( LongPath( sIso ).c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
		OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL );
	if( hIso == INVALID_HANDLE_VALUE ) {
		SetErr( pszError, nErrorLen, "Cannot open '%s' (error %lu)", sIso.c_str(), GetLastError() );
		return FALSE;
	}

	LARGE_INTEGER liSize;
	if( !GetFileSizeEx( hIso, &liSize ) || liSize.QuadPart <= 0 ) {
		CloseHandle( hIso );
		SetErr( pszError, nErrorLen, "Cannot read the ISO size." );
		return FALSE;
	}

	u64 uVolumeDesc = 0;
	if( !FindVolume( hIso, (u64)liSize.QuadPart, &uVolumeDesc ) ) {
		CloseHandle( hIso );
		SetErr( pszError, nErrorLen, "This is not an Xbox XDVDFS image (no MICROSOFT*XBOX*MEDIA descriptor)." );
		return FALSE;
	}
	// The descriptor lives at sector 32 of the XDVDFS partition, so the
	// partition base is 0x10000 before it. A game-only image has the
	// descriptor at 0x10000 (base 0); a full disc with a video partition has
	// it later (e.g. base 0x18300000).
	if( uVolumeDesc < 0x10000 ) {
		CloseHandle( hIso );
		SetErr( pszError, nErrorLen, "Invalid volume descriptor offset." );
		return FALSE;
	}
	u64 uVolume = uVolumeDesc - 0x10000;

	u8 vd[XISO_SECTOR_SIZE];
	if( !ReadAt( hIso, uVolumeDesc, vd, sizeof(vd) ) ) {
		CloseHandle( hIso );
		SetErr( pszError, nErrorLen, "Cannot read the volume descriptor." );
		return FALSE;
	}
	u32 uRootSector, uRootSize;
	memcpy( &uRootSector, &vd[20], 4 );
	memcpy( &uRootSize,   &vd[24], 4 );
	if( uRootSize == 0 || uRootSize > XISO_MAX_DIR_BYTES ) {
		CloseHandle( hIso );
		SetErr( pszError, nErrorLen, "Invalid root directory size (%u).", uRootSize );
		return FALSE;
	}

	EnsureDir( sDest );

	XisoCtx c;
	c.hIso = hIso;
	c.uVolumeBase = uVolume;
	c.sDest = sDest;
	c.pFn = pFn;
	c.pUser = pUser;
	c.uFiles = 0;
	c.uBytes = 0;
	c.pszError = pszError;
	c.nErrorLen = nErrorLen;

	u64 uRootOffset = uVolume + (u64)uRootSector * XISO_SECTOR_SIZE;
	BOOL bOk = WalkDir( c, uRootOffset, uRootSize, "", 0 );
	CloseHandle( hIso );

	if( puFiles ) *puFiles = c.uFiles;
	if( puBytes ) *puBytes = c.uBytes;
	if( !bOk ) {
		if( pszError && nErrorLen > 0 && pszError[0] == 0 ) {
			SetErr( pszError, nErrorLen, "Extraction failed." );
		}
		return FALSE;
	}
	return TRUE;
}
