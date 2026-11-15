#pragma once

#include "fang.h"
#include "ffile.h"

// Runtime mod profiles, modelled on Cobblecorn's MA_PORT_MOD_API_V1 profile
// loader. A vanilla launch leaves this disabled and never consults a mod
// directory. A modded launch points pcmods_Configure() at a folder containing
// runtime.ini, resources/ and optionally media/. With no explicit folder the
// loader scans <exe>/mods and loads every valid profile it finds.
void pcmods_Configure( const char *pszRoot );
void pcmods_Disable( void );
BOOL pcmods_IsActive( void );
const char *pcmods_GetRoot( void );

BOOL pcmods_ResolveFile( const char *requested, char *output, unsigned capacity );
const FFileMasterEntry_t *pcmods_OpenStream( const char *requested, u32 *size );
BOOL pcmods_ReadStream( const FFileMasterEntry_t *entry, u32 start, u32 amount,
	void *destination, FFileAsyncReadCallback_t *callback, void *user, s32 *result );

// Named feature switches and additive maps/media declarations (API v3).
struct PcModLevel {
	char title[128], world[16], csv[16], materials[16];
};

BOOL pcmods_Feature( const char *name );
unsigned pcmods_CustomLevelCount( void );
const PcModLevel *pcmods_CustomLevel( unsigned index );
BOOL pcmods_IsCustomLevel( s32 levelId );
BOOL pcmods_ResolveMedia( const char *requested, char *output, unsigned capacity );
BOOL pcmods_HasMovies( void );

// Custom levels are registered after the retail multiplayer block. These are
// the same first/last ids Cobblecorn's profile API uses.
#define PC_MODS_LEVEL_BASE	56
#define PC_MODS_LEVEL_MAX	16
