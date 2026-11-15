#include "pc_mods.h"
#include "fdata.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <list>
#include <string>
#include <vector>

namespace {

struct Config;

struct Profile {
    std::string root;
    std::string ini;
    std::vector<PcModLevel> levels;
};

struct Config {
    BOOL enabled;
    BOOL configured;
    BOOL mergedBuilt;
    std::vector<Profile> profiles;
    std::string summary;
    std::string mergedCsv;
    std::string mergedPath;
    Config() : enabled( FALSE ), configured( FALSE ), mergedBuilt( FALSE ) {}
};

struct StreamEntry {
    FFileMasterEntry_t entry;
    std::string path;
    std::string data; // Non-empty for synthetic streams such as merged CSV data.
};

Config _config;
std::list<StreamEntry> _streams; // Stable entry addresses; no open handles retained.

BOOL _SafeResourceName( const char *name ) {
    if( !name || !name[0] ) return FALSE;
    if( strlen( name ) >= FDATA_PRJFILE_FILENAME_LEN ) return FALSE;
    if( strchr( name, ':' ) || strchr( name, '*' ) || strchr( name, '?' ) ||
        !strcmp( name, "." ) || !strcmp( name, ".." ) ) return FALSE;
    for( const char *p = name; *p; ++p ) {
        if( !( ( *p >= 'a' && *p <= 'z' ) || ( *p >= 'A' && *p <= 'Z' ) ||
               ( *p >= '0' && *p <= '9' ) || *p == '_' || *p == '$' || *p == '.' ) ) {
            return FALSE;
        }
    }
    return TRUE;
}

const char *_BaseName( const char *requested ) {
    const char *name = requested;
    for( const char *p = requested; *p; ++p ) {
        if( *p == '/' || *p == '\\' || *p == ':' ) name = p + 1;
    }
    return name;
}

BOOL _IsDirectory( const char *path ) {
    const DWORD flags = GetFileAttributesA( path );
    return flags != INVALID_FILE_ATTRIBUTES && ( flags & FILE_ATTRIBUTE_DIRECTORY );
}

BOOL _FileExists( const char *path ) {
    const DWORD flags = GetFileAttributesA( path );
    return flags != INVALID_FILE_ATTRIBUTES && !( flags & FILE_ATTRIBUTE_DIRECTORY );
}

std::string _ResourcePath( const Profile &profile, const char *name ) {
    char path[1200];
    snprintf( path, sizeof( path ), "%s\\resources\\%s", profile.root.c_str(), name );
    return std::string( path );
}

BOOL _ReadWholeFile( const std::string &path, std::string &data ) {
    HANDLE file = CreateFileA( path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
    if( file == INVALID_HANDLE_VALUE ) return FALSE;
    DWORD high = 0;
    const DWORD low = GetFileSize( file, &high );
    if( high || low > 64u * 1024u * 1024u ) { CloseHandle( file ); return FALSE; }
    data.resize( low );
    DWORD read = 0;
    const BOOL ok = low == 0 || ( ReadFile( file, &data[0], low, &read, NULL ) && read == low );
    CloseHandle( file );
    if( !ok ) data.clear();
    return ok;
}

BOOL _WriteWholeFile( const std::string &path, const std::string &data ) {
    HANDLE file = CreateFileA( path.c_str(), GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
    if( file == INVALID_HANDLE_VALUE ) return FALSE;
    DWORD written = 0;
    const BOOL ok = data.empty() || ( WriteFile( file, data.data(), (DWORD)data.size(), &written, NULL ) && written == data.size() );
    CloseHandle( file );
    return ok;
}


struct CsvField {
    u32 type;
    u32 number;      // Type 1 raw float bits.
    std::string text; // Types 0 and 2 bytes, including no terminator.
    CsvField() : type( 0 ), number( 0 ) {}
};

struct CsvTable {
    std::string key;
    u16 tableIndex;
    std::vector<CsvField> fields;
    CsvTable() : tableIndex( 0 ) {}
};

void _PutU32( std::string &out, size_t offset, u32 value ) {
    memcpy( &out[offset], &value, sizeof( value ) );
}

void _PutU16( std::string &out, size_t offset, u16 value ) {
    memcpy( &out[offset], &value, sizeof( value ) );
}

void _AppendU32( std::string &out, u32 value ) {
    out.append( (const char *)&value, sizeof( value ) );
}

BOOL _ParseCsv( const std::string &data, std::vector<CsvTable> &tables ) {
    if( data.size() < 16 ) return FALSE;
    const u32 fileSize = *(const u32 *)&data[0];
    const u32 tableCount = *(const u32 *)&data[4];
    const u32 tableOffset = *(const u32 *)&data[8];
    if( fileSize > data.size() || fileSize < 16 || tableOffset > fileSize ||
        tableCount > ( fileSize - tableOffset ) / 16 ) {
        return FALSE;
    }
    tables.clear();
    tables.reserve( tableCount );
    for( u32 i = 0; i < tableCount; ++i ) {
        const char *entry = &data[tableOffset + i * 16];
        const u32 keyOffset = *(const u32 *)( entry + 0 );
        const u32 keyLength = *(const u32 *)( entry + 4 );
        const u16 fieldCount = *(const u16 *)( entry + 8 );
        const u16 tableIndex = *(const u16 *)( entry + 10 );
        const u32 fieldOffset = *(const u32 *)( entry + 12 );
        if( keyOffset > fileSize || keyLength > fileSize - keyOffset ||
            fieldOffset > fileSize || fieldCount > ( fileSize - fieldOffset ) / 12 ) {
            tables.clear();
            return FALSE;
        }
        CsvTable table;
        table.key.assign( data.data() + keyOffset, keyLength );
        table.tableIndex = tableIndex;
        table.fields.reserve( fieldCount );
        for( u16 f = 0; f < fieldCount; ++f ) {
            const char *field = &data[fieldOffset + f * 12];
            CsvField value;
            value.type = *(const u32 *)( field + 0 );
            value.number = *(const u32 *)( field + 4 );
            const u32 length = *(const u32 *)( field + 8 );
            if( value.type == 0 ) {
                if( value.number > fileSize || length > fileSize - value.number ) { tables.clear(); return FALSE; }
                value.text.assign( data.data() + value.number, length );
            } else if( value.type == 2 ) {
                if( value.number > fileSize || length > ( fileSize - value.number ) / 2 ) { tables.clear(); return FALSE; }
                value.text.assign( data.data() + value.number, length * 2 );
            } else if( value.type != 1 ) {
                tables.clear();
                return FALSE;
            }
            table.fields.push_back( value );
        }
        tables.push_back( table );
    }
    return TRUE;
}

std::string _BuildCsv( const std::vector<CsvTable> &tables ) {
    if( tables.empty() ) return std::string();
    const u32 tableCount = (u32)tables.size();
    const u32 tableOffset = 16;
    const u32 fieldStart = tableOffset + tableCount * 16;
    u32 cursor = fieldStart;
    std::vector<u32> fieldOffsets;
    fieldOffsets.reserve( tableCount );
    for( size_t i = 0; i < tables.size(); ++i ) {
        fieldOffsets.push_back( cursor );
        cursor += (u32)tables[i].fields.size() * 12;
    }
    const u32 stringsOffset = cursor;
    std::string strings;
    std::vector<u32> keyOffsets;
    std::vector<std::string> fieldBlobs;
    keyOffsets.reserve( tableCount );
    fieldBlobs.reserve( tableCount );
    for( size_t i = 0; i < tables.size(); ++i ) {
        const CsvTable &table = tables[i];
        keyOffsets.push_back( stringsOffset + (u32)strings.size() );
        strings.append( table.key );
        strings.push_back( '\0' );
        std::string blob;
        for( size_t f = 0; f < table.fields.size(); ++f ) {
            const CsvField &field = table.fields[f];
            const u32 stringOffset = stringsOffset + (u32)strings.size();
            if( field.type == 0 ) {
                _AppendU32( blob, 0 );
                _AppendU32( blob, stringOffset );
                _AppendU32( blob, (u32)field.text.size() );
                strings.append( field.text );
                strings.push_back( '\0' );
            } else if( field.type == 2 ) {
                _AppendU32( blob, 2 );
                _AppendU32( blob, stringOffset );
                _AppendU32( blob, (u32)field.text.size() / 2 );
                strings.append( field.text );
                strings.push_back( '\0' );
                strings.push_back( '\0' );
            } else {
                _AppendU32( blob, 1 );
                _AppendU32( blob, field.number );
                _AppendU32( blob, 0 );
            }
        }
        fieldBlobs.push_back( blob );
    }

    std::string out;
    out.resize( stringsOffset + strings.size(), '\0' );
    const u32 totalSize = (u32)out.size();
    _PutU32( out, 0, totalSize );
    _PutU32( out, 4, tableCount );
    _PutU32( out, 8, tableOffset );
    _PutU32( out, 12, 0 );
    for( u32 i = 0; i < tableCount; ++i ) {
        const size_t entry = tableOffset + i * 16;
        _PutU32( out, entry + 0, keyOffsets[i] );
        _PutU32( out, entry + 4, (u32)tables[i].key.size() );
        _PutU16( out, entry + 8, (u16)tables[i].fields.size() );
        _PutU16( out, entry + 10, tables[i].tableIndex );
        _PutU32( out, entry + 12, fieldOffsets[i] );
        memcpy( &out[fieldOffsets[i]], fieldBlobs[i].data(), fieldBlobs[i].size() );
    }
    memcpy( &out[stringsOffset], strings.data(), strings.size() );
    return out;
}

u32 _FloatBits( f32 value ) {
    union {
        f32 f;
        u32 u;
    } bits;
    bits.f = value;
    return bits.u;
}

CsvTable *_FindCsvTable( std::vector<CsvTable> &tables, const char *name ) {
    for( size_t i = 0; i < tables.size(); ++i ) {
        if( !_stricmp( tables[i].key.c_str(), name ) ) return &tables[i];
    }
    return NULL;
}

std::string _BuildMergedCsv( void ) {
    const u32 retailFields = 14 * 6;
    std::vector<CsvTable> base;
    BOOL haveBase = FALSE;
    u32 customLevel = 0;

    for( size_t p = 0; p < _config.profiles.size(); ++p ) {
        if( customLevel >= PC_MODS_LEVEL_MAX ) break;
        const Profile &profile = _config.profiles[p];
        if( profile.levels.empty() ) continue;
        const std::string path = _ResourcePath( profile, "multi_lvl$.csv" );
        std::string data;
        if( !_ReadWholeFile( path, data ) ) continue;
        std::vector<CsvTable> tables;
        if( !_ParseCsv( data, tables ) ) continue;
        CsvTable *table = _FindCsvTable( tables, "deathmatchlevels" );
        if( !table || table->fields.size() < retailFields ) continue;
        u32 wanted = (u32)profile.levels.size();
        if( wanted > PC_MODS_LEVEL_MAX - customLevel ) wanted = PC_MODS_LEVEL_MAX - customLevel;
        if( table->fields.size() < retailFields + wanted * 6 ) continue;

        std::vector<CsvField> customFields;
        customFields.reserve( wanted * 6 );
        for( u32 row = 0; row < wanted; ++row ) {
            for( u32 field = 0; field < 6; ++field ) {
                CsvField value = table->fields[retailFields + row * 6 + field];
                if( field == 5 ) {
                    // Merge-time ids are global across all discovered profiles.
                    value.type = 1;
                    value.number = _FloatBits( (f32)( PC_MODS_LEVEL_BASE + customLevel + 1 ) );
                }
                customFields.push_back( value );
            }
            ++customLevel;
        }

        if( !haveBase ) {
            base = tables;
            CsvTable *baseTable = _FindCsvTable( base, "deathmatchlevels" );
            if( !baseTable ) continue;
            baseTable->fields.resize( retailFields );
            baseTable->fields.insert( baseTable->fields.end(), customFields.begin(), customFields.end() );
            haveBase = TRUE;
        } else {
            CsvTable *baseTable = _FindCsvTable( base, "deathmatchlevels" );
            if( !baseTable ) break;
            baseTable->fields.insert( baseTable->fields.end(), customFields.begin(), customFields.end() );
        }
    }

    return haveBase ? _BuildCsv( base ) : std::string();
}

BOOL _EnsureMergedCsv( void ) {
    if( !_config.mergedBuilt ) {
        _config.mergedCsv = _BuildMergedCsv();
        _config.mergedBuilt = TRUE;
    }
    if( _config.mergedCsv.empty() ) return FALSE;
    if( _config.mergedPath.empty() ) {
        char temp[MAX_PATH];
        const DWORD length = GetTempPathA( sizeof( temp ), temp );
        if( !length || length >= sizeof( temp ) ) return FALSE;
        char path[1200];
        snprintf( path, sizeof( path ), "%sma_mod_%lu_multi_lvl.csv", temp,
            (unsigned long)GetCurrentProcessId() );
        if( !_WriteWholeFile( path, _config.mergedCsv ) ) return FALSE;
        _config.mergedPath = path;
        DEVPRINTF( "PORT-MODS merged multi_lvl$.csv %s\n", path );
    }
    return TRUE;
}

void _LoadProfileLevels( Profile &profile ) {
    const int count = GetPrivateProfileIntA( "levels", "count", 0, profile.ini.c_str() );
    const int limit = ( count < 0 ) ? 0 : ( count > PC_MODS_LEVEL_MAX ? PC_MODS_LEVEL_MAX : count );
    for( int i = 0; i < limit; ++i ) {
        PcModLevel level;
        memset( &level, 0, sizeof( level ) );
        char section[32];
        snprintf( section, sizeof( section ), "level.%d", i );
        GetPrivateProfileStringA( section, "title", "", level.title, sizeof( level.title ), profile.ini.c_str() );
        GetPrivateProfileStringA( section, "world", "", level.world, sizeof( level.world ), profile.ini.c_str() );
        GetPrivateProfileStringA( section, "csv", "", level.csv, sizeof( level.csv ), profile.ini.c_str() );
        GetPrivateProfileStringA( section, "materials", "ms_mp_01", level.materials, sizeof( level.materials ), profile.ini.c_str() );
        if( !level.title[0] || !_SafeResourceName( level.world ) ||
            !_SafeResourceName( level.csv ) || !_SafeResourceName( level.materials ) ) {
            DEVPRINTF( "PORT-MODS skipping invalid level %d in %s\n", i, profile.ini.c_str() );
            continue;
        }
        profile.levels.push_back( level );
    }
}

BOOL _AddProfile( const std::string &root ) {
    char iniPath[1200];
    snprintf( iniPath, sizeof( iniPath ), "%s\\runtime.ini", root.c_str() );
    if( !_FileExists( iniPath ) ) return FALSE;
    if( GetPrivateProfileIntA( "profile", "api", 0, iniPath ) != 1 ) return FALSE;

    Profile profile;
    profile.root = root;
    profile.ini = iniPath;
    _LoadProfileLevels( profile );
    _config.profiles.push_back( profile );
    if( !_config.summary.empty() ) _config.summary += "; ";
    _config.summary += root;
    DEVPRINTF( "PORT-MODS profile %s levels=%u\n", root.c_str(), (unsigned)profile.levels.size() );
    return TRUE;
}

std::string _NormalizeRoot( const char *root ) {
    if( !root || !root[0] ) return std::string();
    char full[MAX_PATH];
    const DWORD length = GetFullPathNameA( root, sizeof( full ), full, NULL );
    if( !length || length >= sizeof( full ) ) return std::string();
    return std::string( full );
}

void _DiscoverProfiles( const char *explicitRoot ) {
    _config.profiles.clear();
    _config.summary.clear();
    _config.enabled = FALSE;
    _config.mergedBuilt = FALSE;
    _config.mergedCsv.clear();
    _config.mergedPath.clear();
    _streams.clear();

    char envRoot[1024];
    const char *root = explicitRoot;
    if( !root || !root[0] ) {
        const DWORD count = GetEnvironmentVariableA( "MA_PORT_MOD_ROOT", envRoot, sizeof( envRoot ) );
        if( count && count < sizeof( envRoot ) ) root = envRoot;
    }
    if( root && root[0] ) {
        const std::string normalized = _NormalizeRoot( root );
        if( !normalized.empty() ) _AddProfile( normalized );
        _config.enabled = !_config.profiles.empty();
        return;
    }

    // No explicit profile: scan <exe>/mods. Profiles are loaded in lexical
    // order, so an install with both maps presents both level rows at once.
    char exePath[MAX_PATH];
    const DWORD exeLength = GetModuleFileNameA( NULL, exePath, sizeof( exePath ) );
    if( !exeLength || exeLength >= sizeof( exePath ) ) return;
    char *slash = strrchr( exePath, '\\' );
    char *forward = strrchr( exePath, '/' );
    if( forward > slash ) slash = forward;
    if( !slash ) return;
    *slash = '\0';
    char pattern[1200];
    snprintf( pattern, sizeof( pattern ), "%s\\mods\\*", exePath );
    WIN32_FIND_DATAA data;
    HANDLE find = FindFirstFileA( pattern, &data );
    if( find == INVALID_HANDLE_VALUE ) return;
    std::vector<std::string> roots;
    do {
        if( !( data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) ) continue;
        if( !strcmp( data.cFileName, "." ) || !strcmp( data.cFileName, ".." ) ) continue;
        char rootPath[1200];
        snprintf( rootPath, sizeof( rootPath ), "%s\\mods\\%s", exePath, data.cFileName );
        roots.push_back( rootPath );
    } while( FindNextFileA( find, &data ) );
    FindClose( find );
    std::sort( roots.begin(), roots.end() );
    for( size_t i = 0; i < roots.size(); ++i ) {
        _AddProfile( _NormalizeRoot( roots[i].c_str() ) );
    }
    _config.enabled = !_config.profiles.empty();
    if( _config.enabled ) {
        DEVPRINTF( "PORT-MODS scanned %s profiles=%u\n", pattern, (unsigned)_config.profiles.size() );
    }
}

Config &_Config( void ) {
    if( !_config.configured ) {
        _config.configured = TRUE;
        _DiscoverProfiles( NULL );
    }
    return _config;
}

} // namespace

void pcmods_Configure( const char *pszRoot ) {
    _config.configured = TRUE;
    _DiscoverProfiles( pszRoot );
}

void pcmods_Disable( void ) {
    _config.configured = TRUE;
    _config.enabled = FALSE;
    _config.mergedBuilt = FALSE;
    _config.profiles.clear();
    _config.summary.clear();
    _config.mergedCsv.clear();
    _config.mergedPath.clear();
    _streams.clear();
}

BOOL pcmods_IsActive( void ) {
    return _Config().enabled;
}

const char *pcmods_GetRoot( void ) {
    return _Config().enabled ? _config.summary.c_str() : "";
}

BOOL pcmods_ResolveFile( const char *requested, char *output, unsigned capacity ) {
    const Config &cfg = _Config();
    if( !cfg.enabled || !requested || !output || !capacity ) return FALSE;
    const char *name = _BaseName( requested );
    if( !_SafeResourceName( name ) ) return FALSE;
    // multi_lvl$.csv is synthesized from every active profile. Materialize it
    // once for callers that use ffile_Open instead of the stream API.
    if( !_stricmp( name, "multi_lvl$.csv" ) ) {
        if( !_EnsureMergedCsv() ) return FALSE;
        if( _config.mergedPath.size() >= 255 || _config.mergedPath.size() >= capacity ) return FALSE;
        strcpy( output, _config.mergedPath.c_str() );
        return TRUE;
    }

    for( size_t i = 0; i < cfg.profiles.size(); ++i ) {
        const std::string path = _ResourcePath( cfg.profiles[i], name );
        if( !_FileExists( path.c_str() ) ) continue;
        if( path.size() >= 255 || path.size() >= capacity ) return FALSE;
        strcpy( output, path.c_str() );
        return TRUE;
    }
    return FALSE;
}

const FFileMasterEntry_t *pcmods_OpenStream( const char *requested, u32 *size ) {
    const Config &cfg = _Config();
    if( !cfg.enabled || !requested ) return NULL;
    const char *name = _BaseName( requested );

    if( !_stricmp( name, "multi_lvl$.csv" ) ) {
        if( !_EnsureMergedCsv() ) return NULL;
        const std::string key = "<merged:multi_lvl$.csv>";
        for( std::list<StreamEntry>::iterator it = _streams.begin(); it != _streams.end(); ++it ) {
            if( it->path == key ) {
                if( size ) *size = it->entry.nNumBytes;
                return &it->entry;
            }
        }
        StreamEntry stream;
        stream.entry.nNumBytes = (u32)_config.mergedCsv.size();
        stream.path = key;
        stream.data = _config.mergedCsv;
        _streams.push_back( stream );
        if( size ) *size = _streams.back().entry.nNumBytes;
        DEVPRINTF( "PORT-MODS merged multi_lvl$.csv bytes=%u\n", (unsigned)_config.mergedCsv.size() );
        return &_streams.back().entry;
    }

    if( !_SafeResourceName( name ) ) return NULL;
    for( size_t i = 0; i < cfg.profiles.size(); ++i ) {
        const std::string path = _ResourcePath( cfg.profiles[i], name );
        if( !_FileExists( path.c_str() ) ) continue;
        for( std::list<StreamEntry>::iterator it = _streams.begin(); it != _streams.end(); ++it ) {
            if( !_stricmp( it->path.c_str(), path.c_str() ) ) {
                if( size ) *size = it->entry.nNumBytes;
                return &it->entry;
            }
        }
        WIN32_FILE_ATTRIBUTE_DATA info;
        if( !GetFileAttributesExA( path.c_str(), GetFileExInfoStandard, &info ) ||
            info.nFileSizeHigh || info.nFileSizeLow > 64u * 1024u * 1024u ) {
            return NULL;
        }
        StreamEntry stream;
        stream.entry.nNumBytes = info.nFileSizeLow;
        stream.path = path;
        _streams.push_back( stream );
        if( size ) *size = _streams.back().entry.nNumBytes;
        DEVPRINTF( "PORT-MODS stream override %s\n", path.c_str() );
        return &_streams.back().entry;
    }
    return NULL;
}

BOOL pcmods_ReadStream( const FFileMasterEntry_t *entry, u32 start, u32 amount,
    void *destination, FFileAsyncReadCallback_t *callback, void *user, s32 *result ) {
    for( std::list<StreamEntry>::const_iterator it = _streams.begin(); it != _streams.end(); ++it ) {
        if( &it->entry != entry ) continue;
        *result = FFILE_ERROR_RET;
        if( start > entry->nNumBytes || amount > entry->nNumBytes - start ) return TRUE;
        if( !it->data.empty() ) {
            memcpy( destination, it->data.data() + start, amount );
            *result = (s32)amount;
            if( callback ) callback( 0, FFILE_INVALID_HANDLE, destination, amount, user );
            return TRUE;
        }
        HANDLE file = CreateFileA( it->path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
        if( file == INVALID_HANDLE_VALUE ) return TRUE;
        DWORD read = 0;
        const BOOL ok = SetFilePointer( file, start, NULL, FILE_BEGIN ) != INVALID_SET_FILE_POINTER &&
            ReadFile( file, destination, amount, &read, NULL ) && read == amount;
        CloseHandle( file );
        if( ok ) {
            // The stream consumer has already set its in-flight state; complete
            // the loose override immediately, like a finished master-file read.
            *result = (s32)read;
            if( callback ) callback( 0, FFILE_INVALID_HANDLE, destination, read, user );
        }
        return TRUE;
    }
    return FALSE;
}

BOOL pcmods_Feature( const char *name ) {
    const Config &cfg = _Config();
    if( !cfg.enabled ) return FALSE;
    for( size_t i = 0; i < cfg.profiles.size(); ++i ) {
        if( GetPrivateProfileIntA( "features", name, 0, cfg.profiles[i].ini.c_str() ) != 0 ) return TRUE;
    }
    return FALSE;
}

unsigned pcmods_CustomLevelCount( void ) {
    const Config &cfg = _Config();
    unsigned count = 0;
    for( size_t i = 0; i < cfg.profiles.size(); ++i ) count += (unsigned)cfg.profiles[i].levels.size();
    return count > PC_MODS_LEVEL_MAX ? PC_MODS_LEVEL_MAX : count;
}

const PcModLevel *pcmods_CustomLevel( unsigned index ) {
    const Config &cfg = _Config();
    for( size_t i = 0; i < cfg.profiles.size(); ++i ) {
        const std::vector<PcModLevel> &levels = cfg.profiles[i].levels;
        if( index < levels.size() ) return &levels[index];
        index -= (unsigned)levels.size();
    }
    return NULL;
}

BOOL pcmods_IsCustomLevel( s32 levelId ) {
    return levelId >= PC_MODS_LEVEL_BASE &&
        (unsigned)levelId < PC_MODS_LEVEL_BASE + pcmods_CustomLevelCount();
}

BOOL pcmods_ResolveMedia( const char *requested, char *output, unsigned capacity ) {
    const Config &cfg = _Config();
    if( !cfg.enabled || !requested || !output || !capacity ) return FALSE;
    const char *name = _BaseName( requested );
    if( !name[0] || strlen( name ) > 79 || !strcmp( name, "." ) || !strcmp( name, ".." ) ) return FALSE;
    for( const char *p = name; *p; ++p ) {
        if( !( ( *p >= 'a' && *p <= 'z' ) || ( *p >= 'A' && *p <= 'Z' ) ||
               ( *p >= '0' && *p <= '9' ) || *p == '_' || *p == '.' || *p == '-' ) ) {
            return FALSE;
        }
    }
    for( size_t i = 0; i < cfg.profiles.size(); ++i ) {
        char path[1200];
        const int length = snprintf( path, sizeof( path ), "%s\\media\\%s", cfg.profiles[i].root.c_str(), name );
        if( length < 0 || length >= 255 || (unsigned)length >= sizeof( path ) ) continue;
        if( _FileExists( path ) ) {
            if( (unsigned)length >= capacity ) return FALSE;
            strcpy( output, path );
            return TRUE;
        }
    }
    return FALSE;
}

BOOL pcmods_HasMovies( void ) {
    const Config &cfg = _Config();
    if( !cfg.enabled ) return FALSE;
    for( size_t i = 0; i < cfg.profiles.size(); ++i ) {
        char pattern[1200];
        const int length = snprintf( pattern, sizeof( pattern ), "%s\\media\\*.bik", cfg.profiles[i].root.c_str() );
        if( length < 0 || length >= 255 ) continue;
        WIN32_FIND_DATAA data;
        HANDLE find = FindFirstFileA( pattern, &data );
        if( find == INVALID_HANDLE_VALUE ) continue;
        BOOL found = FALSE;
        do {
            if( !( data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) ) {
                found = TRUE;
                break;
            }
        } while( FindNextFileA( find, &data ) );
        FindClose( find );
        if( found ) return TRUE;
    }
    return FALSE;
}
