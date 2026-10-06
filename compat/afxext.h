// compat/afxext.h -- forwards to the MFC subset shim. See compat/mfc.h.
//
// App/ma/win/StdAfx.h and FileInfo.h include the MFC headers by these names.
// Nothing in the engine uses the facilities they normally provide (OLE
// automation, dialog classes, Internet Explorer controls); the only MFC types
// actually referenced are CWinThread and CString, both in mfc.h.
#pragma once
#include "mfc.h"
