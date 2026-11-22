//////////////////////////////////////////////////////////////////////////////////////
// weapon_laser.cpp - Mining Laser weapon.
//
// Author: Justin Link
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
// 06/19/02 Link		Created.
//////////////////////////////////////////////////////////////////////////////////////

#include "fang.h"
#include "weapon_laser.h"
#include "fgamedata.h"
#include "fworld.h"
#include "fmesh.h"
#include "fanim.h"
#include "fresload.h"
#include "MeshTypes.h"
#include "floop.h"
#include "bot.h"
#include "fforce.h"
#include "player.h"
#include "explosion.h"
#include "potmark.h"
#include "fcamera.h"
#include "ItemInst.h"
#include "LaserBeam.h"
#include "fsndfx.h"
#include "fsound.h"
#include "Ai/AIEnviro.h"
#include "eshield.h"
#include "Damage.h"
#include "gcoll.h"
#include "fdecal.h"
#include "fdebris.h"
#include "LightPool.h"

#include "sas_user.h"


#define _USER_PROP_FILENAME		"w_laser.csv"


static const u32 _uFizzleThreshhold = 25;

// TracerDef_t::nUser tagging for the laser. The low nibble remains the
// ricochet counter used by the R156 callback; high bits identify a charged
// burst and its retail damage tier.
#define _LASER_NUSER_BURST			0x8000
#define _LASER_NUSER_TIER_SHIFT		8
#define _LASER_NUSER_LEVEL_SHIFT	10
#define _LASER_NUSER_RICOCHET_MASK	0x000f


//**********************************************************************************************************************************
//**********************************************************************************************************************************
//
// CWeaponLaserBuilder
//
//**********************************************************************************************************************************
//**********************************************************************************************************************************

static CWeaponLaserBuilder _WeaponLaserBuilder;


void CWeaponLaserBuilder::SetDefaults( u64 nEntityTypeBits, u64 nEntityLeafTypeBit, cchar *pszEntityType ) {
	ENTITY_BUILDER_SET_PARENT_CLASS_DEFAULTS( CWeaponBuilder, ENTITY_BIT_WEAPONLASER, pszEntityType );
}


BOOL CWeaponLaserBuilder::PostInterpretFixup( void ) {
	return CWeaponBuilder::PostInterpretFixup();
}


BOOL CWeaponLaserBuilder::InterpretTable( void ) {
	return CWeaponBuilder::InterpretTable();
}




//**********************************************************************************************************************************
//**********************************************************************************************************************************
//
// CWeaponLaser
//
//**********************************************************************************************************************************
//**********************************************************************************************************************************

CWeaponLaser::_UserProps_t CWeaponLaser::m_aUserProps[EUK_COUNT_LASER];
CDamageProfile *CWeaponLaser::m_apDamageProfile[EUK_COUNT_LASER];
CFSoundGroup *CWeaponLaser::m_pSoundGroupImpactEnergy = NULL;
TracerDef_t CWeaponLaser::m_aoTracerDef[EUK_COUNT_LASER];
TracerDef_t CWeaponLaser::m_aoBurstTracerDef[EUK_COUNT_LASER];
CFTexInst CWeaponLaser::m_aTracerTexInst[EUK_COUNT_LASER];

// Retail tracer colour: w_laser.csv fields 3..6 (RGBA), per upgrade level.
// The leak hardcoded white, which is why the laser tracer rendered grey.
// Defaults are the retail values; InitSystem refreshes them from the CSV.
static f32 _aRetailTracerColor[CWeapon::EUK_COUNT_LASER][4] = {
	{ 0.25f, 1.00f, 0.25f, 1.0f },	// LaserL1  (green)
	{ 1.00f, 0.75f, 0.25f, 1.0f },	// LaserL2  (orange)
	{ 1.00f, 0.25f, 0.25f, 1.0f },	// LaserL3  (red)
	{ 0.25f, 0.25f, 1.00f, 1.0f },	// LaserMil (blue)
	{ 0.25f, 0.25f, 1.00f, 1.0f },	// LaserMil_Possessed
};


// This table describes to fgamedata how our user property table is to be interpreted:
const FGameData_TableEntry_t CWeaponLaser::m_aUserPropVocab[] = {
	// apszMeshName[_MESH_WEAPON]:
	FGAMEDATA_VAR_TYPE_STRING|
	FGAMEDATA_FLAGS_STRING_PTR_TO_MAIN_STR_TBL,
	sizeof( char * ),
	F32_DATATABLE_0,
	F32_DATATABLE_0,

	// fClipAmmoMax:
	FGAMEDATA_VAR_TYPE_FLOAT|
	FGAMEDATA_FLAGS_FLOAT_X | FGAMEDATA_FLAGS_FLOAT_CLAMP_AND_GO,
	sizeof( f32 ),
	F32_DATATABLE_Neg1,
	F32_DATATABLE_65534,

	// fReserveAmmoMax:
	FGAMEDATA_VAR_TYPE_FLOAT|
	FGAMEDATA_FLAGS_FLOAT_X | FGAMEDATA_FLAGS_FLOAT_CLAMP_AND_GO,
	sizeof( f32 ),
	F32_DATATABLE_Neg1,
	F32_DATATABLE_65534,

	// fRoundsPerSec:
	// fOORoundsPerSec:
	FGAMEDATA_VAR_TYPE_FLOAT|
	FGAMEDATA_FLAGS_FLOAT_X | FGAMEDATA_FLAGS_FLOAT_CLAMP_AND_GO | FGAMEDATA_FLAGS_FLOAT_OO_X,
	sizeof( f32 ) * 2,
	F32_DATATABLE_Pt001,
	F32_DATATABLE_1000,

	// fWeaponCullDist:
	FGAMEDATA_VAR_TYPE_FLOAT|
	FGAMEDATA_FLAGS_FLOAT_X | FGAMEDATA_FLAGS_FLOAT_CLAMP_AND_GO,
	sizeof( f32 ),
	F32_DATATABLE_0,
	F32_DATATABLE_100000,

	// fRocketsInPoolCount:
	FGAMEDATA_VAR_TYPE_FLOAT|
	FGAMEDATA_FLAGS_FLOAT_X | FGAMEDATA_FLAGS_FLOAT_CLAMP_AND_GO,
	sizeof( f32 ),
	F32_DATATABLE_1,
	F32_DATATABLE_1000,

	// fMaxLiveRange:
	FGAMEDATA_VAR_TYPE_FLOAT|
	FGAMEDATA_FLAGS_FLOAT_X | FGAMEDATA_FLAGS_FLOAT_CLAMP_AND_GO,
	sizeof( f32 ),
	F32_DATATABLE_1,
	F32_DATATABLE_100000,

	// fUnitRecoil:
	FGAMEDATA_VAR_TYPE_FLOAT|
	FGAMEDATA_FLAGS_FLOAT_X | FGAMEDATA_FLAGS_FLOAT_CLAMP_AND_GO,
	sizeof( f32 ),
	F32_DATATABLE_0,
	F32_DATATABLE_1,

	// fRocketCullDist:
	FGAMEDATA_VAR_TYPE_FLOAT|
	FGAMEDATA_FLAGS_FLOAT_X | FGAMEDATA_FLAGS_FLOAT_CLAMP_AND_GO,
	sizeof( f32 ),
	F32_DATATABLE_0,
	F32_DATATABLE_100000,

	// fRechargeRate:
	// fOORechargeRate:
	FGAMEDATA_VAR_TYPE_FLOAT|
	FGAMEDATA_FLAGS_FLOAT_X | FGAMEDATA_FLAGS_FLOAT_CLAMP_AND_GO | FGAMEDATA_FLAGS_FLOAT_OO_X,
	sizeof( f32 ) * 2,
	F32_DATATABLE_0,
	F32_DATATABLE_100000,

	// fRocketSpeed:
	FGAMEDATA_VAR_TYPE_FLOAT|
	FGAMEDATA_FLAGS_FLOAT_X | FGAMEDATA_FLAGS_FLOAT_CLAMP_AND_GO,
	sizeof( f32 ),
	F32_DATATABLE_1,
	F32_DATATABLE_100000,

	// fTracerLength
	FGAMEDATA_VAR_TYPE_FLOAT|
	FGAMEDATA_FLAGS_FLOAT_X | FGAMEDATA_FLAGS_FLOAT_CLAMP_AND_GO,
	sizeof( f32 ),
	F32_DATATABLE_0,
	F32_DATATABLE_500,

	// fTracerWidth
	FGAMEDATA_VAR_TYPE_FLOAT|
	FGAMEDATA_FLAGS_FLOAT_X | FGAMEDATA_FLAGS_FLOAT_CLAMP_AND_GO,
	sizeof( f32 ),
	F32_DATATABLE_0,
	F32_DATATABLE_10,

	// fDistFromWeaponOrigToMuzzle:
	FGAMEDATA_VAR_TYPE_FLOAT|
	FGAMEDATA_FLAGS_FLOAT_X | FGAMEDATA_FLAGS_FLOAT_CLAMP_AND_GO,
	sizeof( f32 ),
	F32_DATATABLE_1,
	F32_DATATABLE_100000,

	// fMinTargetAssistDist:
	FGAMEDATA_VAR_TYPE_FLOAT|
	FGAMEDATA_FLAGS_FLOAT_X | FGAMEDATA_FLAGS_FLOAT_CLAMP_AND_GO,
	sizeof( f32 ),
	F32_DATATABLE_0,
	F32_DATATABLE_100000,

	// fMaxTargetAssistDist:
	FGAMEDATA_VAR_TYPE_FLOAT|
	FGAMEDATA_FLAGS_FLOAT_X | FGAMEDATA_FLAGS_FLOAT_CLAMP_AND_GO,
	sizeof( f32 ),
	F32_DATATABLE_0,
	F32_DATATABLE_100000,

	// pszL12_TexName
	FGAMEDATA_VAR_TYPE_STRING|
	FGAMEDATA_FLAGS_STRING_PTR_TO_MAIN_STR_TBL,
	sizeof( char * ),
	F32_DATATABLE_0,
	F32_DATATABLE_0,

	// pszMuzzleTex
	FGAMEDATA_VAR_TYPE_STRING|
	FGAMEDATA_FLAGS_STRING_PTR_TO_MAIN_STR_TBL,
	sizeof( char * ),
	F32_DATATABLE_0,
	F32_DATATABLE_0,

	// fMuzzleWidth
	FGAMEDATA_VAR_TYPE_FLOAT|
	FGAMEDATA_FLAGS_FLOAT_X | FGAMEDATA_FLAGS_FLOAT_CLAMP_AND_GO,
	sizeof( f32 ),
	F32_DATATABLE_0,
	F32_DATATABLE_100000,

	// fMuzzleHeight
	FGAMEDATA_VAR_TYPE_FLOAT|
	FGAMEDATA_FLAGS_FLOAT_X | FGAMEDATA_FLAGS_FLOAT_CLAMP_AND_GO,
	sizeof( f32 ),
	F32_DATATABLE_0,
	F32_DATATABLE_100000,

	// pszDamageProfile:
	FGAMEDATA_VAR_TYPE_STRING|
	FGAMEDATA_FLAGS_STRING_PTR_TO_MAIN_STR_TBL,
	sizeof( char * ),
	F32_DATATABLE_0,
	F32_DATATABLE_0,

	FGAMEDATA_VOCAB_DAMAGE,		// charged tier 0
	FGAMEDATA_VOCAB_DAMAGE,		// charged tier 1
	FGAMEDATA_VOCAB_DAMAGE,		// charged tier 2

	// PC port: retail's vocab flags for this field are 0x81000 =
	// STRING_TO_SOUND_GROUP|NONE_TO_NULL - w_laser.csv field 65 holds the
	// sound-group name 'LaserFire' (defined in snd_wpn.csv over SFX
	// 'Fire Laser EUK1'), which never resolves as a raw bank SFX.
	FGAMEDATA_VOCAB_SOUND_GROUP,	// pSoundGroupFire ('LaserFire')
	FGAMEDATA_VOCAB_SOUND_GROUP,	// pSoundGroupCharge ('LaserCharge')
	FGAMEDATA_VOCAB_SOUND_GROUP,	// pSoundGroupBurst1 ('LaserBurst1')
	FGAMEDATA_VOCAB_SOUND_GROUP,	// pSoundGroupBurst2 ('LaserBurst2')
	FGAMEDATA_VOCAB_SOUND_GROUP,	// pSoundGroupBurstBeep ('LaserBrstBeep')

	FGAMEDATA_VOCAB_DECAL_DEF,		// hDecalDef

	FGAMEDATA_VOCAB_PARTICLE,		// hRetailChargeParticle ('Laser07')
	FGAMEDATA_VOCAB_PARTICLE,		// hRetailBurstParticle ('Laser05a')
	FGAMEDATA_VOCAB_PARTICLE,		// hRetailEndParticle ('Laser06a')

	// L2 charge/burst timings, energy and burst tracer shape (retail
	// columns 35-37, 39-40, 22-24). These are per-EUK values.
	FGAMEDATA_VAR_TYPE_FLOAT|
	FGAMEDATA_FLAGS_FLOAT_X | FGAMEDATA_FLAGS_FLOAT_CLAMP_AND_GO,
	sizeof( f32 ), F32_DATATABLE_0, F32_DATATABLE_1000,	// fRetailBurstCooldown

	FGAMEDATA_VAR_TYPE_FLOAT|
	FGAMEDATA_FLAGS_FLOAT_X | FGAMEDATA_FLAGS_FLOAT_CLAMP_AND_GO,
	sizeof( f32 ), F32_DATATABLE_0, F32_DATATABLE_1000,	// fRetailChargeDelay

	FGAMEDATA_VAR_TYPE_FLOAT|
	FGAMEDATA_FLAGS_FLOAT_X | FGAMEDATA_FLAGS_FLOAT_CLAMP_AND_GO,
	sizeof( f32 ), F32_DATATABLE_Pt001, F32_DATATABLE_1000,	// fRetailChargeTime

	FGAMEDATA_VAR_TYPE_FLOAT|
	FGAMEDATA_FLAGS_FLOAT_X | FGAMEDATA_FLAGS_FLOAT_CLAMP_AND_GO,
	sizeof( f32 ), F32_DATATABLE_0, F32_DATATABLE_1,	// fRetailReadyEnergy

	FGAMEDATA_VAR_TYPE_FLOAT|
	FGAMEDATA_FLAGS_FLOAT_X | FGAMEDATA_FLAGS_FLOAT_CLAMP_AND_GO,
	sizeof( f32 ), F32_DATATABLE_0, F32_DATATABLE_1,	// fRetailEnergyAfterBurst

	FGAMEDATA_VAR_TYPE_FLOAT|
	FGAMEDATA_FLAGS_FLOAT_X | FGAMEDATA_FLAGS_FLOAT_CLAMP_AND_GO,
	sizeof( f32 ), F32_DATATABLE_1, F32_DATATABLE_100000,	// fRetailBurstSpeed

	FGAMEDATA_VAR_TYPE_FLOAT|
	FGAMEDATA_FLAGS_FLOAT_X | FGAMEDATA_FLAGS_FLOAT_CLAMP_AND_GO,
	sizeof( f32 ), F32_DATATABLE_0, F32_DATATABLE_100000,	// fRetailBurstLength

	FGAMEDATA_VAR_TYPE_FLOAT|
	FGAMEDATA_FLAGS_FLOAT_X | FGAMEDATA_FLAGS_FLOAT_CLAMP_AND_GO,
	sizeof( f32 ), F32_DATATABLE_0, F32_DATATABLE_100000,	// fRetailBurstWidth

	// Retail charged-burst impact package:
	FGAMEDATA_VOCAB_PARTICLE,		// hRetailBurstAttachParticle1 ('Laser04')
	FGAMEDATA_VOCAB_PARTICLE,		// hRetailFiringSmokeParticle ('LsrSmoke3'/'LsrSmoke1')
	FGAMEDATA_VOCAB_PARTICLE,		// hRetailBurstAttachParticle2 ('LsrSmoke4'/'LsrSmoke2')
	FGAMEDATA_VOCAB_EXPLODE_GROUP,	// hRetailBurstHitExplosion ('LsrBrstHit')
	FGAMEDATA_VOCAB_EXPLODE_GROUP,	// hRetailBurstRicExplosion ('LsrBrstRic')
	FGAMEDATA_VOCAB_SOUND_GROUP,	// pSoundGroupRic ('LaserRic')
	FGAMEDATA_VOCAB_DECAL_DEF,		// hRetailBurstDecal ('LaserBurstL2'/'LaserBurstL3')

	// End of table:
	FGAMEDATA_VAR_TYPE_COUNT| 0, 0, F32_DATATABLE_0, F32_DATATABLE_0
};


static const u8 _aUserPropFieldMap[] = {
	0, 7, 23, 8, 43, 56, 13, 44, 15, 26, 10, 11, 12, 34, 46, 14,
	2, 1, 20, 21, 59, 60, 61, 62, 65, 66, 67, 68, 70, 71,
	48, 49, 50, 33, 34, 35, 37, 39, 20, 21, 22,
	47, 51, 52, 63, 64, 69, 72,
};

const FGameDataMap_t CWeaponLaser::m_aUserPropMapTable[] = {
	"LaserL1",
	m_aUserPropVocab,
	sizeof(m_aUserProps),
	(void *)&m_aUserProps[0],

	"LaserL2",
	m_aUserPropVocab,
	sizeof(m_aUserProps),
	(void *)&m_aUserProps[1],

	"LaserL3",
	m_aUserPropVocab,
	sizeof(m_aUserProps),
	(void *)&m_aUserProps[2],

	"LaserMil",
	m_aUserPropVocab,
	sizeof(m_aUserProps),
	(void *)&m_aUserProps[3],

	"LaserMil_Possessed",
	m_aUserPropVocab,
	sizeof(m_aUserProps),
	(void *)&m_aUserProps[4],

	NULL
};



// [ma] Round-18: laser tracers never skipped the owner, so their muzzle-started rays
// collided with our own gun/arm mesh (self-damage; beams absorbing at the gun before
// ever reaching anything). Mirror CWeaponRivet::_BuildTracerSkipListCallback.
static void _LaserTracerBuildSkipListCallback( void *pUser ) {
	CWeaponLaser *pLaser = (CWeaponLaser *)pUser;
	if( pLaser ) {
		if( pLaser->GetOwner() ) {
			pLaser->GetOwner()->AppendTrackerSkipList();
		} else {
			pLaser->AppendTrackerSkipList();
		}
	}
}

BOOL CWeaponLaser::InitSystem( void ) {
	Info_t *pInfo;
	u32 i;
	FTexDef_t *pTexDef;

	FResFrame_t ResFrame = fres_GetFrame();

	fang_MemZero( m_aUserProps, sizeof(m_aUserProps) );

	// Read the user properties for all EUK levels of this weapon...
	if( !fgamedata_ReadFileUsingMap( m_aUserPropMapTable, _USER_PROP_FILENAME, _aUserPropFieldMap ) ) {
		DEVPRINTF( "CWeaponLaser::InitSystem(): Could not read user properties from file '%s'.\n", _USER_PROP_FILENAME );
		goto _ExitWithError;
		return FALSE;
	}

	// Retail tracer colour lives in fields 3..6 of each laser table. Read it
	// explicitly: the source's 25-field vocab has no slot for it.
	{
		static const FGameData_TableEntry_t RetailFloat = {
			FGAMEDATA_VAR_TYPE_FLOAT | FGAMEDATA_FLAGS_FLOAT_X, sizeof(f32),
			F32_DATATABLE_0, F32_DATATABLE_0 };
		FMemFrame_t Frame = fmem_GetFrame();
		FGameDataFileHandle_t hFile = fgamedata_LoadFileToFMem( _USER_PROP_FILENAME );
		if( hFile != FGAMEDATA_INVALID_FILE_HANDLE ) {
			for( u32 n = 0; n < EUK_COUNT_LASER; ++n ) {
				FGameDataTableHandle_t hTable = fgamedata_GetFirstTableHandle( hFile, m_aUserPropMapTable[n].pszTableName );
				if( hTable == FGAMEDATA_INVALID_TABLE_HANDLE ) {
					continue;
				}
				f32 afColor[4];
				BOOL bOK = TRUE;
				for( u32 c = 0; bOK && c < 4; ++c ) {
					bOK = fgamedata_GetFieldFromTable( hTable, 3 + c, &RetailFloat, &afColor[c] );
				}
				if( bOK ) {
					for( u32 c = 0; c < 4; ++c ) {
						_aRetailTracerColor[n][c] = afColor[c];
					}
				}
			}
		}
		fmem_ReleaseFrame( Frame );
	}

	// PC port: the fire sound now resolves through the 'LaserFire' sound
	// group (retail semantics - w_laser.csv field 65 is a group name), which
	// replaces the old raw-SFX alias fallback that remapped the never-
	// resolving 'LaserFire' bank name to 'Fire Laser EUK1' by hand.
	m_pSoundGroupImpactEnergy = CFSoundGroup::RegisterGroup( "Energy" );

	// Do this for each EUK level...
	// The retail laser muzzle flash is a particle (fields 47-50); the source's
	// textured card used fields that map to unrelated retail floats (400x50),
	// which drew a screen-sized flash. Disable the card.
	for( i=0; i<EUK_COUNT_LASER; ++i ) {
		m_aUserProps[i].pszMuzzleTex = NULL;
		m_aUserProps[i].fMuzzleWidth = 0.0f;
		m_aUserProps[i].fMuzzleHeight = 0.0f;
	}

	for( i=0; i<EUK_COUNT_LASER; i++ ) {
		{
			m_aoTracerDef[i].pUser = NULL;				// This will get set at the point of spawning the tracers.
			m_aoTracerDef[i].pFcnKillCallback = _TracerKilledCallback;
			m_aoTracerDef[i].fWidth_WS = m_aUserProps[i].fTracerWidth;
			m_aoTracerDef[i].fLength_WS = m_aUserProps[i].fTracerLength;
			m_aoTracerDef[i].fSpeed_WS = m_aUserProps[i].fRocketSpeed;
			m_aoTracerDef[i].fMaxTailDist_WS = m_aUserProps[i].fMaxLiveRange;
			m_aoTracerDef[i].fBeginDeathUnitFade_WS = 0.25f;
			// The retail tracer texture (TF_1shot_01) is opaque, with a black
			// background around the streak. Lerp blending drew that background
			// as a black wedge; additive blending adds zero for black, leaving
			// only the coloured core.
			m_aoTracerDef[i].uFlags = TRACERFLAG_DRAW_ADDITIVE;
			m_aoTracerDef[i].pFcnBuildSkipList = _LaserTracerBuildSkipListCallback;

			// L2 charged burst: same tracer callback/colour, but the retail
			// burst uses the wider/longer shape from its own CSV columns.
			m_aoBurstTracerDef[i] = m_aoTracerDef[i];
			m_aoBurstTracerDef[i].uFlags |= TRACERFLAG_THICK_PROJECTILE;
			m_aoBurstTracerDef[i].fWidth_WS = (m_aUserProps[i].fRetailBurstWidth > 0.0f)
				? m_aUserProps[i].fRetailBurstWidth : (m_aUserProps[i].fTracerWidth * 4.0f);
			m_aoBurstTracerDef[i].fLength_WS = (m_aUserProps[i].fRetailBurstLength > 0.0f)
				? m_aUserProps[i].fRetailBurstLength : m_aUserProps[i].fTracerLength;
			m_aoBurstTracerDef[i].fSpeed_WS = (m_aUserProps[i].fRetailBurstSpeed > 0.0f)
				? m_aUserProps[i].fRetailBurstSpeed : m_aUserProps[i].fRocketSpeed;

			pTexDef = (FTexDef_t *)(fresload_Load(FTEX_RESNAME, m_aUserProps[i].pszL12_TexName));
			if(pTexDef == NULL)
			{
				DEVPRINTF("CWeaponLaser::InitSystem() : Could not load tracer texture '%s'.\n", m_aUserProps[i].pszL12_TexName);
			}

			m_aTracerTexInst[i].SetTexDef( pTexDef );

			// [ma] TEMP r18: tracer texture resolution (grey-beam check).
			{
				static u32 s_uTEMPTexMask = 0;
				if( ( s_uTEMPTexMask & ( 1u << i ) ) == 0 ) {
					s_uTEMPTexMask |= ( 1u << i );
					DEVPRINTF( "[ma] TEMP r18 laser tex u=%u name=%s tex=%p\n",
						i, m_aUserProps[i].pszL12_TexName ? m_aUserProps[i].pszL12_TexName : "(null)", pTexDef );
				}
			}
		}

		// Fill out our global info data...
		pInfo = &m_aaInfo[WEAPON_TYPE_LASER][i];

		// Do any special limitations on the values we got from the CSV file.

		pInfo->nGrip = GRIP_RIGHT_ARM;
		pInfo->nReloadType = RELOAD_TYPE_FULLYAUTOMATIC;
		pInfo->nStanceType = STANCE_TYPE_STANDARD;
		pInfo->fMinTargetAssistDist = m_aUserProps[i].fMinTargetAssistDist;
		pInfo->fMaxTargetAssistDist = m_aUserProps[i].fMaxTargetAssistDist;
		pInfo->fMaxLiveRange = m_aUserProps[i].fMaxLiveRange;
		pInfo->fUnitRecoil = m_aUserProps[i].fUnitRecoil;
		pInfo->nClipAmmoMax = (m_aUserProps[i].fClipAmmoMax) >= 0.0f ? (u16)m_aUserProps[i].fClipAmmoMax : INFINITE_AMMO;
		pInfo->nReserveAmmoMax = (m_aUserProps[i].fReserveAmmoMax) >= 0.0f ? (u16)m_aUserProps[i].fReserveAmmoMax : INFINITE_AMMO;
		pInfo->nInfoFlags = INFOFLAG_NONE;
		pInfo->nReticleType = CReticle::TYPE_DROID_STANDARD;

		m_apDamageProfile[i] = CDamage::FindDamageProfile( m_aUserProps[i].pszDamageProfile );
	}

	// [ma] TEMP r17: one-shot dump of resolved laser damage profiles + key props.
	{
		static BOOL s_bTEMPLaserPropsDumped = FALSE;
		if( !s_bTEMPLaserPropsDumped ) {
			u32 j;
			s_bTEMPLaserPropsDumped = TRUE;
			for( j = 0; j < EUK_COUNT_LASER; j++ ) {
				CDamageProfile *pTEMPProf = m_apDamageProfile[j];
				DEVPRINTF( "[ma] TEMP r17 laser props u=%u profileName=%s profile=%p cause=%s innerHP=%g ooR=%g clip=%g\n",
					j,
					m_aUserProps[j].pszDamageProfile ? m_aUserProps[j].pszDamageProfile : "(null)",
					pTEMPProf,
					( pTEMPProf && pTEMPProf->m_pszName ) ? pTEMPProf->m_pszName : "(null)",
					pTEMPProf ? pTEMPProf->m_HitpointRange.m_fInnerValue : -1.0f,
					m_aUserProps[j].fOORechargeRate,
					m_aUserProps[j].fClipAmmoMax );
			}
		}
	}

	// Success...

	return TRUE;

	// Error...
_ExitWithError:
	fres_ReleaseFrame( ResFrame );
	return FALSE;
}


void CWeaponLaser::UninitSystem( void ) {
}



CWeaponLaser::CWeaponLaser() {
	_ClearDataMembers();
}



CWeaponLaser::~CWeaponLaser() {
	if( IsSystemInitialized() && IsCreated() ) {
		DetachFromParent();
		DetachAllChildren();
		RemoveFromWorld( TRUE );
		ClassHierarchyDestroy();
	}
}



BOOL CWeaponLaser::Create( cchar *pszEntityName, const CFMtx43A *pMtx, cchar *pszAIBuilderName ) {
	// Get pointer to the leaf class's builder object...
	CWeaponLaserBuilder *pBuilder = (CWeaponLaserBuilder *)GetLeafClassBuilder();

	// If we're the leaf class, set the builder defaults...
	if( pBuilder == &_WeaponLaserBuilder ) {
		pBuilder->SetDefaults( 0, 0, ENTITY_TYPE_WEAPONLASER );
	}

	m_pSoundEmitter = NULL;

	// Set our builder parameters...

	// Create an entity...
	return CWeapon::Create( WEAPON_TYPE_LASER, pszEntityName, pMtx, pszAIBuilderName );
}



BOOL CWeaponLaser::ClassHierarchyBuild( void ) {
	u32 i;//, j;
	FMesh_t *pMesh;
	FMeshInit_t MeshInit;
	_ResourceData_t *pResourceData;
	Info_t *pInfo;
	u32 uMeshEUK;
	u32 uResEUK;

	FASSERT( IsSystemInitialized() );
	FASSERT( !IsCreated() );
	FASSERT( FWorld_pWorld );

	// Get a frame...
	FResFrame_t ResFrame = fres_GetFrame();

	// Get pointer to the leaf class's builder object...
	CWeaponLaserBuilder *pBuilder = (CWeaponLaserBuilder *)GetLeafClassBuilder();

	// Set input parameters for CWeapon creation...

	// Build parent class...
	if( !CWeapon::ClassHierarchyBuild() ) {
		// Parent class could not be built...
		goto _ExitWithError;
	}

	// Set defaults...
	_ClearDataMembers();

	// Initialize from builder object...

	for( i=0, pResourceData=m_aResourceData; i<EUK_COUNT_LASER; ++i, ++pResourceData ) {
		// Each EUK level...
		if( m_nSingleMeshForEUKs >= 0 ) {
			FASSERT( m_nSingleMeshForEUKs < EUK_COUNT_LASER );
			uMeshEUK = m_nSingleMeshForEUKs;
		} else {
			uMeshEUK = i;
		}

		pInfo = &m_aaInfo[WEAPON_TYPE_LASER][i];

		// Create the world mesh...
		pResourceData->m_pWorldMesh = fnew CFWorldMesh;
		if( pResourceData->m_pWorldMesh == NULL ) {
			DEVPRINTF( "CWeaponLaser::Create(): Not enough memory to create m_pWorldMesh.\n" );
			goto _ExitWithError;
		}

		// Load the mesh resource...
		pMesh = (FMesh_t *)fresload_Load( FMESH_RESTYPE, m_aUserProps[uMeshEUK].apszMeshName[_MESH_WEAPON] );


		if( pMesh == NULL ) {
			DEVPRINTF( "CWeaponLaser::Create(): Could not find mesh '%s'.\n", m_aUserProps[i].apszMeshName[_MESH_WEAPON] );

			pMesh = m_pErrorMesh;
			if( pMesh == NULL ) {
				goto _ExitWithError;
			}
		}

		// Init the world mesh...
		MeshInit.pMesh = pMesh;
		MeshInit.nFlags = FMESHINST_FLAG_NOBONES;
		MeshInit.fCullDist = m_aUserProps[i].fWeaponCullDist;
		MeshInit.Mtx = *MtxToWorld();

		uResEUK = i;
		if( !CreateSharedEUKData( &MeshInit, &uResEUK, &(pResourceData->m_pWorldMesh) ) ) {
			DEVPRINTF( "CWeaponLaser::ClassHierarchyBuild():  Error creating EUK shared data\n" );
			goto _ExitWithError;
		}

		if( uResEUK == i ) {
			pResourceData->m_pWorldMesh->Init( &MeshInit );
			pResourceData->m_pWorldMesh->RemoveFromWorld();
			pResourceData->m_pWorldMesh->SetCollisionFlag( TRUE );
			pResourceData->m_pWorldMesh->m_nUser = MESHTYPES_ENTITY;
			pResourceData->m_pWorldMesh->m_pUser = this;
			pResourceData->m_pWorldMesh->SetUserTypeBits( TypeBits() );
			pResourceData->m_pWorldMesh->SetLineOfSightFlag(FALSE); //pgm added this so that weapons never block los tests
		}

		if(uMeshEUK == 2)
		{
			m_apL3_LaserBeam[0] = CLaserBeam::GetLaserBeam();
			m_apL3_LaserBeam[1] = CLaserBeam::GetLaserBeam();
			if((m_apL3_LaserBeam[0] == NULL) || (m_apL3_LaserBeam[1] == NULL))
			{
				DEVPRINTF("CWeaponLaser::ClassHierarchyBuild(): Could not get laser beam object.\n");
				goto _ExitWithError;
			}
			{
				CFColorRGBA BeamColor;
				BeamColor.Set( _aRetailTracerColor[2][0], _aRetailTracerColor[2][1],
							   _aRetailTracerColor[2][2], _aRetailTracerColor[2][3] );
				m_apL3_LaserBeam[0]->SetColor( BeamColor );
				m_apL3_LaserBeam[1]->SetColor( BeamColor );
			}
		}
	}

	m_hTracerGroup = tracer_CreateGroup(&m_aTracerTexInst[m_nUpgradeLevel], 18);
	if(m_hTracerGroup == TRACER_NULLGROUPHANDLE)
	{
		DEVPRINTF( "CWeaponLaser::ClassHierarchyBuild(): Could not create tracer group.\n" );
		goto _ExitWithError;
	}

	m_pResourceData = &m_aResourceData[ m_nUpgradeLevel ];

	fforce_NullHandle( &m_hForce );
	m_bFiredLastFrame = FALSE;
	return TRUE;

	// Error:
_ExitWithError:
	Destroy();
	fres_ReleaseFrame( ResFrame );
	return FALSE;
}


BOOL CWeaponLaser::ClassHierarchyBuilt( void ) {
	FASSERT( IsSystemInitialized() );
	FASSERT( IsCreated() );

	FResFrame_t ResFrame = fres_GetFrame();

	if( !CWeapon::ClassHierarchyBuilt() ) {
		goto _ExitWithError;
	}

	// Retail's laser meter sits at half reserve at rest; only the upper half
	// is used by the charge display. Set that baseline at creation so the bar
	// does not start full and then visibly drop when charging begins.
	if( m_pInfo && ( m_pInfo->nClipAmmoMax != INFINITE_AMMO ) ) {
		SetClipAmmo( (u16)( m_pInfo->nClipAmmoMax / 2 ), FALSE );
	}

	EnableOurWorkBit();

	return TRUE;

_ExitWithError:
	Destroy();
	fres_ReleaseFrame( ResFrame );
	return FALSE;
}


void CWeaponLaser::ClassHierarchyDestroy( void ) {
	// Destroy ourselves first...

	_StopChargeEffects();
	m_nChargeStartAmmo = 0;

	fdelete( m_apL3_LaserBeam[0] );
	fdelete( m_apL3_LaserBeam[1] );

	fforce_Kill( &m_hForce );

	tracer_DestroyGroup(m_hTracerGroup);
	m_hTracerGroup = TRACER_NULLGROUPHANDLE;


	for( u32 i=0; i<EUK_COUNT_LASER; i++ ) {
		DestroySharedEUKData( i );
	}

	_ClearDataMembers();

	// Now destroy our parent...
	CWeapon::ClassHierarchyDestroy();
}


CEntityBuilder *CWeaponLaser::GetLeafClassBuilder( void ) {
	return &_WeaponLaserBuilder;
}


void CWeaponLaser::_ClearDataMembers( void ) {
	u32 i;//, j;

	for( i=0; i<EUK_COUNT_LASER; ++i )
	{
		m_aResourceData[i].m_pWorldMesh = NULL;
	}

	m_pResourceData = NULL;
	m_fSecondsCountdownTimer = 0.0f;
	m_fRechargeCntDn = 0.0f;
	m_L3_fDrainCntDn = 0.0f;
	m_bFireThisFrame = FALSE;
	m_bRechargeThisFrame = FALSE;
	m_bFiredLastFrame = FALSE;
	m_bCharging = FALSE;
	m_bBurstThisFrame = FALSE;
	m_nBurstDamageTier = 0;
	m_bChargeSoundStarted = FALSE;
	m_bChargeReadySoundPlayed = FALSE;
	m_fChargeDelayTimer = 0.0f;
	m_fChargeUnit = 0.0f;
	m_pChargeSoundEmitter = NULL;
	m_hChargeParticle = NULL;
	m_pChargeLight = NULL;
	m_vChargeMuzzle_WS.Zero();
	m_nChargeStartAmmo = 0;
	m_nChargeCost = 0;
	m_hTracerGroup = TRACER_NULLGROUPHANDLE;
	m_pvecBuddyFirePos_WS = NULL;

	m_vTargetPos_WS.Zero();
	m_vecL3_EndPoint.Zero();
	m_apL3_LaserBeam[0] = NULL;
	m_apL3_LaserBeam[1] = NULL;
}


void CWeaponLaser::_StopChargeEffects( void ) {
	if( m_pChargeSoundEmitter ) {
		m_pChargeSoundEmitter->Destroy();
		m_pChargeSoundEmitter = NULL;
	}
	if( m_hChargeParticle ) {
		fparticle_KillEmitter( m_hChargeParticle );
		m_hChargeParticle = NULL;
	}
	if( m_pChargeLight ) {
		CLightPool::ReturnToFreePool( m_pChargeLight );
		m_pChargeLight = NULL;
	}
	m_bChargeReadySoundPlayed = FALSE;
}


void CWeaponLaser::_UpdateChargeAmmo( void ) {
	if( m_nChargeStartAmmo == 0 || !m_pInfo || m_pInfo->nClipAmmoMax == 0 ) {
		return;
	}
	// The lower half is the fixed reserve; charging fills only the upper half.
	const u16 nMax = m_pInfo->nClipAmmoMax;
	const u16 nBase = nMax / 2;
	u16 nAmmo = (u16)( (f32)nBase + (f32)( nMax - nBase ) * m_fChargeUnit + 0.5f );
	SetClipAmmo( nAmmo );
}


void CWeaponLaser::_TracerKilledCallback( TracerDef_t *pTracerDef, TracerKillReason_e nKillReason, const FCollImpact_t *pImpact ) {
	// [ma] TEMP r17: which kill reasons fire at all (are tracers hitting anything?).
	{
		static u32 s_nTEMPKill = 0;
		static u32 s_nTEMPNoGeo = 0;
		s_nTEMPKill++;
		if( nKillReason != TRACER_KILLREASON_HIT_GEO ) { s_nTEMPNoGeo++; }
		if( s_nTEMPKill <= 16 || ( s_nTEMPKill % 120 ) == 0 ) {
			DEVPRINTF( "[ma] TEMP r17 laser kill f=%u cnt=%u reason=%u noGeo=%u\n",
				(unsigned)FVid_nFrameCounter, s_nTEMPKill, (unsigned)nKillReason, s_nTEMPNoGeo );
		}
	}

	if( nKillReason != TRACER_KILLREASON_HIT_GEO ) {
		// Retail burst tracers leave an end puff when they fizzle at max range.
		if( pTracerDef && ( pTracerDef->nUser & _LASER_NUSER_BURST ) && pTracerDef->pUser ) {
			CWeaponLaser *pLaser = (CWeaponLaser *)(pTracerDef->pUser);
			const u32 nLevel = ( pTracerDef->nUser >> _LASER_NUSER_LEVEL_SHIFT ) & 7;
			if( nLevel < EUK_COUNT_LASER ) {
				const _UserProps_t &UserProps = pLaser->m_aUserProps[nLevel];
				if( UserProps.hRetailBurstParticle ) {
					CFVec3A vEndPos = pTracerDef->TailPos_WS;
					CFVec3A vEndOffset = pTracerDef->UnitDir_WS;
					vEndOffset.Mul( 1.0f );
					vEndPos.Add( vEndOffset );
					fparticle_SpawnEmitter( UserProps.hRetailBurstParticle, vEndPos.v3,
											&pTracerDef->UnitDir_WS.v3, 1.0f );
				}
			}
		}
		return;
	}

	FASSERT( pTracerDef->pUser != NULL );

	// [ma] TEMP r17: HIT_GEO breakdown: tagless geo vs tag-not-entity vs entity.
	{
		static u32 s_nTEMPGeo = 0;
		static u32 s_nTEMPNoTag = 0;
		static u32 s_nTEMPTagOther = 0;
		s_nTEMPGeo++;
		if( pImpact->pTag == NULL ) { s_nTEMPNoTag++; }
		else if( ((CFWorldMesh *)(pImpact->pTag))->m_nUser != MESHTYPES_ENTITY ) { s_nTEMPTagOther++; }
		if( s_nTEMPGeo <= 16 || ( s_nTEMPGeo % 120 ) == 0 ) {
			CFVec3A vTEMPd;
			vTEMPd.Sub( pImpact->ImpactPoint, pTracerDef->TailPos_WS );
			DEVPRINTF( "[ma] TEMP r17 laser geo f=%u cnt=%u noTag=%u tagOther=%u d2=%g\n",
				(unsigned)FVid_nFrameCounter, s_nTEMPGeo, s_nTEMPNoTag, s_nTEMPTagOther, vTEMPd.MagSq() );
		}
	}

	CWeaponLaser *pLaser = (CWeaponLaser *)(pTracerDef->pUser);
	u32 nLevel = ( pTracerDef->nUser >> _LASER_NUSER_LEVEL_SHIFT ) & 7;
	u32 nTier = ( pTracerDef->nUser >> _LASER_NUSER_TIER_SHIFT ) & 3;
	if( nLevel >= EUK_COUNT_LASER ) { nLevel = pLaser->m_nUpgradeLevel; }
	if( nTier >= 3 ) { nTier = 2; }
	const _UserProps_t &UserProps = pLaser->m_aUserProps[nLevel];

	CDamageProfile *pLaserDamage = pLaser->m_apDamageProfile[nLevel];
	if( pTracerDef->nUser & _LASER_NUSER_BURST ) {
		CDamageProfile *pBurstDamage = UserProps.apChargedDamageProfile[nTier];
		if( pBurstDamage ) { pLaserDamage = pBurstDamage; }
	}

	const CGCollMaterial *pCollMaterial = CGColl::GetMaterial( pImpact->nUserType );

	// Retail lets the tracer continue through wires rather than treating the
	// wire as a solid surface.
	if( CGColl::ExtractWire( pImpact ) ) {
		TracerDef_t WireDef = *pTracerDef;
		WireDef.TailPos_WS = pImpact->ImpactPoint;
		tracer_NewTracer( pLaser->m_hTracerGroup, &WireDef, 0.0f, FALSE, FALSE, &CDamageForm::m_TempDamager );
		return;
	}

	const BOOL bBurst = !!( pTracerDef->nUser & _LASER_NUSER_BURST );
	CFVec3A vReflection;
	vReflection.ReceiveReflection( pTracerDef->UnitDir_WS, pImpact->UnitFaceNormal );

	const u8 nProjReact = CGColl::ComputeProjectileReaction( pImpact );
	BOOL bRicochet = ( nProjReact == CEntity::PROJECTILE_REACTION_RICOCHET ) ||
		( nProjReact == CEntity::PROJECTILE_REACTION_SHIELD );
	if( !bBurst && !bRicochet &&
		pCollMaterial->IsBlastDecalEnabled() &&
		( ( pTracerDef->nUser & _LASER_NUSER_RICOCHET_MASK ) < 4 ) ) {
		const f32 fNormalDotDir = pTracerDef->UnitDir_WS.Dot( pImpact->UnitFaceNormal );
		if( (-fNormalDotDir) < 0.3f ) { bRicochet = TRUE; }
	}

	if( bRicochet ) {
		TracerDef_t RicochetDef = *pTracerDef;
		RicochetDef.nUser = (u16)( ( pTracerDef->nUser & ~_LASER_NUSER_RICOCHET_MASK ) |
			( ( pTracerDef->nUser & _LASER_NUSER_RICOCHET_MASK ) + 1 ) );
		RicochetDef.UnitDir_WS = vReflection;
		RicochetDef.TailPos_WS = pImpact->ImpactPoint;

		CFVec3A vStartOffset = vReflection;
		vStartOffset.Mul( 0.1f );
		RicochetDef.TailPos_WS.Add( vStartOffset );

		tracer_NewTracer( pLaser->m_hTracerGroup, &RicochetDef, 0.0f, FALSE, FALSE, &CDamageForm::m_TempDamager );

		if( UserProps.pSoundGroupRic ) {
			CFSoundGroup::PlaySound( UserProps.pSoundGroupRic, FALSE, &pImpact->ImpactPoint );
		}

		if( bBurst ) {
			if( UserProps.hRetailBurstRicExplosion != FEXPLOSION_INVALID_HANDLE ) {
				FExplosion_SpawnerHandle_t hSpawner = CExplosion2::GetExplosionSpawner();
				if( hSpawner != FEXPLOSION_INVALID_HANDLE ) {
					FExplosionSpawnParams_t SpawnParams;
					SpawnParams.InitToDefaults();
					SpawnParams.pDamager = &pTracerDef->Damager;
					SpawnParams.Pos_WS = pImpact->ImpactPoint;
					SpawnParams.UnitDir = pImpact->UnitFaceNormal;
					SpawnParams.uSurfaceType = pImpact->nUserType;
					CExplosion2::SpawnExplosion( hSpawner, UserProps.hRetailBurstRicExplosion, &SpawnParams );
				}
			}

			if( UserProps.hRetailBurstParticle ) {
				fparticle_SpawnEmitter( UserProps.hRetailBurstParticle, pImpact->ImpactPoint.v3,
										&vReflection.v3, 1.0f );
			}

			if( pImpact->pTag ) {
				CFWorldMesh *pHitWorldMesh = (CFWorldMesh *)(pImpact->pTag);
				if( pHitWorldMesh->m_nUser == MESHTYPES_ENTITY ) {
					CEntity *pEDamagee = (CEntity *)(pHitWorldMesh->m_pUser);
					CDamageForm *pDamageForm = CDamage::GetEmptyDamageFormFromPool();
					if( pDamageForm ) {
						pDamageForm->m_nDamageLocale = CDamageForm::DAMAGE_LOCALE_IMPACT;
						pDamageForm->m_nDamageDelivery = CDamageForm::DAMAGE_DELIVERY_ONE_SPECIFIC_ENTITY;
						pDamageForm->m_pDamageProfile = pLaserDamage;
						pDamageForm->m_Damager.pWeapon = pLaser;
						pDamageForm->m_Damager.pBot = pLaser->GetOwner();
						pDamageForm->m_Damager.nDamagerPlayerIndex = pLaser->GetOwner() ? pLaser->GetOwner()->m_nPossessionPlayerIndex : -1;
						pDamageForm->m_pDamageeEntity = pEDamagee;
						pDamageForm->InitTriDataFromCollImpact( pHitWorldMesh, pImpact, &pTracerDef->UnitDir_WS );
						CDamage::SubmitDamageForm( pDamageForm );
					}
				}
			}
		} else {
			pCollMaterial->DrawParticle( CGCollMaterial::PARTICLE_TYPE_BITS,
				&pImpact->ImpactPoint, &vReflection, 1.0f );
			pCollMaterial->DrawParticle( CGCollMaterial::PARTICLE_TYPE_DUST,
				&pImpact->ImpactPoint, &vReflection, 0.5f );
		}

		return;
	}

	// Retail plays the material-class energy impact sound on every surface hit.
	if( m_pSoundGroupImpactEnergy ) {
		CFSoundGroup::PlaySound( m_pSoundGroupImpactEnergy, FALSE, &pImpact->ImpactPoint );
	}

	if( bBurst ) {
		if( UserProps.hRetailBurstHitExplosion != FEXPLOSION_INVALID_HANDLE ) {
			FExplosion_SpawnerHandle_t hSpawner = CExplosion2::GetExplosionSpawner();
			if( hSpawner != FEXPLOSION_INVALID_HANDLE ) {
				FExplosionSpawnParams_t SpawnParams;
				SpawnParams.InitToDefaults();
				SpawnParams.pDamager = &pTracerDef->Damager;
				SpawnParams.Pos_WS = pImpact->ImpactPoint;
				SpawnParams.UnitDir = pImpact->UnitFaceNormal;
				SpawnParams.uSurfaceType = pImpact->nUserType;
				CExplosion2::SpawnExplosion( hSpawner, UserProps.hRetailBurstHitExplosion, &SpawnParams );
			}
		}

		pCollMaterial->DrawAllDrawableParticles(
			&pImpact->ImpactPoint,
			&pImpact->UnitFaceNormal,
			TRUE,
			1.0f,
			1.0f,
			1.0f
		);

		CFDebrisSpawner DebrisSpawner;
		DebrisSpawner.InitToDefaults();
		DebrisSpawner.m_Mtx.m_vPos = pImpact->ImpactPoint;
		DebrisSpawner.m_Mtx.m_vZ = vReflection;
		DebrisSpawner.m_nEmitterType = CFDebrisSpawner::EMITTER_TYPE_POINT;
		DebrisSpawner.m_pDebrisGroup = pCollMaterial->m_apDebrisGroup[ CGCollMaterial::DEBRIS_GROUP_MEDIUM ];
		DebrisSpawner.m_fMinSpeed = 10.0f;
		DebrisSpawner.m_fMaxSpeed = 20.0f;
		DebrisSpawner.m_fUnitDirSpread = 1.0f;
		DebrisSpawner.m_nMinDebrisCount = 10;
		DebrisSpawner.m_nMaxDebrisCount = 14;
		CGColl::SpawnDebris( &DebrisSpawner );

		const f32 afTierIntensity[3] = { 0.0f, 0.5f, 1.0f };
		const f32 fTierIntensity = afTierIntensity[nTier];
		if( UserProps.hRetailBurstParticle ) {
			fparticle_SpawnEmitter( UserProps.hRetailBurstParticle, pImpact->ImpactPoint.v3,
									&vReflection.v3, fTierIntensity );
		}

		if( UserProps.hRetailBurstDecal ) {
			CFDecal::Create( UserProps.hRetailBurstDecal, pImpact, &pTracerDef->UnitDir_WS );
		}

		if( pCollMaterial->IsBlastDecalEnabled() ) {
			if( UserProps.hRetailBurstAttachParticle1 ) {
				FParticle_EmitterHandle_t hAttach = fparticle_SpawnEmitter(
					UserProps.hRetailBurstAttachParticle1, pImpact->ImpactPoint.v3,
					&pImpact->UnitFaceNormal.v3, 1.0f );
				if( hAttach ) {
					fparticle_SetDurationMultiplier( hAttach, 1.0f + 2.0f * (f32)nTier );
				}
			}
			if( UserProps.hRetailBurstAttachParticle2 ) {
				FParticle_EmitterHandle_t hAttach = fparticle_SpawnEmitter(
					UserProps.hRetailBurstAttachParticle2, pImpact->ImpactPoint.v3,
					&pImpact->UnitFaceNormal.v3, 1.0f );
				if( hAttach ) {
					fparticle_SetDurationMultiplier( hAttach, 1.0f + 4.0f * (f32)nTier );
				}
			}
		}

		if( UserProps.hRetailEndParticle ) {
			CFVec3A vSecondaryPos = pImpact->UnitFaceNormal;
			vSecondaryPos.Mul( 0.75f );
			vSecondaryPos.Add( pImpact->ImpactPoint );
			fparticle_SpawnEmitter( UserProps.hRetailEndParticle, vSecondaryPos.v3,
									&pImpact->UnitFaceNormal.v3, fTierIntensity );
		}
	} else {
		pCollMaterial->DrawAllDrawableParticles(
			&pImpact->ImpactPoint,
			&pImpact->UnitFaceNormal,
			TRUE,
			fmath_RandomFloatRange( 0.0f, 0.0f ),
			fmath_RandomFloatRange( 0.0f, 0.1f ),
			fmath_RandomFloatRange( 0.4f, 0.5f )
		);

		if( UserProps.hDecalDef ) {
			CFDecal::Create( UserProps.hDecalDef, pImpact, &pTracerDef->UnitDir_WS );
		}
	}

	if( pImpact->pTag ) {
		CFWorldMesh *pHitWorldMesh = (CFWorldMesh *)(pImpact->pTag);
		if( pHitWorldMesh->m_nUser == MESHTYPES_ENTITY ) {
			CEntity *pEDamagee = (CEntity *)(pHitWorldMesh->m_pUser);

			// [ma] TEMP r17: entity-branch entries (form submission gate passed).
			{
				static u32 s_nTEMPHitEnt = 0;
				s_nTEMPHitEnt++;
				if( s_nTEMPHitEnt <= 16 || ( s_nTEMPHitEnt % 120 ) == 0 ) {
					DEVPRINTF( "[ma] TEMP r17 laser hitEntity f=%u cnt=%u damagee=%p profile=%p upg=%u owner=%p self=%d\n",
						(unsigned)FVid_nFrameCounter, s_nTEMPHitEnt, pEDamagee,
						pLaserDamage, (unsigned)pLaser->m_nUpgradeLevel,
						pLaser->GetOwner(),
						(int)( (void *)pEDamagee == (void *)pLaser->GetOwner() ) );
				}
			}

			CDamageForm *pDamageForm = CDamage::GetEmptyDamageFormFromPool();
			if( pDamageForm ) {
				pDamageForm->m_nDamageLocale = CDamageForm::DAMAGE_LOCALE_IMPACT;
				pDamageForm->m_nDamageDelivery = CDamageForm::DAMAGE_DELIVERY_ONE_SPECIFIC_ENTITY;
				pDamageForm->m_pDamageProfile = pLaserDamage;
				pDamageForm->m_Damager.pWeapon = pLaser;
				pDamageForm->m_Damager.pBot = pLaser->GetOwner();
				pDamageForm->m_Damager.nDamagerPlayerIndex = pLaser->GetOwner() ? pLaser->GetOwner()->m_nPossessionPlayerIndex : -1;
				pDamageForm->m_pDamageeEntity = pEDamagee;
				pDamageForm->InitTriDataFromCollImpact( pHitWorldMesh, pImpact, &pTracerDef->UnitDir_WS );
				CDamage::SubmitDamageForm( pDamageForm );
			} else {
				static u32 s_nTEMPPool19 = 0;
				s_nTEMPPool19++;
				if( s_nTEMPPool19 <= 16 || ( s_nTEMPPool19 % 120 ) == 0 ) {
					DEVPRINTF( "[ma] TEMP r19 laser pool EMPTY f=%u cnt=%u\n",
						(unsigned)FVid_nFrameCounter, s_nTEMPPool19 );
				}
			}
		}
	}

	//make an AI impact sound in case any AI are listening
	if( pLaser->m_pOwnerBot->m_nPossessionPlayerIndex > -1 && pLaser->m_fSecsUntilNextSound <= 0.0f) {
		AIEnviro_AddSound( pImpact->ImpactPoint, AIEnviro_fLaserImpactAudibleRange, 0.3f, AISOUNDTYPE_WEAPON, 0, pLaser );
		pLaser->m_fSecsUntilNextSound = 0.5f;
	}
}


void CWeaponLaser::KillRumble( void ) {
	FASSERT( IsCreated() );

	fforce_Kill( &m_hForce );
}


void CWeaponLaser::SetItemInst( CItemInst *pItemInst, BOOL bUpdateItemInstAmmoFromWeaponAmmo ) {
	CWeapon::SetItemInst(pItemInst, bUpdateItemInstAmmoFromWeaponAmmo);
	if( pItemInst ) {
		pItemInst->SetAmmoDisplayType(CItemInst::AMMODISPLAY_METER, CItemInst::AMMODISPLAY_METER);

		// Item pickup/restore can put a full clip back into the weapon. The
		// laser's resting meter is half; normalize before the HUD ever sees it.
		if( m_pInfo && ( m_pInfo->nClipAmmoMax != INFINITE_AMMO ) ) {
			u16 nHalf = (u16)( m_pInfo->nClipAmmoMax / 2 );
			if( GetClipAmmo() > nHalf ) {
				SetClipAmmo( nHalf, FALSE );
			}
			pItemInst->NotifyAmmoMightHaveChanged( GetClipAmmo(), GetReserveAmmo() );
		}
	}
}



void CWeaponLaser::ClassHierarchyAddToWorld( void ) {
	FASSERT( IsCreated() );
	FASSERT( !IsInWorld() );

	CWeapon::ClassHierarchyAddToWorld();

	m_pResourceData->m_pWorldMesh->m_Xfm.BuildFromMtx( m_MtxToWorld, m_fScaleToWorld );
	m_pResourceData->m_pWorldMesh->UpdateTracker();
}


void CWeaponLaser::ClassHierarchyRemoveFromWorld( void ) {
	FASSERT( IsCreated() );
	FASSERT( IsInWorld() );

	CWeapon::ClassHierarchyRemoveFromWorld();

	// Remove the weapon mesh from the world...
	m_pResourceData->m_pWorldMesh->RemoveFromWorld();
}


void CWeaponLaser::ClassHierarchyDrawEnable( BOOL bDrawingHasBeenEnabled ) {
	CWeapon::ClassHierarchyDrawEnable( bDrawingHasBeenEnabled );

//	if( IsInWorld() ) 
	{
		u32 i;

		if( bDrawingHasBeenEnabled ) {
			// Enable drawing of this weapon...

			for( i=0; i<EUK_COUNT_LASER; ++i ) {
				FMATH_CLEARBITMASK( m_aResourceData[i].m_pWorldMesh->m_nFlags, FMESHINST_FLAG_DONT_DRAW );
			}
		} else {
			// Disable drawing of this weapon...

			for( i=0; i<EUK_COUNT_LASER; ++i ) {
				FMATH_SETBITMASK( m_aResourceData[i].m_pWorldMesh->m_nFlags, FMESHINST_FLAG_DONT_DRAW );
			}

			if( m_nChargeStartAmmo ) {
				SetClipAmmo( m_nChargeStartAmmo );
				m_nChargeStartAmmo = 0;
			}
			_StopChargeEffects();
			m_bCharging = FALSE;
			m_fChargeUnit = 0.0f;
			m_fChargeDelayTimer = 0.0f;
		}
	}
}


void CWeaponLaser::ClassHierarchyRelocated( void *pIdentifier ) {
	CWeapon::ClassHierarchyRelocated( pIdentifier );

	if( IsInWorld() ) {
		if( ( 0.0f == m_fScaleToWorld ) || ( m_fScaleToWorld != m_fScaleToWorld ) ) {
			DEVPRINTF( "[ma] TEMP laser reloc bad scale: name=%s scale=%g\n", Name() ? Name() : "(null)", m_fScaleToWorld );
		}
		m_pResourceData->m_pWorldMesh->m_Xfm.BuildFromMtx( m_MtxToWorld, m_fScaleToWorld );
		m_pResourceData->m_pWorldMesh->UpdateTracker();
	}
}


void CWeaponLaser::AppendTrackerSkipList(u32& nTrackerSkipListCount, CFWorldTracker ** apTrackerSkipList) {
	// Add our weapon mesh...
	FASSERT( (nTrackerSkipListCount + 1) <= FWORLD_MAX_SKIPLIST_ENTRIES );
	if( nTrackerSkipListCount < FWORLD_MAX_SKIPLIST_ENTRIES ) { apTrackerSkipList[nTrackerSkipListCount++] = m_pResourceData->m_pWorldMesh; }
}

// Note that ClassHierarchyResetToState() is always called prior to this call.
void CWeaponLaser::ClassHierarchySetUpgradeLevel( u32 nPreviousUpgradeLevel ) {
	FASSERT( !IsTransitionState( CurrentState() ) );

	_ResourceData_t *pOldResourceData = &m_aResourceData[nPreviousUpgradeLevel];
	Info_t *pOldInfo = &m_aaInfo[WEAPON_TYPE_LASER][nPreviousUpgradeLevel];

	// Remove old mesh from world...
	pOldResourceData->m_pWorldMesh->RemoveFromWorld();

	// Set up new level...
	m_pResourceData = &m_aResourceData[ m_nUpgradeLevel ];

	if( IsInWorld() ) {
		m_pResourceData->m_pWorldMesh->m_Xfm = pOldResourceData->m_pWorldMesh->m_Xfm;
		m_pResourceData->m_pWorldMesh->UpdateTracker();
	}

	tracer_SetNewTexture( m_hTracerGroup, &m_aTracerTexInst[m_nUpgradeLevel] );

	// Do not carry a half-completed L2 charge across an upgrade swap.
	if( m_nChargeStartAmmo ) {
		SetClipAmmo( m_nChargeStartAmmo );
		m_nChargeStartAmmo = 0;
	}
	_StopChargeEffects();
	m_bCharging = FALSE;
	m_bBurstThisFrame = FALSE;
	m_bChargeSoundStarted = FALSE;
	m_fChargeUnit = 0.0f;
	m_fChargeDelayTimer = 0.0f;
}



void CWeaponLaser::ComputeMuzzlePoint_WS( CFVec3A *pMuzzlePoint_WS ) const {
	FASSERT( IsCreated() );

	pMuzzlePoint_WS->Mul( m_MtxToWorld.m_vFront, m_aUserProps[m_nUpgradeLevel].fDistFromWeaponOrigToMuzzle ).Add( m_MtxToWorld.m_vPos );
}



// This function is called to let the weapon know what the trigger values are.
// It should be called whether or not the triggers are pressed because this function
// is responsible for firing and reloading of the weapon.
//
// pProjUnitDir_WS must point in the direction that the weapon will fire.
//
// Return value:
//   Bit 0: Trigger #1 fired
//   Bit 1: Trigger #2 fired
u32 CWeaponLaser::TriggerWork( f32 fUnitTriggerVal1, f32 fUnitTriggerVal2, const CFVec3A *pvTargetPos_WS, const CFVec3A *pBuddyFirePos_WS/* = NULL*/ ) {
	FASSERT( IsCreated() );

	m_bBurstThisFrame = FALSE;

	// L1 has no charge row; L2/L3/Mil carry the retail charge data. Keep the
	// right mouse/analog behavior identical across the upgraded lasers.
	if( m_nUpgradeLevel != 0 ) {
		return _TriggerWorkCharged( fUnitTriggerVal1, pvTargetPos_WS, pBuddyFirePos_WS );
	}

	return _TriggerWorkNormal( fUnitTriggerVal1, pvTargetPos_WS, pBuddyFirePos_WS );
}


// The original single-shot/rapid-fire path. Used by every EUK except the L2
// charge path, and as the fallback when the L2 trigger is fully pulled.
u32 CWeaponLaser::_TriggerWorkNormal( f32 fUnitTriggerVal1, const CFVec3A *pvTargetPos_WS, const CFVec3A *pBuddyFirePos_WS ) {
	m_bFireThisFrame = FALSE;

	if(fUnitTriggerVal1 < 0.25f)
	{
		// Trigger not down...
		m_bRechargeThisFrame = TRUE;
		return(0);
	}

	m_bRechargeThisFrame = FALSE;

	if(CurrentState() != STATE_DEPLOYED)
	{
		// Not in a state to fire...
		return(0);
	}

	u32 uLowerThreshold;
	if((m_nUpgradeLevel == 2) && (!m_bFiredLastFrame))
	{
		uLowerThreshold = 10;
	}
	else
	{
		uLowerThreshold = 0;
	}

	if(m_nClipAmmo <= uLowerThreshold)
	{
		// No rounds in clip...
		return(0);
	}

	if((m_nUpgradeLevel != 2) && (m_fSecondsCountdownTimer > 0.0f))
	{
		// Can't fire another round yet...
		return(0);
	}

	m_vTargetPos_WS.Set( *pvTargetPos_WS );
	if ( m_pOwnerBot && m_pOwnerBot->m_nPossessionPlayerIndex >= 0 )
	{
		m_pOwnerBot->FocusHumanTargetPoint_WS( &m_vTargetPos_WS, this );
	}

	// We passed all the tests, let's go ahead and fire off a round.
	m_bFireThisFrame = TRUE;
	m_pvecBuddyFirePos_WS = pBuddyFirePos_WS;

	return(1);
}


// XBE 0x00142d10: state 0 enters state 1 (rapid fire) only when the trigger
// value is exactly 1.0. A partial pull enters state 3, then the state-4 charge.
// Releasing the partial pull fires the burst.
u32 CWeaponLaser::_TriggerWorkCharged( f32 fUnitTriggerVal1, const CFVec3A *pvTargetPos_WS, const CFVec3A *pBuddyFirePos_WS ) {
	const f32 fDt = FLoop_fPreviousLoopSecs;
	const BOOL bTriggerDown = ( fUnitTriggerVal1 >= 0.25f );
	const BOOL bFullPull = ( fUnitTriggerVal1 >= 0.99f );
	const BOOL bPartialPull = bTriggerDown && !bFullPull;

	m_bFireThisFrame = FALSE;

	if( m_bCharging ) {
		if( !bTriggerDown ) {
			// Released: a completed charge becomes the burst.
			BOOL bFireBurst = ( m_fChargeUnit > 0.0f );
			m_nBurstDamageTier = ( m_fChargeUnit >= 0.95f ) ? 2 : ( ( m_fChargeUnit >= 0.5f ) ? 1 : 0 );
			m_bCharging = FALSE;
			m_fChargeUnit = 0.0f;
			m_fChargeDelayTimer = 0.0f;
			m_bChargeSoundStarted = FALSE;

			if( bFireBurst ) {
				m_bFireThisFrame = TRUE;
				m_bBurstThisFrame = TRUE;
				m_bRechargeThisFrame = FALSE;
				m_vTargetPos_WS.Set( *pvTargetPos_WS );
				if( m_pOwnerBot && m_pOwnerBot->m_nPossessionPlayerIndex >= 0 ) {
					m_pOwnerBot->FocusHumanTargetPoint_WS( &m_vTargetPos_WS, this );
				}
				m_pvecBuddyFirePos_WS = pBuddyFirePos_WS;
				return(1);
			}

			if( m_nChargeStartAmmo ) {
				SetClipAmmo( m_nChargeStartAmmo );
				m_nChargeStartAmmo = 0;
			}
			m_bRechargeThisFrame = TRUE;
			return(0);
		}

		if( bFullPull ) {
			// A full pull aborts the charge and uses the rapid-fire path.
			if( m_nChargeStartAmmo ) {
				SetClipAmmo( m_nChargeStartAmmo );
				m_nChargeStartAmmo = 0;
			}
			m_bCharging = FALSE;
			m_fChargeUnit = 0.0f;
			m_fChargeDelayTimer = 0.0f;
			m_bChargeSoundStarted = FALSE;
		} else {
			m_bRechargeThisFrame = FALSE;
			if( m_fChargeDelayTimer > 0.0f ) {
				m_fChargeDelayTimer -= fDt;
				if( m_fChargeDelayTimer < 0.0f ) {
					m_fChargeDelayTimer = 0.0f;
				}
			} else {
				f32 fChargeTime = m_aUserProps[m_nUpgradeLevel].fRetailChargeTime;
				if( fChargeTime <= 0.0f ) {
					fChargeTime = 2.0f;
				}
				m_fChargeUnit += fDt / fChargeTime;
				FMATH_CLAMPMAX( m_fChargeUnit, 1.0f );
				_UpdateChargeAmmo();
			}
			return(0);
		}
	}

	if( bPartialPull && (CurrentState() == STATE_DEPLOYED) &&
		(m_fSecondsCountdownTimer <= 0.0f) ) {
		const u16 nHalf = ( m_pInfo && m_pInfo->nClipAmmoMax != INFINITE_AMMO )
			? (u16)( m_pInfo->nClipAmmoMax / 2 ) : 0;

		// The charge cannot begin until the lower-half reserve is full.
		if( GetClipAmmo() < nHalf ) {
			m_bRechargeThisFrame = TRUE;
			return(0);
		}

		m_bRechargeThisFrame = FALSE;
		m_bCharging = TRUE;
		m_bChargeSoundStarted = FALSE;
		m_bChargeReadySoundPlayed = FALSE;
		m_fChargeUnit = 0.0f;
		m_fChargeDelayTimer = 0.0f;

		m_nChargeStartAmmo = nHalf;
		m_nChargeCost = 0;
		SetClipAmmo( nHalf );
		_UpdateChargeAmmo();
		return(0);
	}

	return _TriggerWorkNormal( fUnitTriggerVal1, pvTargetPos_WS, pBuddyFirePos_WS );
}


f32 CWeaponLaser::GetProjectileSpeed(void) const
{
	return m_aoTracerDef[m_nUpgradeLevel].fSpeed_WS;
}



static void _TrackPlayerLaser(void* pUserData, const CFVec3A& NewPos_WS, BOOL bDied)
{
	CWeaponLaser* pLaser = (CWeaponLaser*) pUserData;

	if (bDied)
	{
//		pLaser->m_uAISoundHandle = 0;
	}
	else
	{
		if( pLaser->m_uAISoundHandle &&
			!AIEnviro_ModifySound( pLaser->m_uAISoundHandle,  NewPos_WS, AIEnviro_fProjectileVisualRadius, 0.1f, AISOUNDTYPE_PROJECTILE, pLaser->GetOwner()) )
		{
			pLaser->m_uAISoundHandle = 0;
		}
	}

}
// Called once per frame to update animations, timers, etc.
void CWeaponLaser::ClassHierarchyWork( void ) {
	// Call our parent. It might need to do some work...
	CWeapon::ClassHierarchyWork();

	// Exit if we don't have any work to do...
	if( !IsOurWorkBitSet() ) {
		return;
	}

	// At rest the laser meter is the lower half reserve. Clamp any item/pickup
	// restore that put a full clip back into it; charging owns the upper half.
	if( !m_bCharging && !m_bFireThisFrame && m_pInfo &&
		( m_pInfo->nClipAmmoMax != INFINITE_AMMO ) ) {
		u16 nHalf = (u16)( m_pInfo->nClipAmmoMax / 2 );
		if( nHalf > 0 && GetClipAmmo() > nHalf ) {
			SetClipAmmo( nHalf );
		}
	}

	// L2 charge feedback. This mirrors the retail surge effect set:
	// 'LaserCharge' loop, 'Laser07' particle, the omni light and the
	// 'LaserBrstBeep' ready cue. The particle/light follow the muzzle while
	// the weapon is charging.
	if( m_bCharging ) {
		ComputeMuzzlePoint_WS( &m_vChargeMuzzle_WS );
		const _UserProps_t *pChargeProps = &m_aUserProps[m_nUpgradeLevel];
		// Retail drives the surge particle from the normalized surge value;
		// at the half-reserve baseline it is zero. Starting it at a nonzero
		// floor made the particle cover the weapon before charging really began.
		const f32 fPower = m_fChargeUnit;

		if( !m_pChargeSoundEmitter && pChargeProps->pSoundGroupCharge ) {
			m_pChargeSoundEmitter = AllocAndPlaySound( pChargeProps->pSoundGroupCharge,
				1.0f, 1.0f, -1.0f, &m_vChargeMuzzle_WS );
			m_bChargeSoundStarted = TRUE;
		}
		if( m_pChargeSoundEmitter ) {
			m_pChargeSoundEmitter->SetFrequencyFactor( 1.0f + m_fChargeUnit );
		}

		if( !m_hChargeParticle && pChargeProps->hRetailChargeParticle && IsDrawEnabled() ) {
			// Keep the position pointer: this overload makes the emitter follow
			// the muzzle while Glitch/vehicle moves. The copy-by-value overload
			// leaves the charge glow at the spawn point.
			m_hChargeParticle = fparticle_SpawnEmitter( pChargeProps->hRetailChargeParticle,
				&m_vChargeMuzzle_WS.v3, &m_MtxToWorld.m_vFront.v3, NULL, fPower );
		}
		if( m_hChargeParticle ) {
			fparticle_SetIntensity( m_hChargeParticle, fPower );
		}

		if( m_fChargeUnit >= 1.0f && !m_bChargeReadySoundPlayed ) {
			if( pChargeProps->pSoundGroupBurstBeep ) {
				PlaySound( pChargeProps->pSoundGroupBurstBeep, 0.8f, 1.0f, -1.0f, &m_vChargeMuzzle_WS );
			}
			m_bChargeReadySoundPlayed = TRUE;
		}
	} else {
		_StopChargeEffects();
		m_bChargeSoundStarted = FALSE;
	}

	if(m_bFireThisFrame)
	{
		//////////////////////////////////////////////////////////////////////
		// Do the effects for the firing.
		BOOL bPlayForce = FALSE;
		CFVec3A vecPerturb;
		CFVec3A vFireUnitDir;
		CFVec3A vecMuzzlePoint;

		
		//vFireUnitDir.Sub( m_vTargetPos_WS, vecMuzzlePoint );
		//f32 fMag2 = vFireUnitDir.MagSq();
		//if( fMag2 < 0.001f ) {
		//	// too close
		//	vFireUnitDir = m_MtxToWorld.m_vFront;
		//}
		//
		//vFireUnitDir.Mul( fmath_InvSqrt( fMag2 ) );

		ComputeMuzzlePoint_WS(&vecMuzzlePoint);
		ComputeFireUnitDir( &vFireUnitDir, &vecMuzzlePoint, &m_MtxToWorld.m_vFront, &m_vTargetPos_WS, FALSE );

		const BOOL bBurst = m_bBurstThisFrame;
		const f32 fPerturbFactor = bBurst ? 0.125f : ( ( m_nUpgradeLevel == 3 ) ? 0.0625f : 0.125f );
		vecPerturb.Set(fmath_RandomFloatRange(-fPerturbFactor, fPerturbFactor), fmath_RandomFloatRange(-fPerturbFactor, fPerturbFactor), fmath_RandomFloatRange(-fPerturbFactor, fPerturbFactor));
		vFireUnitDir.Add(vecPerturb);
		vFireUnitDir.Unitize();

		TracerDef_t *pTracerDef = bBurst
			? &m_aoBurstTracerDef[m_nUpgradeLevel]
			: &m_aoTracerDef[m_nUpgradeLevel];

		u16 nTracerUser = 0;
		if( bBurst ) {
			nTracerUser = (u16)( _LASER_NUSER_BURST |
				( ( m_nBurstDamageTier & 3 ) << _LASER_NUSER_TIER_SHIFT ) |
				( ( m_nUpgradeLevel & 7 ) << _LASER_NUSER_LEVEL_SHIFT ) );
		}

		if( bBurst && m_aUserProps[m_nUpgradeLevel].hRetailBurstParticle && IsDrawEnabled() ) {
			fparticle_SpawnEmitter( m_aUserProps[m_nUpgradeLevel].hRetailBurstParticle,
				vecMuzzlePoint.v3, &vFireUnitDir.v3, 1.0f );
		}

		if( m_aUserProps[m_nUpgradeLevel].hRetailFiringSmokeParticle && IsDrawEnabled() ) {
			fparticle_SpawnEmitter( m_aUserProps[m_nUpgradeLevel].hRetailFiringSmokeParticle,
				vecMuzzlePoint.v3, &vFireUnitDir.v3, 1.0f );
		}

		if( bBurst && m_pOwnerBot && ( m_pOwnerBot->m_nPossessionPlayerIndex >= 0 ) ) {
			CFCamera *pCamera = fcamera_GetCameraByIndex( m_pOwnerBot->m_nPossessionPlayerIndex );
			if( pCamera ) {
				pCamera->ShakeCamera( 0.2f, 0.15f );
			}
		}

		switch( bBurst ? 0 : m_nUpgradeLevel )
		{
			case 2:
			{
				if((m_nClipAmmo >= _uFizzleThreshhold) || (fmath_RandomChance(1.0f - (f32)(m_nClipAmmo) * (1.0f / (f32)(_uFizzleThreshhold)))))
				{
					// The leak's continuous L3 beam path is broken in this port (white
					// screen). Retail GameCube L3 uses the same tracer path as L1/L2;
					// do the same here and fall through to the shared tracer code.

					if( IsDrawEnabled() ) {
					}

					if(m_pvecBuddyFirePos_WS != NULL)
					{

						if( IsDrawEnabled() ) {
						}
					}

					if ( !m_pSoundEmitter )
					{
						// PC port: sound-group playback; the helper picks 2D/3D
						// from ownership like the old pair of alloc calls.
						m_pSoundEmitter = AllocAndPlaySound( m_aUserProps[m_nUpgradeLevel].pSoundGroupFire, 1.0f, 1.0f, m_aUserProps[m_nUpgradeLevel].fMaxLiveRange, &vecMuzzlePoint );
					}

					if (GetOwner() && GetOwner()->m_nPossessionPlayerIndex > -1)
					{
						AIEnviro_BoostPlayerSoundTo(GetOwner()->m_nPossessionPlayerIndex, 30.0f);
					}

					bPlayForce = TRUE;
				}
				else if ( m_pSoundEmitter )
				{
					m_pSoundEmitter->Destroy();
					m_pSoundEmitter = NULL;
				}
				// fall through to the shared tracer path (L3 red tracer)
			}
			case 0:
			case 1:
			case 3:
			default:
			{
				// [ma] TEMP r18: spawn inputs (muzzle/target/dir garbage or NaN check).
				{
					static u32 s_nTEMPSpawn = 0;
					s_nTEMPSpawn++;
					if( s_nTEMPSpawn <= 16 || ( s_nTEMPSpawn % 120 ) == 0 ) {
						DEVPRINTF( "[ma] TEMP r18 laser spawn f=%u cnt=%u muzzle=(%g,%g,%g) target=(%g,%g,%g) dir=(%g,%g,%g) orig=(%g,%g,%g) dist=%g eye=(%g,%g,%g)\n",
							(unsigned)FVid_nFrameCounter, s_nTEMPSpawn,
							vecMuzzlePoint.x, vecMuzzlePoint.y, vecMuzzlePoint.z,
							m_vTargetPos_WS.x, m_vTargetPos_WS.y, m_vTargetPos_WS.z,
							vFireUnitDir.x, vFireUnitDir.y, vFireUnitDir.z,
							m_MtxToWorld.m_vPos.x, m_MtxToWorld.m_vPos.y, m_MtxToWorld.m_vPos.z,
							m_aUserProps[m_nUpgradeLevel].fDistFromWeaponOrigToMuzzle,
							GetOwner() ? GetOwner()->GetApproxEyePoint()->x : 0.0f,
							GetOwner() ? GetOwner()->GetApproxEyePoint()->y : 0.0f,
							GetOwner() ? GetOwner()->GetApproxEyePoint()->z : 0.0f );
					}
				}

				pTracerDef->pUser = this;
				pTracerDef->ColorRGBA.Set( _aRetailTracerColor[m_nUpgradeLevel][0],
										  _aRetailTracerColor[m_nUpgradeLevel][1],
										  _aRetailTracerColor[m_nUpgradeLevel][2],
										  _aRetailTracerColor[m_nUpgradeLevel][3] );
				pTracerDef->UnitDir_WS = vFireUnitDir;
				pTracerDef->TailPos_WS = vecMuzzlePoint;
				pTracerDef->nUser = nTracerUser;	// ricochet counter low nibble
//				pTracerDef->ppCollSkipTrackers = &m_pOwnerBot->m_pWorldMesh;

				CFCamera *pCamera = fcamera_GetCameraByIndex(CPlayer::m_pCurrent->m_nPlayerIndex);

				if(m_nUpgradeLevel == 1)
				{
					if( IsDrawEnabled() ) {
					}
				}

				ConstructDamagerData();
				if (tracer_NewTracer(m_hTracerGroup, pTracerDef, 0.2f, FALSE, FALSE, &CDamageForm::m_TempDamager))
				{

					if (m_pOwnerBot->m_nPossessionPlayerIndex > -1)
					{
						if( !m_uAISoundHandle || !AIEnviro_SoundHandleToPtr(m_uAISoundHandle)) {
							m_uAISoundHandle = AIEnviro_AddSound( vecMuzzlePoint, AIEnviro_fProjectileVisualRadius, 0.1f, AISOUNDTYPE_PROJECTILE, AISOUNDCTRL_VISIBLE_ENTITY, GetOwner());
						}
						tracer_AddExtraParamsToLastTracer(_TrackPlayerLaser, (void*) this);
					}
				
				}
				if(m_pvecBuddyFirePos_WS != NULL)
				{
					pTracerDef->UnitDir_WS = vFireUnitDir;
					pTracerDef->TailPos_WS = *m_pvecBuddyFirePos_WS;
					pTracerDef->nUser = nTracerUser;	// ricochet counter low nibble

					ConstructDamagerData();
					tracer_NewTracer(m_hTracerGroup, pTracerDef, 0.2f, FALSE, FALSE, &CDamageForm::m_TempDamager);
				}

				// Sound-group volume/radius come from the group definition;
				// the original per-owner multipliers (0.75 2D, 0.6 @ 450 3D)
				// are kept.
				if( bBurst ) {
					// Retail's burst uses the dedicated layered groups, not the
					// ordinary repeating-fire group.
					if( m_aUserProps[m_nUpgradeLevel].pSoundGroupBurst1 ) {
						PlaySound( m_aUserProps[m_nUpgradeLevel].pSoundGroupBurst1,
									0.9f, 1.0f, -1.0f, &vecMuzzlePoint );
					}
					if( m_aUserProps[m_nUpgradeLevel].pSoundGroupBurst2 ) {
						PlaySound( m_aUserProps[m_nUpgradeLevel].pSoundGroupBurst2,
									0.9f, 1.0f, -1.0f, &vecMuzzlePoint );
					}
				} else if ( IsOwnedByPlayer() )
				{
					PlaySound( m_aUserProps[m_nUpgradeLevel].pSoundGroupFire,
									0.75f,	//fVolumeMultiplier
									1.0f,
									-1.0f,
									&vecMuzzlePoint );
				}
				else
				{
					PlaySound( m_aUserProps[m_nUpgradeLevel].pSoundGroupFire,
									0.6f,	//fVolumeMultiplier
									1.0f,
									450.0f,
									&vecMuzzlePoint );
				}

				if (GetOwner() && GetOwner()->m_nPossessionPlayerIndex > -1)
				{
					AIEnviro_BoostPlayerSoundTo(GetOwner()->m_nPossessionPlayerIndex, 30.0f);
				}
	
				bPlayForce = TRUE;

				break;
			}
		}

		//
		//////////////////////////////////////////////////////////////////////

		//////////////////////////////////////////////////////////////////////
		// Force feedback effects.
		if(bPlayForce && IsOwnedByPlayer())
		{
			// Human player...
			FForce_Effect_e eForce;
			if( bBurst ) {
				eForce = FFORCE_EFFECT_ENERGY_PULSE;
			} else if( m_nUpgradeLevel == 0 ) {
				eForce = FFORCE_EFFECT_ROUGH_RUMBLE_MED;
			} else if( m_nUpgradeLevel == 1 ) {
				eForce = FFORCE_EFFECT_ROUGH_RUMBLE_HIGH;
			} else {
				eForce = FFORCE_EFFECT_ROUGH_RUMBLE_MAX;
			}

			fforce_Kill(&m_hForce);
			fforce_Play(Player_aPlayer[GetOwner()->m_nPossessionPlayerIndex].m_nControllerIndex,
				eForce, &m_hForce);
		}
		//
		//////////////////////////////////////////////////////////////////////

		//////////////////////////////////////////////////////////////////////
		// Update timer variables and do any appropriate energy depletion.
		if( bBurst )
		{
			// The retail burst has its own recovery delay and leaves the
			// weapon at its post-burst energy level.
			f32 fCooldown = m_aUserProps[m_nUpgradeLevel].fRetailBurstCooldown;
			if( fCooldown <= 0.0f ) {
				fCooldown = m_aUserProps[m_nUpgradeLevel].fOORoundsPerSec;
			}
			m_fSecondsCountdownTimer = fCooldown;

			// The charge uses only the upper half of the meter; the lower
			// half is the laser's standing energy reserve.
			u16 nAfterAmmo = m_pInfo->nClipAmmoMax / 2;
			SetClipAmmo( nAfterAmmo );
			m_nChargeStartAmmo = 0;
			m_nChargeCost = 0;
			m_bCharging = FALSE;
			m_fChargeUnit = 0.0f;
			m_fChargeDelayTimer = 0.0f;
			m_bChargeSoundStarted = FALSE;
			m_bBurstThisFrame = FALSE;
		}
		else
		{
			switch(m_nUpgradeLevel)
			{
				case 2:
				{
					// See if 
					m_L3_fDrainCntDn -= FLoop_fPreviousLoopSecs;
					while(m_L3_fDrainCntDn <= 0.0f)
					{
						// JUSTIN: Have this come from the CSV file.
						m_L3_fDrainCntDn += m_aUserProps[m_nUpgradeLevel].fOORoundsPerSec;
						RemoveFromClip(1);
					}
					break;
				}
				case 0:
				case 1:
				case 3:
				default:
				{
					// Set timer until the next round is ready...
					m_fSecondsCountdownTimer += m_aUserProps[m_nUpgradeLevel].fOORoundsPerSec;
					RemoveFromClip(1);
					break;
				}
			}
		}
		//
		//////////////////////////////////////////////////////////////////////

		m_bFiredLastFrame = TRUE;
	}
	else
	{
		m_bFiredLastFrame = FALSE;

		if ( m_pSoundEmitter )
		{
			m_pSoundEmitter->Destroy();
			m_pSoundEmitter = NULL;
		}
	}

	if(m_bRechargeThisFrame)
	{
		// Replenish the energy.
		m_fRechargeCntDn -= FLoop_fPreviousLoopSecs;

		while(m_fRechargeCntDn <= 0.0f)
		{
			m_fRechargeCntDn += m_aUserProps[m_nUpgradeLevel].fRechargeRate;			// JUSTIN: Get this from a CSV.

			// The lower half is the laser's resting reserve. Normal recharge
			// must not creep into the upper half used by the charge display.
			u16 nHalf = ( m_pInfo && m_pInfo->nClipAmmoMax != INFINITE_AMMO )
				? (u16)( m_pInfo->nClipAmmoMax / 2 ) : 0;
			if( nHalf > 0 && GetClipAmmo() >= nHalf ) {
				break;
			}
			SetClipAmmo(GetClipAmmo() + 1);
		}

		// [ma] TEMP r17: recharge ticking (first 16 then every 120th).
		{
			static u32 s_nTEMPRech = 0;
			s_nTEMPRech++;
			if( s_nTEMPRech <= 16 || ( s_nTEMPRech % 120 ) == 0 ) {
				DEVPRINTF( "[ma] TEMP r17 laser recharge f=%u cnt=%u upg=%u cntDn=%g rate=%g ooR=%g clip=%u\n",
					(unsigned)FVid_nFrameCounter, s_nTEMPRech, (unsigned)m_nUpgradeLevel,
					m_fRechargeCntDn, m_aUserProps[m_nUpgradeLevel].fRechargeRate,
					m_aUserProps[m_nUpgradeLevel].fOORechargeRate, (unsigned)GetClipAmmo() );
			}
		}
	}
	else
	{
		// [ma] TEMP r17: idle state every 600 frames (is recharge ever armed?).
		static u32 s_nTEMPIdle = 0;
		s_nTEMPIdle++;
		if( ( s_nTEMPIdle % 600 ) == 0 ) {
			DEVPRINTF( "[ma] TEMP r17 laser idle f=%u upg=%u clip=%u cntDn=%g\n",
				(unsigned)FVid_nFrameCounter, (unsigned)m_nUpgradeLevel, (unsigned)GetClipAmmo(), m_fRechargeCntDn );
		}
	}

	if(m_fSecondsCountdownTimer > 0.0f)
	{
		m_fSecondsCountdownTimer -= FLoop_fPreviousLoopSecs;
	}

	// Do EUK-level-specific work...
	switch( m_nUpgradeLevel )
	{
	case 0:
		break;
	case 1:
		break;
	case 2:
		break;
	case 3:
		break;
	default:
		break;
	}
}


// Switches immediately to state CWeapon::CurrentState().
void CWeaponLaser::ClassHierarchyResetToState( void ) {
	CWeapon::ClassHierarchyResetToState();

	m_fSecondsCountdownTimer = 0.0f;
	m_L3_fDrainCntDn = 0.0f;
	m_fRechargeCntDn = 0.0f;
	m_bFiredLastFrame = FALSE;
	if( m_nChargeStartAmmo ) {
		SetClipAmmo( m_nChargeStartAmmo );
		m_nChargeStartAmmo = 0;
	}
	_StopChargeEffects();
	m_bCharging = FALSE;
	m_bBurstThisFrame = FALSE;
	m_bChargeSoundStarted = FALSE;
	m_fChargeDelayTimer = 0.0f;
	m_fChargeUnit = 0.0f;
}



void CWeaponLaser::NotifyAmmoMightHaveChanged( void ) {

}


