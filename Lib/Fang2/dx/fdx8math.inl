//////////////////////////////////////////////////////////////////////////////////////
// fdx8math.inl - Fang math module (DX version).
//
// Author: Steve Ranck     
//////////////////////////////////////////////////////////////////////////////////////
// THIS CODE IS PROPRIETARY PROPERTY OF SWINGIN' APE STUDIOS, INC.
// Copyright (c) 2000
//
// The contents of this file may not be disclosed to third
// parties, copied or duplicated in any form, in whole or in part,
// without the prior written permission of Swingin' Ape Studios, Inc.
//////////////////////////////////////////////////////////////////////////////////////
// Modification History:
//
// Date     Who         Description
// -------- ----------  --------------------------------------------------------------
// 10/06/00 Ranck       Created.
//////////////////////////////////////////////////////////////////////////////////////

#include "fang.h"

#include <math.h>
#include <xmmintrin.h>

static void __attribute__((unused)) fmath_TempLogBadRA( void *pvRA, f32 fA, f32 fB, f32 fC, f32 fD )
{
	static void *_apvLogged[ 32 ];
	static u32 _uLogged = 0;
	u32 i;
	for( i = 0; i < _uLogged; ++i ) {
		if( _apvLogged[i] == pvRA ) return;
	}
	if( _uLogged < 32 ) {
		_apvLogged[_uLogged++] = pvRA;
		DEVPRINTF( "[ma] TEMP math RA %p a=%g b=%g c=%g d=%g\n", pvRA, fA, fB, fC, fD );
	}
}



//---------------------------------------------------------------------------------------------
// Fast approximation functions:
//---------------------------------------------------------------------------------------------

// Approximate fast sin.
FINLINE f32 fmath_Sin( f32 fAngle ) {
#if 0
	return fmath_AcuSin(fAngle);
#else
	s32 a1, a2, i, i1, i2, nAngle;
	f32 v1, v2;

	if( fAngle<-FMATH_MAX_ANGLE_BEFORE_MODULATING || fAngle>FMATH_MAX_ANGLE_BEFORE_MODULATING ) {
		fAngle = fmath_Fmod( fAngle, FMATH_2PI );
	}

	nAngle = fmath_FloatToS32( fAngle * (65536.0f * (f32)FMATH_SINTBL_ROUND_TABLE_COUNT / FMATH_2PI) );

	i = nAngle & (FMATH_SINTBL_ROUND_TABLE_COUNT-1);
	a1 = nAngle >> FMATH_SINTBL_ROUND_BIT_COUNT;
	a2 = a1 + 1;
	i1 = a1 & 16383;
	i2 = a2 & 16383;

	v1 = SinTbl_afTable[ (a1 & 16384) ? (16384-i1) : i1 ];
	v2 = SinTbl_afTable[ (a2 & 16384) ? (16384-i2) : i2 ];
	if( a1 & 32768 ) v1 *= -1.0f;
	if( a2 & 32768 ) v2 *= -1.0f;

	return v1*FMath_afInterpTable1[i] + v2*FMath_afInterpTable2[i];
#endif
}


// Approximate fast cos.
FINLINE f32 fmath_Cos( f32 fAngle ) {
#if 0
	return fmath_AcuCos(fAngle);
#else
	s32 a1, a2, i, i1, i2, nAngle;
	f32 v1, v2;

	if( fAngle<-FMATH_MAX_ANGLE_BEFORE_MODULATING || fAngle>FMATH_MAX_ANGLE_BEFORE_MODULATING ) {
		fAngle = fmath_Fmod( fAngle, FMATH_2PI );
	}

	nAngle = (16384 * FMATH_SINTBL_ROUND_TABLE_COUNT) + fmath_FloatToS32( fAngle * (65536.0f * (f32)FMATH_SINTBL_ROUND_TABLE_COUNT / FMATH_2PI) );

	i = nAngle & (FMATH_SINTBL_ROUND_TABLE_COUNT-1);
	a1 = nAngle >> FMATH_SINTBL_ROUND_BIT_COUNT;
	a2 = a1 + 1;
	i1 = a1 & 16383;
	i2 = a2 & 16383;

	v1 = SinTbl_afTable[ (a1 & 16384) ? (16384-i1) : i1 ];
	v2 = SinTbl_afTable[ (a2 & 16384) ? (16384-i2) : i2 ];
	if( a1 & 32768 ) v1 *= -1.0f;
	if( a2 & 32768 ) v2 *= -1.0f;

	return v1*FMath_afInterpTable1[i] + v2*FMath_afInterpTable2[i];
#endif
}


// Approximate fast sin & cos.
FINLINE void fmath_SinCos( f32 fAngle, f32 *pfSin, f32 *pfCos ) {
	*pfSin = fmath_Sin( fAngle );
	*pfCos = fmath_Cos( fAngle );
}

// fast approximate inverse cosine. From magic-software.com
FINLINE f32 fmath_InvCos( f32 fCos )
{
	BOOL bNegative = FALSE;

	FMATH_CLAMP( fCos, -1.0f, 1.0f );

	if( fCos < 0.0f )
	{
		fCos = -fCos;
		bNegative = TRUE;
	}

    f32 fRoot = fmath_Sqrt( 1.0f - fCos );

    f32 fResult = -0.0187293f;
    fResult *= fCos;
    fResult += 0.0742610f;
    fResult *= fCos;
    fResult -= 0.2121144f;
    fResult *= fCos;
    fResult += 1.5707288f;
    fResult *= fRoot;

	if( bNegative )
	{
		fResult = FMATH_PI - fResult;
	}

    return fResult;
}


// Approximate fast square-root.
FINLINE f32 fmath_Sqrt( f32 fVal ) {
	f32 fRetVal;

	FASSERT_POS_FLOAT( fVal );

	// Was MSVC asm: rsqrtss / rcpss / movss. Same ops as the intrinsics.
	{
		__m128 v = _mm_rsqrt_ss(_mm_set_ss(fVal));
		fRetVal = _mm_cvtss_f32(_mm_rcp_ss(v));
	}


	if( fmath_Fcheck( fRetVal ) != FMATH_FCHECK_RESULT_OK ) {
		fmath_TempLogBadRA( __builtin_return_address( 0 ), fRetVal, 0.0f, 0.0f, 0.0f );
	}
	FMATH_DEBUG_FCHECK( fRetVal );

	return fRetVal;
}


// Approximate fast inverse square-root.
FINLINE f32 fmath_InvSqrt( f32 fVal ) {
	f32 fRetVal;

	FASSERT_POS_FLOAT( fVal );

	// Was MSVC asm: rsqrtss / movss.
	fRetVal = _mm_cvtss_f32(_mm_rsqrt_ss(_mm_set_ss(fVal)));


	if( fmath_Fcheck( fRetVal ) != FMATH_FCHECK_RESULT_OK ) {
		fmath_TempLogBadRA( __builtin_return_address( 0 ), fRetVal, 0.0f, 0.0f, 0.0f );
	}
	FMATH_DEBUG_FCHECK( fRetVal );

	return fRetVal;
}


// Approximate fast reciprocal.
FINLINE f32 fmath_Inv( f32 fVal ) {
	f32 fRetVal;

	FASSERT( fVal != 0.0f );

	// Was MSVC asm: rcpss / movss.
	fRetVal = _mm_cvtss_f32(_mm_rcp_ss(_mm_set_ss(fVal)));


	if( fmath_Fcheck( fRetVal ) != FMATH_FCHECK_RESULT_OK ) {
		fmath_TempLogBadRA( __builtin_return_address( 0 ), fRetVal, 0.0f, 0.0f, 0.0f );
	}
	FMATH_DEBUG_FCHECK( fRetVal );

	return fRetVal;
}


// Approximate fast division.
FINLINE f32 fmath_Div( f32 fNumerator, f32 fDenominator ) {
	f32 fRetVal;

	FASSERT( fDenominator != 0.0f );

	// Was MSVC asm: rcpss / mulss / movss.
	{
		__m128 v = _mm_rcp_ss(_mm_set_ss(fDenominator));
		v = _mm_mul_ss(v, _mm_set_ss(fNumerator));
		fRetVal = _mm_cvtss_f32(v);
	}


	if( fmath_Fcheck( fRetVal ) != FMATH_FCHECK_RESULT_OK ) {
		fmath_TempLogBadRA( __builtin_return_address( 0 ), fRetVal, 0.0f, 0.0f, 0.0f );
	}
	FMATH_DEBUG_FCHECK( fRetVal );

	return fRetVal;
}


FINLINE s32 fmath_FloatToS32( f32 fVal ) {
	s32 nVal;

	FMATH_DEBUG_FCHECK( fVal );
	FASSERT( fVal>(f32)(s32)0x81000000 && fVal<(f32)(s32)0x7effffff );

	// Was MSVC asm: fld / fistp. FISTP converts using the current FPU rounding
	// mode (round-to-nearest by default) whereas a C cast truncates toward
	// zero; lrintf() honours the same rounding mode FISTP did.
	nVal = (s32)lrintf(fVal);


	return nVal;
}


FINLINE u32 fmath_FloatToU32( f32 fVal ) {
	u32 nVal;

	FMATH_DEBUG_FCHECK( fVal );
	FASSERT( fVal >= 0.0f );
	FASSERT( fVal < (f32)(u32)0xfeffffff );

	// Was MSVC asm: fld / fistp. FISTP converts using the current FPU rounding
	// mode (round-to-nearest by default) whereas a C cast truncates toward
	// zero; lrintf() honours the same rounding mode FISTP did.
	nVal = (u32)lrintf(fVal);


	return nVal;
}



//---------------------------------------------------------------------------------------------
// ANSI compatible, accurate, slow functions:
//---------------------------------------------------------------------------------------------

// ANSI compatible square-root.
FINLINE f32 fmath_AcuSqrt( f32 fVal ) {
	f32 fRetVal;

	FASSERT_POS_FLOAT( fVal );

	// Was MSVC asm: fld / fsqrt / fstp.
	fRetVal = sqrtf(fVal);


	if( fmath_Fcheck( fRetVal ) != FMATH_FCHECK_RESULT_OK ) {
		fmath_TempLogBadRA( __builtin_return_address( 0 ), fRetVal, 0.0f, 0.0f, 0.0f );
	}
	FMATH_DEBUG_FCHECK( fRetVal );

	return fRetVal;
}


// ANSI compatible inverse square-root.
FINLINE f32 fmath_AcuInvSqrt( f32 fVal ) {
#if FANG_DEBUG_BUILD || FANG_TEST_BUILD
	FASSERT_POS_FLOAT( fVal );

	f32 fRetVal = 1.0f / fmath_AcuSqrt( fVal );
	if( fmath_Fcheck( fRetVal ) != FMATH_FCHECK_RESULT_OK ) {
		fmath_TempLogBadRA( __builtin_return_address( 0 ), fRetVal, 0.0f, 0.0f, 0.0f );
	}
	FMATH_DEBUG_FCHECK( fRetVal );

	return fRetVal;
#else
	FASSERT_POS_FLOAT( fVal );
	return 1.0f / fmath_AcuSqrt( fVal );
#endif
}


// Accurate slow sin.
FINLINE f32 fmath_AcuSin( f32 fAngle ) {
	f32 fRetValue;

	// Was MSVC asm: fld / fsin / fstp.
	fRetValue = sinf(fAngle);


	return fRetValue;
}


// Accurate slow cos.
FINLINE f32 fmath_AcuCos( f32 fAngle ) {
	f32 fRetValue;

	// Was MSVC asm: fld / fcos / fstp.
	fRetValue = cosf(fAngle);


	return fRetValue;
}


// Accurate slow sin & cos.
FINLINE void fmath_AcuSinCos( f32 fAngle, f32 *pfSin, f32 *pfCos ) {
	f32 fSinResult, fCosResult;

	// Was MSVC asm: fld / fsincos / fstp.
	fSinResult = sinf(fAngle);
	fCosResult = cosf(fAngle);


	*pfSin = fSinResult;
	*pfCos = fCosResult;
}


// Accurate slow sin biased from 0 to +1.
FINLINE f32 fmath_AcuPositiveSin( f32 fAngle ) {
	f32 fRetValue;

	// Was MSVC asm: fld / fsin / fstp.
	fRetValue = sinf(fAngle);


	return 0.5f + 0.5f*fRetValue;
}


// Accurate slow cos biased from 0 to +1.
FINLINE f32 fmath_AcuPositiveCos( f32 fAngle ) {
	f32 fRetValue;

	// Was MSVC asm: fld / fcos / fstp.
	fRetValue = cosf(fAngle);


	return 0.5f + 0.5f*fRetValue;
}


// Accurate slow sin & cos biased from 0 to +1.
FINLINE void fmath_AcuPositiveSinCos( f32 fAngle, f32 *pfSin, f32 *pfCos ) {
	f32 fSinResult, fCosResult;

	// Was MSVC asm: fld / fsincos / fstp.
	fSinResult = sinf(fAngle);
	fCosResult = cosf(fAngle);


	*pfSin = 0.5f + 0.5f*fSinResult;
	*pfCos = 0.5f + 0.5f*fCosResult;
}




