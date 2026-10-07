//////////////////////////////////////////////////////////////////////////////////////
// fdx8gcmath_quat.inl - Fang quaternion library.
//
// Author: Steve Ranck     
//////////////////////////////////////////////////////////////////////////////////////
// THIS CODE IS PROPRIETARY PROPERTY OF SWINGIN' APE STUDIOS, INC.
// Copyright (c) 2002
//
// The contents of this file may not be disclosed to third
// parties, copied or duplicated in any form, in whole or in part,
// without the prior written permission of Swingin' Ape Studios, Inc.
//////////////////////////////////////////////////////////////////////////////////////
// Modification History:
//
// Date     Who         Description
// -------- ----------  --------------------------------------------------------------
// 02/07/02 Ranck       Created.
//////////////////////////////////////////////////////////////////////////////////////

#if FANG_WINGC


static const CFVec4A _nnnnNegMaskW( (u32)0x00000000, (u32)0x00000000, (u32)0x00000000, (u32)0x80000000 );
static const CFVec4A _nnnnZeroMaskW( (u32)0xffffffff, (u32)0xffffffff, (u32)0xffffffff, (u32)0x00000000 );





//--------------------------------------------------------------------
// CFQuatA Implementation:
//--------------------------------------------------------------------
FINLINE CFQuatA::CFQuatA( void ) {}
FINLINE CFQuatA::CFQuatA( const CFQuatA &rQ ) { Set( rQ ); }
FINLINE CFQuatA::CFQuatA( const CFVec4A &rV ) { Set( rV ); }
FINLINE CFQuatA::CFQuatA( const f32 &fX, const f32 &fY, const f32 &fZ, const f32 &fW ) { Set( fX, fY, fZ, fW ); }

FINLINE BOOL CFQuatA::operator == ( const CFQuatA &rQ ) const { return (v == rQ.v); }
FINLINE BOOL CFQuatA::operator != ( const CFQuatA &rQ ) const { return (v != rQ.v); }

FINLINE CFQuatA &CFQuatA::Zero( void ) { v.Zero(); return *this; }
FINLINE CFQuatA &CFQuatA::Identity( void ) { v.ZeroW1(); return *this; }

FINLINE CFQuatA &CFQuatA::operator = ( const CFQuatA &rQ ) { v = rQ.v; return *this; }
FINLINE CFQuatA &CFQuatA::Set( const CFQuatA &rQ ) { v = rQ.v; return *this; }
FINLINE CFQuatA &CFQuatA::Set( const CFVec4A &rV ) { v = rV; return *this; }
FINLINE CFQuatA &CFQuatA::Set( const f32 &fX, const f32 &fY, const f32 &fZ, const f32 &fW ) { v.Set( fX, fY, fZ, fW );  return *this; }

FINLINE CFQuatA &CFQuatA::BuildQuat( const FPackedQuat_t &rPackedQuat ) {
	x = (f32)rPackedQuat.x * (1.0f/127.0f);
	y = (f32)rPackedQuat.y * (1.0f/127.0f);
	z = (f32)rPackedQuat.z * (1.0f/127.0f);
	w = (f32)rPackedQuat.w * (1.0f/127.0f);

	return *this;
}

FINLINE CFQuatA &CFQuatA::BuildQuat( const CFMtx43 &rMtx ) { return BuildQuat( rMtx.m33 ); }

FINLINE CFQuatA &CFQuatA::BuildQuat( const CFVec4A &rUnitVecToRotateAbout, const f32 &fRadiansToRotateBy ) {
	f32 fSin;

	fmath_SinCos( 0.5f*fRadiansToRotateBy, &fSin, &w );

	x = fSin * rUnitVecToRotateAbout.x;
	y = fSin * rUnitVecToRotateAbout.y;
	z = fSin * rUnitVecToRotateAbout.z;

	return *this;
}

FINLINE CFQuatA &CFQuatA::BuildQuat( const CFVec3A &rUnitVecToRotateAbout, const f32 &fRadiansToRotateBy ) {
	return BuildQuat( rUnitVecToRotateAbout.v4a, fRadiansToRotateBy );
}

FINLINE CFQuatA &CFQuatA::BuildQuat( const CFVec3A &rUnitVecToRotateFrom, const CFVec3A &rUnitVecToRotateTo ) {
	CFVec3A RotationUnitAxis;
	f32 fRotationAxisMag, fCosAngleBetweenVecs, fHalfSin;

	fCosAngleBetweenVecs = rUnitVecToRotateFrom.Dot( rUnitVecToRotateTo );

	w = fmath_Sqrt( (1.0f + fCosAngleBetweenVecs) * 0.5f );

	fCosAngleBetweenVecs = 1.0f - fCosAngleBetweenVecs;
	if( fCosAngleBetweenVecs > 0.00001f ) {
		fHalfSin = fmath_Sqrt( fCosAngleBetweenVecs * 0.5f );

		RotationUnitAxis.Cross( rUnitVecToRotateFrom, rUnitVecToRotateTo );
		fRotationAxisMag = RotationUnitAxis.Mag();

		if( fRotationAxisMag > 0.00001f ) {
			RotationUnitAxis.Div( fRotationAxisMag );
			v.v3 = RotationUnitAxis.v3 * fHalfSin;
		} else {
			v.v3.Zero();
		}
	} else {
		v.v3.Zero();
	}

	FMATH_CLASS_DEBUG_FCHECK( *this );

	return *this;
}

FINLINE CFQuatA &CFQuatA::ReceiveNegative( const CFQuatA &rQ ) { v.Mul( rQ.v, FDX8Math_NegOnesW1 ); return *this; }
FINLINE CFQuatA &CFQuatA::Negate( void ) { v.Mul( FDX8Math_NegOnesW1 ); return *this; }

FINLINE CFQuatA &CFQuatA::Mul( const CFQuatA &rQ1, const CFQuatA &rQ2 ) {
	// The original SSE asm (fdx8math_quat.inl) reads both operands before
	// storing the result, so it is safe when this aliases rQ1/rQ2 - MulPoint
	// depends on that (qResult.Mul(qResult, qConj)). The plain-C rewrite
	// assigned a[3] first and then read rQ1.a[3] in the vector line, so any
	// aliased call squared/mixed the w scalars wrongly.
	f32 fW = rQ1.a[3]*rQ2.a[3] - rQ1.v.v3.Dot( rQ2.v.v3 );
	v.v3 = rQ1.v.v3.Cross( rQ2.v.v3 ) + rQ2.v.v3*rQ1.a[3] + rQ1.v.v3*rQ2.a[3];
	a[3] = fW;

	return *this;
}

FINLINE CFQuatA &CFQuatA::Mul( const CFQuatA &rQ ) {
	CFQuatA TempQuat = *this;

	a[3] = TempQuat.a[3]*rQ.a[3] - TempQuat.v.v3.Dot( rQ.v.v3 );
	v.v3 = TempQuat.v.v3.Cross( rQ.v.v3 ) + rQ.v.v3*TempQuat.a[3] + TempQuat.v.v3*rQ.a[3];

	return *this;
}

FINLINE CFVec3A &CFQuatA::MulPoint( CFVec3A &rRV, const CFVec3A &rV ) const { MulPoint( rRV.v4a, rV.v4a ); return rRV; }

FINLINE CFVec4A &CFQuatA::MulPoint( CFVec4A &rRV, const CFVec4A &rV ) const {
	// Was hand-written SSE assembly computing  q * p * conj(q)  (the block's own
	// comments label it 's1*Q1*p2*Q1''). Re-expressed with the plain-C
	// CFQuatA::Mul already present in this file, so the result is identical by
	// construction rather than by hand-transcribed operand shuffling.
	CFQuatA qPoint, qConj, qResult;

	qPoint.v = rV;
	qPoint.a[3] = 0.0f;
	qConj = *this;
	qConj.v.v3 = qConj.v.v3 * -1.0f;

	qResult.Mul( *this, qPoint );
	qResult.Mul( qResult, qConj );
	rRV = qResult.v;


	return rRV;
}

FINLINE CFVec3A &CFQuatA::MulPoint( CFVec3A &rV ) const { MulPoint( rV.v4a ); return rV; }

FINLINE CFVec4A &CFQuatA::MulPoint( CFVec4A &rV ) const {
	// Was hand-written SSE assembly computing  q * p * conj(q)  (the block's own
	// comments label it 's1*Q1*p2*Q1''). Re-expressed with the plain-C
	// CFQuatA::Mul already present in this file, so the result is identical by
	// construction rather than by hand-transcribed operand shuffling.
	MulPoint( rV, rV );


	return rV;
}

FINLINE CFVec3A &CFQuatA::InvMulPoint( CFVec3A &rRV, const CFVec3A &rV ) const { InvMulPoint( rRV.v4a, rV.v4a ); return rRV; }

FINLINE CFVec4A &CFQuatA::InvMulPoint( CFVec4A &rRV, const CFVec4A &rV ) const {
	// Was hand-written SSE assembly computing  q * p * conj(q)  (the block's own
	// comments label it 's1*Q1*p2*Q1''). Re-expressed with the plain-C
	// CFQuatA::Mul already present in this file, so the result is identical by
	// construction rather than by hand-transcribed operand shuffling.
	CFQuatA qPoint, qConj, qResult;

	qPoint.v = rV;
	qPoint.a[3] = 0.0f;
	qConj = *this;
	qConj.v.v3 = qConj.v.v3 * -1.0f;

	qResult.Mul( qConj, qPoint );
	qResult.Mul( qResult, *this );
	rRV = qResult.v;


	return rRV;
}

FINLINE CFVec3A &CFQuatA::InvMulPoint( CFVec3A &rV ) const { InvMulPoint( rV.v4a ); return rV; }

FINLINE CFVec4A &CFQuatA::InvMulPoint( CFVec4A &rV ) const {
	// Was hand-written SSE assembly computing  q * p * conj(q)  (the block's own
	// comments label it 's1*Q1*p2*Q1''). Re-expressed with the plain-C
	// CFQuatA::Mul already present in this file, so the result is identical by
	// construction rather than by hand-transcribed operand shuffling.
	InvMulPoint( rV, rV );


	return rV;
}

FINLINE CFQuatA &CFQuatA::ReceiveInverse( const CFQuatA &rQ ) { v.Mul( rQ.v, FDX8Math_NegOnesW1 ); return *this; }
FINLINE CFQuatA &CFQuatA::Invert( void ) { v.Mul( FDX8Math_NegOnesW1 ); return *this; }

FINLINE CFQuatA &CFQuatA::ReceiveNonUnitInverse( const CFQuatA &rQ ) { *this=rQ; return NonUnitInvert(); }

FINLINE CFQuatA &CFQuatA::NonUnitInvert( void ) {
	f32 fOOQTimesConjQ;

	fOOQTimesConjQ = fmath_Inv( a[3]*a[3] + a[0]*a[0] + a[1]*a[1] + a[2]*a[2] );

	v.v3 *= -fOOQTimesConjQ;
	a[3] *= fOOQTimesConjQ;

	return *this;
}

FINLINE CFQuatA &CFQuatA::ReceiveUnit( const CFQuatA &rQ ) { v.ReceiveUnit( rQ.v ); return *this; }
FINLINE CFQuatA &CFQuatA::Unitize( void ) { v.Unitize(); return *this; }

FINLINE CFMtx33 &CFQuatA::BuildMtx( CFMtx33 &rDestMtx ) const {
	f32 f2X = 2.0f*x;
	f32 f2Y = 2.0f*y;
	f32 f2Z = 2.0f*z;
	f32 f2WX = f2X*w;
	f32 f2WY = f2Y*w;
	f32 f2WZ = f2Z*w;
	f32 f2XX = f2X*x;
	f32 f2XY = f2Y*x;
	f32 f2XZ = f2Z*x;
	f32 f2YY = f2Y*y;
	f32 f2YZ = f2Z*y;
	f32 f2ZZ = f2Z*z;

	rDestMtx.aa[0][0] = 1.0f - (f2YY + f2ZZ);
	rDestMtx.aa[0][1] = f2XY + f2WZ;
	rDestMtx.aa[0][2] = f2XZ - f2WY;
	rDestMtx.aa[1][0] = f2XY - f2WZ;
	rDestMtx.aa[1][1] = 1.0f - (f2XX + f2ZZ);
	rDestMtx.aa[1][2] = f2YZ + f2WX;
	rDestMtx.aa[2][0] = f2XZ + f2WY;
	rDestMtx.aa[2][1] = f2YZ - f2WX;
	rDestMtx.aa[2][2] = 1.0f - (f2XX + f2YY);

	return rDestMtx;
}


FINLINE CFMtx33 &CFQuatA::BuildMtx( CFMtx33 &rDestMtx, const f32 &fScale ) const {
	f32 f2Scale = 2.0f * fScale;
	f32 f2ScaleX = f2Scale*x;
	f32 f2ScaleY = f2Scale*y;
	f32 f2ScaleZ = f2Scale*z;
	f32 f2ScaleWX = f2ScaleX*w;
	f32 f2ScaleWY = f2ScaleY*w;
	f32 f2ScaleWZ = f2ScaleZ*w;
	f32 f2ScaleXX = f2ScaleX*x;
	f32 f2ScaleXY = f2ScaleY*x;
	f32 f2ScaleXZ = f2ScaleZ*x;
	f32 f2ScaleYY = f2ScaleY*y;
	f32 f2ScaleYZ = f2ScaleZ*y;
	f32 f2ScaleZZ = f2ScaleZ*z;

	rDestMtx.aa[0][0] = fScale - (f2ScaleYY + f2ScaleZZ);
	rDestMtx.aa[0][1] = f2ScaleXY + f2ScaleWZ;
	rDestMtx.aa[0][2] = f2ScaleXZ - f2ScaleWY;
	rDestMtx.aa[1][0] = f2ScaleXY - f2ScaleWZ;
	rDestMtx.aa[1][1] = fScale - (f2ScaleXX + f2ScaleZZ);
	rDestMtx.aa[1][2] = f2ScaleYZ + f2ScaleWX;
	rDestMtx.aa[2][0] = f2ScaleXZ + f2ScaleWY;
	rDestMtx.aa[2][1] = f2ScaleYZ - f2ScaleWX;
	rDestMtx.aa[2][2] = fScale - (f2ScaleXX + f2ScaleYY);

	return rDestMtx;
}


FINLINE CFMtx43A &CFQuatA::BuildMtx( CFMtx43A &rDestMtx ) const {
	f32 f2X = 2.0f*x;
	f32 f2Y = 2.0f*y;
	f32 f2Z = 2.0f*z;
	f32 f2WX = f2X*w;
	f32 f2WY = f2Y*w;
	f32 f2WZ = f2Z*w;
	f32 f2XX = f2X*x;
	f32 f2XY = f2Y*x;
	f32 f2XZ = f2Z*x;
	f32 f2YY = f2Y*y;
	f32 f2YZ = f2Z*y;
	f32 f2ZZ = f2Z*z;

	rDestMtx.aa[0][0] = 1.0f - (f2YY + f2ZZ);
	rDestMtx.aa[0][1] = f2XY + f2WZ;
	rDestMtx.aa[0][2] = f2XZ - f2WY;
	rDestMtx.aa[1][0] = f2XY - f2WZ;
	rDestMtx.aa[1][1] = 1.0f - (f2XX + f2ZZ);
	rDestMtx.aa[1][2] = f2YZ + f2WX;
	rDestMtx.aa[2][0] = f2XZ + f2WY;
	rDestMtx.aa[2][1] = f2YZ - f2WX;
	rDestMtx.aa[2][2] = 1.0f - (f2XX + f2YY);
	rDestMtx.m_vPos = CFVec3A::m_Null;

	return rDestMtx;
}


FINLINE CFMtx43A &CFQuatA::BuildMtx( CFMtx43A &rDestMtx, const f32 &fScale ) const {
	f32 f2Scale = 2.0f * fScale;
	f32 f2ScaleX = f2Scale*x;
	f32 f2ScaleY = f2Scale*y;
	f32 f2ScaleZ = f2Scale*z;
	f32 f2ScaleWX = f2ScaleX*w;
	f32 f2ScaleWY = f2ScaleY*w;
	f32 f2ScaleWZ = f2ScaleZ*w;
	f32 f2ScaleXX = f2ScaleX*x;
	f32 f2ScaleXY = f2ScaleY*x;
	f32 f2ScaleXZ = f2ScaleZ*x;
	f32 f2ScaleYY = f2ScaleY*y;
	f32 f2ScaleYZ = f2ScaleZ*y;
	f32 f2ScaleZZ = f2ScaleZ*z;

	rDestMtx.aa[0][0] = fScale - (f2ScaleYY + f2ScaleZZ);
	rDestMtx.aa[0][1] = f2ScaleXY + f2ScaleWZ;
	rDestMtx.aa[0][2] = f2ScaleXZ - f2ScaleWY;
	rDestMtx.aa[1][0] = f2ScaleXY - f2ScaleWZ;
	rDestMtx.aa[1][1] = fScale - (f2ScaleXX + f2ScaleZZ);
	rDestMtx.aa[1][2] = f2ScaleYZ + f2ScaleWX;
	rDestMtx.aa[2][0] = f2ScaleXZ + f2ScaleWY;
	rDestMtx.aa[2][1] = f2ScaleYZ - f2ScaleWX;
	rDestMtx.aa[2][2] = fScale - (f2ScaleXX + f2ScaleYY);
	rDestMtx.m_vPos = CFVec3A::m_Null;

	return rDestMtx;
}


FINLINE CFMtx43A &CFQuatA::BuildMtx33( CFMtx43A &rDestMtx33 ) const {
	f32 f2X = 2.0f*x;
	f32 f2Y = 2.0f*y;
	f32 f2Z = 2.0f*z;
	f32 f2WX = f2X*w;
	f32 f2WY = f2Y*w;
	f32 f2WZ = f2Z*w;
	f32 f2XX = f2X*x;
	f32 f2XY = f2Y*x;
	f32 f2XZ = f2Z*x;
	f32 f2YY = f2Y*y;
	f32 f2YZ = f2Z*y;
	f32 f2ZZ = f2Z*z;

	rDestMtx33.aa[0][0] = 1.0f - (f2YY + f2ZZ);
	rDestMtx33.aa[0][1] = f2XY + f2WZ;
	rDestMtx33.aa[0][2] = f2XZ - f2WY;
	rDestMtx33.aa[1][0] = f2XY - f2WZ;
	rDestMtx33.aa[1][1] = 1.0f - (f2XX + f2ZZ);
	rDestMtx33.aa[1][2] = f2YZ + f2WX;
	rDestMtx33.aa[2][0] = f2XZ + f2WY;
	rDestMtx33.aa[2][1] = f2YZ - f2WX;
	rDestMtx33.aa[2][2] = 1.0f - (f2XX + f2YY);

	return rDestMtx33;
}


FINLINE CFVec3A &CFQuatA::BuildAxisX( CFVec3A &rDestVec ) const {
	rDestVec.a[0] = 1.0f - 2.0f * (a[1]*a[1] + a[2]*a[2]);
	rDestVec.a[1] = 2.0f * (a[0]*a[1] + a[3]*a[2]);
	rDestVec.a[2] = 2.0f * (a[0]*a[2] - a[3]*a[1]);

	return rDestVec;
}


FINLINE CFVec3A &CFQuatA::BuildAxisY( CFVec3A &rDestVec ) const {
	rDestVec.a[0] = 2.0f * (a[0]*a[1] - a[3]*a[2]);
	rDestVec.a[1] = 1.0f - 2.0f * (a[0]*a[0] + a[2]*a[2]);
	rDestVec.a[2] = 2.0f * (a[1]*a[2] + a[3]*a[0]);

	return rDestVec;
}


FINLINE CFVec3A &CFQuatA::BuildAxisZ( CFVec3A &rDestVec ) const {
	rDestVec.a[0] = 2.0f * (a[0]*a[2] + a[3]*a[1]);
	rDestVec.a[1] = 2.0f * (a[1]*a[2] - a[3]*a[0]);
	rDestVec.a[2] = 1.0f - 2.0f * (a[1]*a[1] + a[0]*a[0]);

	return rDestVec;
}


//--------------------------------------------------------------------
// CFTQuat Implementation:
//--------------------------------------------------------------------
FINLINE CFTQuatA::CFTQuatA( void ) {}
FINLINE CFTQuatA::CFTQuatA( const CFTQuatA &rTQ ) { Set( rTQ ); }
FINLINE CFTQuatA::CFTQuatA( const CFQuatA &rQ, const CFVec3A &rPos, const f32 &fScale ) { Set( rQ, rPos, fScale ); }
FINLINE CFTQuatA::CFTQuatA( const CFVec4A &rV, const CFVec3A &rPos, const f32 &fScale ) { Set( rV, rPos, fScale ); }

FINLINE CFTQuatA::CFTQuatA( const f32 &fQX, const f32 &fQY, const f32 &fQZ, const f32 &fQW, const f32 &fPX, const f32 &fPY, const f32 &fPZ, const f32 &fScale ) {
	Set( fQX, fQY, fQZ, fQW, fPX, fPY, fPZ, fScale );
}

FINLINE BOOL CFTQuatA::operator == ( const CFTQuatA &rQ ) const { return (m_PosScale == rQ.m_PosScale) && (v == rQ.v); }
FINLINE BOOL CFTQuatA::operator != ( const CFTQuatA &rQ ) const { return (m_PosScale != rQ.m_PosScale) || (v != rQ.v); }

FINLINE CFTQuatA &CFTQuatA::Zero( void ) { CFQuatA::Zero(); m_PosScale.Zero(); return *this; }
FINLINE CFTQuatA &CFTQuatA::Identity( void ) { CFQuatA::Identity(); m_PosScale=CFVec4A::m_UnitAxisW; return *this; }

FINLINE CFTQuatA &CFTQuatA::operator = ( const CFTQuatA &rTQ ) { v=rTQ.v; m_PosScale=rTQ.m_PosScale; return *this; }
FINLINE CFTQuatA &CFTQuatA::Set( const CFTQuatA &rTQ ) { v=rTQ.v; m_PosScale=rTQ.m_PosScale; return *this; }
FINLINE CFTQuatA &CFTQuatA::Set( const CFQuatA &rQ ) { v=rQ.v; m_PosScale=CFVec4A::m_UnitAxisW; return *this; }
FINLINE CFTQuatA &CFTQuatA::Set( const CFQuatA &rQ, const CFVec3A &rPos, const f32 &fScale ) { v=rQ.v; m_PosScale=rPos.v4a; m_fScale=fScale; return *this; }
FINLINE CFTQuatA &CFTQuatA::Set( const CFVec4A &rV, const CFVec3A &rPos, const f32 &fScale ) { v=rV; m_PosScale=rPos.v4a; m_fScale=fScale; return *this; }
FINLINE CFTQuatA &CFTQuatA::Set( const f32 &fQX, const f32 &fQY, const f32 &fQZ, const f32 &fQW, const f32 &fPX, const f32 &fPY, const f32 &fPZ, const f32 &fScale ) {
	v.Set( fQX, fQY, fQZ, fQW );
	m_Pos.Set( fPX, fPY, fPZ );
	m_fScale = fScale;

	return *this;
}

FINLINE CFTQuatA &CFTQuatA::BuildQuat( const CFMtx33 &rSrcMtx, const CFVec3A &rPos, BOOL bMtxHasNonUnitScale ) {
	m_Pos = rPos.v3;

	if( !bMtxHasNonUnitScale ) {
		m_fScale = 1.0f;
		CFQuatA::BuildQuat( rSrcMtx );
	} else {
		CFMtx33 UnitMtx;
		m_fScale = rSrcMtx.m_vRight.Mag();
		UnitMtx = rSrcMtx * fmath_Inv(m_fScale);
		CFQuatA::BuildQuat( UnitMtx );
	}

	return *this;
}

FINLINE CFTQuatA &CFTQuatA::BuildQuat( const CFMtx43 &rSrcMtx, BOOL bMtxHasNonUnitScale ) {
	m_Pos = rSrcMtx.m_vPos;

	if( !bMtxHasNonUnitScale ) {
		m_fScale = 1.0f;
		CFQuatA::BuildQuat( rSrcMtx );
	} else {
		CFMtx33 UnitMtx;
		m_fScale = rSrcMtx.m_vRight.Mag();
		UnitMtx = rSrcMtx.m33 * fmath_Inv(m_fScale);
		CFQuatA::BuildQuat( UnitMtx );
	}

	return *this;
}

FINLINE CFTQuatA &CFTQuatA::BuildQuat( const CFMtx44 &rSrcMtx, BOOL bMtxHasNonUnitScale ) {
	m_Pos = rSrcMtx.m_vPos;

	if( !bMtxHasNonUnitScale ) {
		m_fScale = 1.0f;
		CFQuatA::BuildQuat( rSrcMtx );
	} else {
		CFMtx44 UnitMtx;
		m_fScale = rSrcMtx.m_vRight.Mag();
		UnitMtx = rSrcMtx * fmath_Inv(m_fScale);
		CFQuatA::BuildQuat( UnitMtx );
	}

	return *this;
}

FINLINE CFTQuatA &CFTQuatA::BuildQuat( const CFMtx44A &rSrcMtx, BOOL bMtxHasNonUnitScale ) {
	m_Pos = rSrcMtx.m_vPos.v3;

	if( !bMtxHasNonUnitScale ) {
		m_fScale = 1.0f;
		CFQuatA::BuildQuat( rSrcMtx );
	} else {
		CFMtx44A UnitMtx;
		m_fScale = rSrcMtx.m_vRight.Mag();
		UnitMtx.Mul( rSrcMtx, fmath_Inv(m_fScale) );
		CFQuatA::BuildQuat( UnitMtx );
	}

	return *this;
}

FINLINE CFTQuatA &CFTQuatA::BuildQuat( const CFMtx43A &rSrcMtx, BOOL bMtxHasNonUnitScale ) {
	m_Pos = rSrcMtx.m_vPos.v3;

	if( !bMtxHasNonUnitScale ) {
		m_fScale = 1.0f;
		CFQuatA::BuildQuat( rSrcMtx );
	} else {
		CFMtx43A UnitMtx;
		m_fScale = rSrcMtx.m_vRight.Mag();
		UnitMtx.Mul( rSrcMtx, fmath_Inv(m_fScale) );
		CFQuatA::BuildQuat( UnitMtx );
	}

	return *this;
}

FINLINE CFTQuatA &CFTQuatA::BuildQuat( const CFVec3A &rUnitVecToRotateAbout, const f32 &fRadiansToRotateBy, const CFVec3A &rPos, const f32 &fScale ) {
	return BuildQuat( rUnitVecToRotateAbout.v4a, fRadiansToRotateBy, rPos, fScale );
}

FINLINE CFTQuatA &CFTQuatA::BuildQuat( const CFVec4A &rUnitVecToRotateAbout, const f32 &fRadiansToRotateBy, const CFVec3A &rPos, const f32 &fScale ) {
	CFQuatA::BuildQuat( rUnitVecToRotateAbout, fRadiansToRotateBy );
	m_Pos = rPos.v3;
	m_fScale = fScale;

	return *this;
}

FINLINE CFTQuatA &CFTQuatA::Mul( const CFTQuatA &rQ1, const CFTQuatA &rQ2 ) {
	// Original SSE asm (fdx8math_quat.inl): Q3 = Q1*Q2, p3 = s1*Q1*p2*Q1' + p1,
	// s3 = s1*s2 - a straight transform composition of rQ1 then rQ2. The
	// plain-C rewrite used stale *this as the left quaternion operand (a fresh
	// local collapses to a zero quat, poisoning combiner SUM) and stale *this
	// pos/scale for the translation.
	CFQuatA tQ;
	tQ.Mul( *((const CFQuatA *)&rQ1), *((const CFQuatA *)&rQ2) );

	CFVec3A tPos, tP2;
	tP2.v3 = rQ2.m_Pos;
	rQ1.MulPoint( tPos, tP2 );
	m_Pos = tPos.v3;
	m_fScale = rQ1.m_fScale * rQ2.m_fScale;
	v = tQ.v;

	return *this;
}


FINLINE CFTQuatA &CFTQuatA::Mul( const CFTQuatA &rQ ) {
	// Original SSE asm: p1 = s1*Q1*p2*Q1' + p1, s1 = s1*s2, Q1 = Q1*Q2 with
	// (Q1,p1,s1) = *this. Apply the old quat/scale to rQ's position first,
	// then update scale and quat.
	CFVec3A tPos, tP2;
	tP2.v3 = rQ.m_Pos;
	MulPoint( tPos, tP2 );
	m_Pos = tPos.v3;
	m_fScale = m_fScale * rQ.m_fScale;

	CFQuatA tQ;
	tQ.Mul( *((const CFQuatA *)this), *((const CFQuatA *)&rQ) );
	v = tQ.v;

	return *this;
}


FINLINE CFVec3A &CFTQuatA::MulPoint( CFVec3A &rRV, const CFVec3A &rV ) const {
	MulPoint( rRV.v4a, rV.v4a );
	return rRV;
}

FINLINE CFVec4A &CFTQuatA::MulPoint( CFVec4A &rRV, const CFVec4A &rV ) const {
	// Was hand-written SSE assembly computing 's1*Q1*p2*Q1' + p1', i.e. the
	// full transform (rotate by the quaternion part, scale, translate). The
	// asm applies the conjugate Q1' directly rather than dividing by
	// |Q|^2, so it assumes a unit-length quaternion; kept as-is here.
	CFQuatA qPoint, qConj, qResult;

	qPoint.v = rV;
	qPoint.a[3] = 0.0f;
	qConj = *((const CFQuatA *)this);
	qConj.v.v3 = qConj.v.v3 * -1.0f;

	qResult.Mul( *((const CFQuatA *)this), qPoint );
	qResult.Mul( qResult, qConj );

	CFVec4A vPos;
	vPos.x = m_Pos.x;
	vPos.y = m_Pos.y;
	vPos.z = m_Pos.z;
	vPos.w = 0.0f;
	CFVec4A rScaled;
	rScaled.Mul( qResult.v, m_fScale );
	rScaled.Add( vPos );
	rScaled.w = 0.0f;
	rRV = rScaled;


	return rRV;
}

FINLINE CFVec3A &CFTQuatA::MulPoint( CFVec3A &rV ) const {
	MulPoint( rV.v4a );
	return rV;
}

FINLINE CFVec4A &CFTQuatA::MulPoint( CFVec4A &rV ) const {
	// Was hand-written SSE assembly computing 's1*Q1*p2*Q1' + p1', i.e. the
	// full transform (rotate by the quaternion part, scale, translate). The
	// asm applies the conjugate Q1' directly rather than dividing by
	// |Q|^2, so it assumes a unit-length quaternion; kept as-is here.
	MulPoint( rV, rV );


	return rV;
}


FINLINE CFMtx43 &CFTQuatA::BuildMtx( CFMtx43 &rDestMtx ) const {
	f32 f2Scale = 2.0f * m_fScale;
	f32 f2ScaleX = f2Scale*x;
	f32 f2ScaleY = f2Scale*y;
	f32 f2ScaleZ = f2Scale*z;
	f32 f2ScaleWX = f2ScaleX*w;
	f32 f2ScaleWY = f2ScaleY*w;
	f32 f2ScaleWZ = f2ScaleZ*w;
	f32 f2ScaleXX = f2ScaleX*x;
	f32 f2ScaleXY = f2ScaleY*x;
	f32 f2ScaleXZ = f2ScaleZ*x;
	f32 f2ScaleYY = f2ScaleY*y;
	f32 f2ScaleYZ = f2ScaleZ*y;
	f32 f2ScaleZZ = f2ScaleZ*z;

	rDestMtx.aa[0][0] = m_fScale - (f2ScaleYY + f2ScaleZZ);
	rDestMtx.aa[0][1] = f2ScaleXY + f2ScaleWZ;
	rDestMtx.aa[0][2] = f2ScaleXZ - f2ScaleWY;
	rDestMtx.aa[1][0] = f2ScaleXY - f2ScaleWZ;
	rDestMtx.aa[1][1] = m_fScale - (f2ScaleXX + f2ScaleZZ);
	rDestMtx.aa[1][2] = f2ScaleYZ + f2ScaleWX;
	rDestMtx.aa[2][0] = f2ScaleXZ + f2ScaleWY;
	rDestMtx.aa[2][1] = f2ScaleYZ - f2ScaleWX;
	rDestMtx.aa[2][2] = m_fScale - (f2ScaleXX + f2ScaleYY);

	rDestMtx.m_vPos = m_Pos;

	return rDestMtx;
}

FINLINE CFMtx44 &CFTQuatA::BuildMtx( CFMtx44 &rDestMtx ) const {
	f32 f2Scale = 2.0f * m_fScale;
	f32 f2ScaleX = f2Scale*x;
	f32 f2ScaleY = f2Scale*y;
	f32 f2ScaleZ = f2Scale*z;
	f32 f2ScaleWX = f2ScaleX*w;
	f32 f2ScaleWY = f2ScaleY*w;
	f32 f2ScaleWZ = f2ScaleZ*w;
	f32 f2ScaleXX = f2ScaleX*x;
	f32 f2ScaleXY = f2ScaleY*x;
	f32 f2ScaleXZ = f2ScaleZ*x;
	f32 f2ScaleYY = f2ScaleY*y;
	f32 f2ScaleYZ = f2ScaleZ*y;
	f32 f2ScaleZZ = f2ScaleZ*z;

	rDestMtx.aa[0][0] = m_fScale - (f2ScaleYY + f2ScaleZZ);
	rDestMtx.aa[0][1] = f2ScaleXY + f2ScaleWZ;
	rDestMtx.aa[0][2] = f2ScaleXZ - f2ScaleWY;
	rDestMtx.aa[0][3] = 0.0f;
	rDestMtx.aa[1][0] = f2ScaleXY - f2ScaleWZ;
	rDestMtx.aa[1][1] = m_fScale - (f2ScaleXX + f2ScaleZZ);
	rDestMtx.aa[1][2] = f2ScaleYZ + f2ScaleWX;
	rDestMtx.aa[1][3] = 0.0f;
	rDestMtx.aa[2][0] = f2ScaleXZ + f2ScaleWY;
	rDestMtx.aa[2][1] = f2ScaleYZ - f2ScaleWX;
	rDestMtx.aa[2][2] = m_fScale - (f2ScaleXX + f2ScaleYY);
	rDestMtx.aa[2][3] = 0.0f;

	rDestMtx.m_vPos = m_Pos;
	rDestMtx.aa[3][3] = 1.0f;

	return rDestMtx;
}

FINLINE CFMtx44A &CFTQuatA::BuildMtx( CFMtx44A &rDestMtx ) const {
	f32 f2Scale = 2.0f * m_fScale;
	f32 f2ScaleX = f2Scale*x;
	f32 f2ScaleY = f2Scale*y;
	f32 f2ScaleZ = f2Scale*z;
	f32 f2ScaleWX = f2ScaleX*w;
	f32 f2ScaleWY = f2ScaleY*w;
	f32 f2ScaleWZ = f2ScaleZ*w;
	f32 f2ScaleXX = f2ScaleX*x;
	f32 f2ScaleXY = f2ScaleY*x;
	f32 f2ScaleXZ = f2ScaleZ*x;
	f32 f2ScaleYY = f2ScaleY*y;
	f32 f2ScaleYZ = f2ScaleZ*y;
	f32 f2ScaleZZ = f2ScaleZ*z;

	rDestMtx.aa[0][0] = m_fScale - (f2ScaleYY + f2ScaleZZ);
	rDestMtx.aa[0][1] = f2ScaleXY + f2ScaleWZ;
	rDestMtx.aa[0][2] = f2ScaleXZ - f2ScaleWY;
	rDestMtx.aa[0][3] = 0.0f;
	rDestMtx.aa[1][0] = f2ScaleXY - f2ScaleWZ;
	rDestMtx.aa[1][1] = m_fScale - (f2ScaleXX + f2ScaleZZ);
	rDestMtx.aa[1][2] = f2ScaleYZ + f2ScaleWX;
	rDestMtx.aa[1][3] = 0.0f;
	rDestMtx.aa[2][0] = f2ScaleXZ + f2ScaleWY;
	rDestMtx.aa[2][1] = f2ScaleYZ - f2ScaleWX;
	rDestMtx.aa[2][2] = m_fScale - (f2ScaleXX + f2ScaleYY);
	rDestMtx.aa[2][3] = 0.0f;

	rDestMtx.m_vPos = m_PosScale;

	return rDestMtx;
}

FINLINE CFMtx43A &CFTQuatA::BuildMtx( CFMtx43A &rDestMtx ) const {
	f32 f2Scale = 2.0f * m_fScale;
	f32 f2ScaleX = f2Scale*x;
	f32 f2ScaleY = f2Scale*y;
	f32 f2ScaleZ = f2Scale*z;
	f32 f2ScaleWX = f2ScaleX*w;
	f32 f2ScaleWY = f2ScaleY*w;
	f32 f2ScaleWZ = f2ScaleZ*w;
	f32 f2ScaleXX = f2ScaleX*x;
	f32 f2ScaleXY = f2ScaleY*x;
	f32 f2ScaleXZ = f2ScaleZ*x;
	f32 f2ScaleYY = f2ScaleY*y;
	f32 f2ScaleYZ = f2ScaleZ*y;
	f32 f2ScaleZZ = f2ScaleZ*z;

	rDestMtx.aa[0][0] = m_fScale - (f2ScaleYY + f2ScaleZZ);
	rDestMtx.aa[0][1] = f2ScaleXY + f2ScaleWZ;
	rDestMtx.aa[0][2] = f2ScaleXZ - f2ScaleWY;
	rDestMtx.aa[1][0] = f2ScaleXY - f2ScaleWZ;
	rDestMtx.aa[1][1] = m_fScale - (f2ScaleXX + f2ScaleZZ);
	rDestMtx.aa[1][2] = f2ScaleYZ + f2ScaleWX;
	rDestMtx.aa[2][0] = f2ScaleXZ + f2ScaleWY;
	rDestMtx.aa[2][1] = f2ScaleYZ - f2ScaleWX;
	rDestMtx.aa[2][2] = m_fScale - (f2ScaleXX + f2ScaleYY);

	rDestMtx.m_vPos.v3 = m_PosScale.v3;

	return rDestMtx;
}

FINLINE CFTQuatA &CFTQuatA::ReceiveSlerpOf( const f32 &fUnitSlerp, const CFTQuatA &rQuat0, const CFTQuatA &rQuat1 ) {
	CFQuatA::ReceiveSlerpOf( fUnitSlerp, rQuat0, rQuat1 );
	m_Pos.ReceiveLerpOf( fUnitSlerp, rQuat0.m_Pos, rQuat1.m_Pos );
	m_fScale = FMATH_FPOT( fUnitSlerp, rQuat0.m_fScale, rQuat1.m_fScale );

	return *this;
}

FINLINE CFTQuatA &CFTQuatA::ReceiveSlerpOf( const f32 &fUnitSlerp0, const f32 &fUnitSlerp1, const CFTQuatA &rQuat0, const CFTQuatA &rQuat1 ) {
	CFQuatA::ReceiveSlerpOf( fUnitSlerp0, fUnitSlerp1, rQuat0, rQuat1 );

	m_Pos.x = fUnitSlerp0*rQuat0.m_Pos.x + fUnitSlerp1*rQuat1.m_Pos.x;
	m_Pos.y = fUnitSlerp0*rQuat0.m_Pos.y + fUnitSlerp1*rQuat1.m_Pos.y;
	m_Pos.z = fUnitSlerp0*rQuat0.m_Pos.z + fUnitSlerp1*rQuat1.m_Pos.z;

	m_fScale = fUnitSlerp0*rQuat0.m_fScale + fUnitSlerp1*rQuat1.m_fScale;

	return *this;
}

#endif	// FANG_WINGC

