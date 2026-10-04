//////////////////////////////////////////////////////////////////////////////////////
// ScreenGrab.c - screen capture utility
//
// Author: Michael Starich   
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
// 04/27/01 Starich     Created.
//////////////////////////////////////////////////////////////////////////////////////
#include "StdAfx.h"
#include "fang.h"
#include "ScreenGrab.h"
#include "fvid.h" 
#include "fclib.h"
#include <stdio.h>
 
//====================
// private definitions


#define _TGA_SIGNATURE				"TRUEVISION-XFILE.\0"
#define _TGA_HEADER_SIZE_IN_BYTES	18
#define _TGA_FOOTER_SIZE_IN_BYTES	26

#pragma pack( push, 1 )

// TGA File Header (must not be padded to be 32 bit aligned)
typedef struct {                           
    u8 nIdLength;           // Image ID Field Length
    u8 nColorMapType;       // Color Map Type
    u8 nImageType;          // Image Type
    // Color Map Specification
    u16 nColorMapIndex;		// First Entry Index
    u16 nColorMapLength;	// Color Map Length
    u8 nColorMapEntrySize;  // Color Map Entry Size
    // Image Specification
    u16 nX_Origin;			// X-origin of Image
    u16 nY_Origin;			// Y-origin of Image
    u16 nImageWidth;		// Image Width
    u16 nImageHeight;		// Image Height
    u8 nPixelDepth;         // Pixel Depth
    u8 nImagDesc;           // Image Descriptor
} TgaHeader_t;

// TGA File Footer
typedef struct {
    u32 nExtensionOffset;	// Extension Area Offset
    u32 nDeveloperOffset;	// Developer Directory Offset
    char pszSignature[18];	// Signature, dot, and NULL
} TgaFooter_t;

#pragma pack( pop )

//=================
// public variables

//==================
// private variables

static char _pszFullFileName[256];
static char _pszPathName[200];
static char _pszFileName[32];
static u32 _nSFileNum = 0;
static u32 _nMFileSetNum = 0;
static u32 _nMFileNum = 0;
static u32 _nLastFrameNum = 0;
static char *_pImageBuffer = NULL;
static u32 _nImageBufferBytes = 0;
static char *_pDiskImage = NULL;
static u32 _nNumPixels;
static u32 _nNumPixelBytes;
static BOOL _bSystemReady = FALSE;
static TgaHeader_t _TGAHeader;
static TgaFooter_t _TGAFooter;

//===================
// private prototypes

static void _Reset();
static cchar *_CreateFullFileName( BOOL bSingle );
static BOOL _SaveAsBMP( cchar *pszPathName, const u8 *pn24BitImageData );
static void _ConvertToJpegFilename( void );

//=================
// public functions

BOOL screengrab_Init( cchar *pszDirectory ) {
	FILE *pFile;

	_Reset();

	if( !pszDirectory ) {
		return FALSE;
	}
	// get the size of the buffer needed
	_nImageBufferBytes = fvid_GetScreenGrabBufferSizeInBytes();
	if( _nImageBufferBytes == 0 ) {
		return FALSE;
	}
	// allocate enough memory to hold the screen shot
	_pImageBuffer = (char *)fang_Malloc( _nImageBufferBytes, 0 );
	if( !_pImageBuffer ) {
		return FALSE;
	}
	// allocate enough memory to hold the disk image
	_nNumPixels = _nImageBufferBytes >> 2;	
	_nNumPixelBytes = _nNumPixels * 3;
	_pDiskImage = (char *)fang_Malloc( _nNumPixelBytes, 0 );
	if( !_pDiskImage ) {
		fang_Free( _pImageBuffer );
		_pImageBuffer = NULL;
		return FALSE;
	}
	// copy our directory
	fclib_strcpy( _pszPathName, pszDirectory );
	// search _pszPathName for the last single & multi filename
	_nSFileNum = 0;
	while( TRUE ) {	
		_nSFileNum++;
		// try to open the tga file
		pFile = fopen( _CreateFullFileName( TRUE ), "rb" );
		if( pFile ) {
			fclose( pFile );
			continue;
		}
		
		// try to open the jpeg file
		_ConvertToJpegFilename();
		pFile = fopen( _pszFullFileName, "rb" );
		if( pFile ) {
			fclose( pFile );
			continue;
		}
		
		// once we fail we have found the filename
		break;
	}		
	_nMFileNum = 1;
	_nMFileSetNum = 0;
	while( TRUE ) {	
		_nMFileSetNum++;
		// try to open the tga file
		pFile = fopen( _CreateFullFileName( FALSE ), "rb" );
		if( pFile ) {
			fclose( pFile );
			continue;
		}
		
		// try to open the jpeg file
		_ConvertToJpegFilename();
		pFile = fopen( _pszFullFileName, "rb" );
		if( pFile ) {
			fclose( pFile );
			continue;
		}
		
		// once we fail we have found the filename
		break;
	}
	// setup our header
	_TGAHeader.nIdLength = 0;
    _TGAHeader.nColorMapType = 0;
    _TGAHeader.nImageType = 2;
    _TGAHeader.nColorMapIndex = 0;
    _TGAHeader.nColorMapLength = 0;
    _TGAHeader.nColorMapEntrySize = 0;
    _TGAHeader.nX_Origin = 0;
    _TGAHeader.nY_Origin = 0;
    _TGAHeader.nImageWidth = FVid_Mode.nPixelsAcross;
    _TGAHeader.nImageHeight = FVid_Mode.nPixelsDown;
    _TGAHeader.nPixelDepth = 24;
    _TGAHeader.nImagDesc = 0x20;// flip the image
	// fill in the footer
	_TGAFooter.nExtensionOffset = 0;
	_TGAFooter.nDeveloperOffset = 0;
	fclib_strcpy( _TGAFooter.pszSignature, _TGA_SIGNATURE );

	_nLastFrameNum = 0;
	_bSystemReady = TRUE;
	
	return TRUE;
}

void screengrab_Shutdown() {
	_Reset();
}

BOOL screengrab_CaptureTheScreen( BOOL bSingle, ScreenGrabOutputType_e nOutputType/*=SCREEN_GRAB_OUTPUT_TYPE_TGA*/ ) {
	FILE *pFile;
	u32 i;
	char *pSrc, *pDest;
	BOOL bReturnVal;
	
	if( !_bSystemReady ) {
		return FALSE;
	}
	if( nOutputType >= SCREEN_GRAB_OUTPUT_TYPE_COUNT ) {
		// default to TGA
		nOutputType = SCREEN_GRAB_OUTPUT_TYPE_TGA;
	}
	if( !fvid_ScreenGrab( _pImageBuffer, _nImageBufferBytes ) ) {
		return FALSE;
	}
	if( !bSingle ) {
		if( _nLastFrameNum ) {
			// see if we should continue the last set or start a new one
			if( FVid_nFrameCounter != (_nLastFrameNum+1) ) {
				// start a new multi set
				++_nMFileSetNum;
				_nMFileNum = 1;
			}
		}
		// record the frame number
		_nLastFrameNum = FVid_nFrameCounter;
	}	
	_CreateFullFileName( bSingle );
	if( !bSingle ) {
		// advance the multi file num
		_nMFileNum++;
	} else {
		_nSFileNum++;
	}
	// create our memory image
	pDest = _pDiskImage;
	pSrc = _pImageBuffer;
	for( i=0; i < _nImageBufferBytes; i+=4 ) {
		// b
		*pDest = *pSrc;
		pDest++;
		pSrc++;
		// g
		*pDest = *pSrc;
		pDest++;
		pSrc++;
		// r
		*pDest = *pSrc;
		pDest++;
		pSrc += 2;// advance past alpha as well		
	}
	
	switch( nOutputType ) {

	case SCREEN_GRAB_OUTPUT_TYPE_TGA:
		// write out a tga file
		pFile = fopen( _pszFullFileName, "wb" );
		if( !pFile ) {
			return FALSE;
		}
		// write out our file to disk
		fwrite( &_TGAHeader, _TGA_HEADER_SIZE_IN_BYTES, 1, pFile );
		fwrite( _pDiskImage, _nNumPixelBytes, 1, pFile );
		fwrite( &_TGAFooter, _TGA_FOOTER_SIZE_IN_BYTES, 1, pFile );

		fclose( pFile );

		bReturnVal = TRUE;
		break;

	case SCREEN_GRAB_OUTPUT_TYPE_JPEG:
		// write out a jpeg file
		_ConvertToJpegFilename();
		bReturnVal = _SaveAsBMP( _pszFullFileName, (const u8 *)_pDiskImage );
		break;
	}

	return bReturnVal;
}

//==================
// private functions

static void _Reset() {
	_bSystemReady = FALSE;
	fang_MemZero( _pszFullFileName, 256 );
	fang_MemZero( _pszPathName, 200 );
	fang_MemZero( _pszFileName, 32 );
	_nSFileNum = 0;
	_nMFileSetNum = 0;
	_nMFileNum = 0;
	_nLastFrameNum = 0;
	if( _pImageBuffer ) {
		fang_Free( _pImageBuffer );
		_pImageBuffer = NULL;
	}
	if( _pDiskImage ) {
		fang_Free( _pDiskImage );
		_pDiskImage = NULL;
	}
	_nNumPixels = 0;
	_nNumPixelBytes = 0;

	_nImageBufferBytes = 0;
}

// creates an 8.3 name and stores the full file name into _pszFullFileName
static cchar *_CreateFullFileName( BOOL bSingle ) {
	if( bSingle ) {
		sprintf( _pszFileName, "S%07d.TGA", _nSFileNum );
		sprintf( _pszFullFileName, "%s\\%s", _pszPathName, _pszFileName );
	} else {
		sprintf( _pszFileName, "M%02d%05d.TGA", _nMFileSetNum, _nMFileNum );
		sprintf( _pszFullFileName, "%s\\%s", _pszPathName, _pszFileName );
	}
	return _pszFullFileName;
}

// Writes the 24-bit BGR buffer as an uncompressed 24-bit BMP.
//
// This used to encode a JPEG through the Intel JPEG Library (ijl15l.lib). That
// library is an MSVC-compiled static library whose objects reference _except_list
// and _aullshr from the MSVC C runtime, neither of which mingw provides, so it
// cannot be linked into this build at all -- it took ScreenGrab.cpp out of the
// target and with it the in-game screenshot feature. The capture itself is
// independent of the file format, so writing a BMP keeps the feature working.
static BOOL _SaveAsBMP( cchar *pszPathName, const u8 *pn24BitImageData ) {
	const s32 nWidth  = (s32)FVid_Mode.nPixelsAcross;
	const s32 nHeight = (s32)FVid_Mode.nPixelsDown;
	if ( nWidth <= 0 || nHeight <= 0 ) return FALSE;

	// Each BMP scanline is padded to a 4-byte boundary.
	const s32 nRowBytes = ( (nWidth * 3 + 3) / 4 ) * 4;
	const DWORD dwPixelBytes = (DWORD)nRowBytes * (DWORD)nHeight;
	const DWORD dwFileSize = 14 + 40 + dwPixelBytes;

	BITMAPFILEHEADER bf;
	ZeroMemory( &bf, sizeof(bf) );
	bf.bfType      = 0x4D42;               // 'BM'
	bf.bfSize      = dwFileSize;
	bf.bfOffBits   = 14 + 40;
	bf.bfReserved1 = 0;
	bf.bfReserved2 = 0;

	BITMAPINFOHEADER bi;
	ZeroMemory( &bi, sizeof(bi) );
	bi.biSize          = 40;
	bi.biWidth         = nWidth;
	bi.biHeight        = -nHeight;        // negative: top-down rows
	bi.biPlanes        = 1;
	bi.biBitCount      = 24;
	bi.biCompression   = BI_RGB;
	bi.biSizeImage     = dwPixelBytes;

	FILE *pFile = fopen( pszPathName, "wb" );
	if ( !pFile ) {
		DEVPRINTF( "Could not open '%s' for writing\n", pszPathName );
		return FALSE;
	}
	BOOL bOK = (fwrite( &bf, sizeof(bf), 1, pFile) == 1)
	        && (fwrite( &bi, sizeof(bi), 1, pFile) == 1);

	// Bottom-up in the file, so the rows go out in reverse.
	u8 *pRow = (u8 *)malloc( nRowBytes );
	if ( !pRow ) {
		fclose( pFile );
		return FALSE;
	}
	for ( s32 y = 0; bOK && y < nHeight; y++ ) {
		memset( pRow, 0, nRowBytes );
		memcpy( pRow, pn24BitImageData + (size_t)y * (size_t)nWidth * 3, (size_t)nWidth * 3 );
		bOK = (fwrite( pRow, 1, nRowBytes, pFile) == (size_t)nRowBytes);
	}
	free( pRow );
	fclose( pFile );

	if ( !bOK ) {
		DEVPRINTF( "Could not write '%s'\n", pszPathName );
	}
	return bOK;
}

static void _ConvertToJpegFilename( void ) {
	// Only the extension is changed. _SaveAsBMP writes a BMP (see its comment),
	// so the file has to be named .bmp rather than .jpg. The function keeps its
	// original name because SCREEN_GRAB_OUTPUT_TYPE_JPEG and its callers are
	// unchanged.
	s32 nLen = fclib_strlen( _pszFullFileName );
	if( nLen > 3 ) {
		// assume that the last 3 characters are the extension
		_pszFullFileName[nLen-3] = 'B';
		_pszFullFileName[nLen-2] = 'M';
		_pszFullFileName[nLen-1] = 'P';
	}
}
 