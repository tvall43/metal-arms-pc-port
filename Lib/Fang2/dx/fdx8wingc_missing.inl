// fdx8wingc_missing.inl
//
// Fang's WinGC configuration (_FANGDEF_WINGC) is the GameCube-alignment build
// the original team used for tools. It is not a complete game configuration:
// a number of CFVec3A/CFVec4A/CFMtx43A/CFQuatA members are declared in the
// math headers but their definitions sit inside the '#if !FANG_WINGC' region
// of the platform .inl files, so a _FANGDEF_WINGC build compiles but cannot
// link.
//
// These are that set. Every definition below was moved verbatim out of the
// '!FANG_WINGC' block; none of them contain inline assembly, so they are
// plain scalar C++ and are equally valid in either configuration.
//
// The four genuinely asm-backed members (CFMtx43A::GenKey, CFMtx43A::RevMul,
// CFMtx43A::RevMul33, CFMtx44A::GenKey) are handled in fdx8wingc_asm.inl.

#if FANG_WINGC
FINLINE CFQuatA &CFQuatA::BuildQuatRotX( const f32 &fRadiansToRotateBy ) {
	f32 fSin;

	fmath_SinCos( 0.5f*fRadiansToRotateBy, &fSin, &w );

	x = fSin;
	y = 0.0f;
	z = 0.0f;

	return *this;
}

FINLINE CFQuatA &CFQuatA::BuildQuatRotY( const f32 &fRadiansToRotateBy ) {
	f32 fSin;

	fmath_SinCos( 0.5f*fRadiansToRotateBy, &fSin, &w );

	x = 0.0f;
	y = fSin;
	z = 0.0f;

	return *this;
}

FINLINE CFQuatA &CFQuatA::BuildQuatRotZ( const f32 &fRadiansToRotateBy ) {
	f32 fSin;

	fmath_SinCos( 0.5f*fRadiansToRotateBy, &fSin, &w );

	x = 0.0f;
	y = 0.0f;
	z = fSin;

	return *this;
}

FINLINE CFVec3A &CFVec3A::CrossVecWithY( const CFVec3A &rV ) {
	x = -rV.z;
	y = 0.0f;
	z = rV.x;

	return *this;
}

// tests vector to see if it is within pitch and yaw range of center and up vectors.
// vCenter = normalized "origin" look vector
// vUp = normalized "origin" up vector
// fYawCos = cosine of yaw range to either side of vCenter
// fPitchCos = cosine of pitch range to either side of vUp
// returns TRUE if in range, FALSE otherwise.
FINLINE BOOL CFVec3A::InYawPitchRange( const CFVec3A &vCenter, const CFVec3A &vUp, const f32 fYawCos, const f32 fPitchCos ) {
	CFVec3A vProjected;

	vProjected.ReceivePlanarProjection( *this, vUp );

	// check yaw constraint
	if( vCenter.Dot( vProjected ) < fYawCos )
	{
		return FALSE;
	}

	// check pitch constraint
	if( Dot( vProjected ) < fPitchCos )
	{
		return FALSE;
	}

	return TRUE;
}

FINLINE f32 CFVec3A::InvDist( const CFVec3A &rV ) const {
	return m_TempVec.Sub( rV, *this ).InvMag();
}

FINLINE f32 CFVec3A::InvDistSq( const CFVec3A &rV ) const {
	return m_TempVec.Sub( rV, *this ).InvMagSq();
}

// project this vector onto plane defined by unit vector rvNormal
FINLINE CFVec3A &CFVec3A::PlanarProjection( const CFVec3A &rvNormal ) {
	CFVec3A vTemp;

	// this = this - (rvNormal * (rvNormal dot this));
	vTemp = rvNormal;
	vTemp.Mul( rvNormal.Dot( *this ) );
	Sub( vTemp );

	return *this;
}

// project vector rV onto plane defined by unit vector rvNormal
FINLINE CFVec3A &CFVec3A::ReceivePlanarProjection( const CFVec3A &rV, const CFVec3A &rvNormal ) {
	// this = rV - (rvNormal * (rvNormal dot rV));
	Set( rvNormal );
	Mul( rvNormal.Dot( rV ) );
	Negate();
	Add( rV );

	return *this;
}

// See Reflect(), above.
FINLINE CFVec3A &CFVec3A::ReceiveReflection( const CFVec3A &rV, const CFVec3A &rNormal ) {
	Mul( rNormal, -2.0f * rV.Dot(rNormal) ).Add( rV );
	return *this;
}

// a ray intersects a poly with a normal of rNormal, 
// you can Reflect the normalized ray direction vector using 
// this function and the poly normal as the reflection normal.
// The result is the new normalized direction vector for the 
// ray reflection, and is pointing the opposite direction as
// the original ray. This function will work with non-normalized 
// vectors, as well.
FINLINE CFVec3A &CFVec3A::Reflect( const CFVec3A &rNormal ) {
	CFVec3A vTemp;
	vTemp.Mul( rNormal, -2.0f * Dot(rNormal) );
	Add( vTemp );
	return *this;
}

FINLINE CFVec3A &CFVec3A::UnitCrossVecWithY( const CFVec3A &rV ) {
	return CrossVecWithY( rV ).UnitizeXZ();
}

FINLINE CFVec3A &CFVec3A::UnitCrossYWithVec( const CFVec3A &rV ) {
	return CrossYWithVec( rV ).UnitizeXZ();
}

FINLINE f32 CFVec4A::DistSqXZ( const CFVec4A &rV ) const {
	return m_TempVec.Sub( rV, *this ).MagSqXZ();
}

FINLINE f32 CFVec4A::DistXZ( const CFVec4A &rV ) const {
	return m_TempVec.Sub( rV, *this ).MagXZ();
}
// The DX version of this is written against CFVec4A::_m, which only exists when
// FANG_WINGC is off. Copying the CFVec3A's CFVec4A alias is equivalent and also
// zeroes W, which the original comment requires.
FINLINE CFQuatA &CFQuatA::Set( const CFVec3A &rV ) { v = rV.v4a; return *this; }


// CFMtx43A::RevMul. fdx8math_mtx.inl is entirely inside '#if !FANG_WINGC', so
// with FANG_WINGC on this method -- and every other one in that file -- has no
// body at all. eproj_cleaner.cpp calls it, hence the undefined reference.
//
// The MSVC asm decides operand order by which register 'this' and 'rM' land in:
//   Mul     : mov edx,this / mov ecx,rM   -> result = rM * this
//   RevMul  : mov ecx,this / mov edx,rM   -> result = this * rM
// which for 'A.Mul(B)' means A.Mul(B) = B*A and A.RevMul(B) = A*B. So RevMul is
// the plain product in the source order.
//
// aa[4][4] is the same storage the asm addresses ([ecx+0x00] is aa[0][0],
// [ecx+0x10] is aa[1][0], ...). 'this' is copied first because callers do pass
// the same matrix as both arguments.
FINLINE CFMtx43A &CFMtx43A::RevMul( const CFMtx43A &rM )
{
	const CFMtx43A a = *this;
	for ( int i = 0; i < 4; i++ ) {
		for ( int j = 0; j < 4; j++ ) {
			aa[i][j] = a.aa[i][0]*rM.aa[0][j] + a.aa[i][1]*rM.aa[1][j] + a.aa[i][2]*rM.aa[2][j];
		}
	}
	// retail RevMul adds rM's row3 unconditionally (affine); scaling it by
	// a.aa[3][3]==0 like a full 4x4 product would drop the translation.
	for ( int j = 0; j < 4; j++ ) {
		aa[3][j] += rM.aa[3][j];
	}
	return *this;
}

#endif // FANG_WINGC
