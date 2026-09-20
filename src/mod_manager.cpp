#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits.h>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <cwctype>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define USE_WINDOWS_DWORD
#include <windows.h>
#include <winhttp.h>
#include <direct.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#include "mod_manager.h"
#include "browser.h"
#include "c_dispatch.h"
#include "cmdlib.h"
#include "d_main.h"
#include "gameconfigfile.h"
#include "files.h"
#include "md5.h"

#ifdef __ANDROID__
#include "zandronum_android_host.h"
#endif

static const uint64_t MODMANAGER_MAX_FILE_SIZE = 512ULL * 1024ULL * 1024ULL;
static const uint64_t MODMANAGER_MAX_SEARCH_RESPONSE_SIZE = 2ULL * 1024ULL * 1024ULL;
static const uint64_t MODMANAGER_MAX_ARCHIVE_DIRECTORY_SIZE = 4ULL * 1024ULL * 1024ULL;
static const unsigned int MODMANAGER_MAX_ARCHIVE_ENTRIES = 4096;
static const unsigned int MODMANAGER_MAX_ARCHIVE_EXPANSION_RATIO = 100;
static const char *MODMANAGER_AFS_DIRECTORY = "https://static.allfearthesentinel.com/wads/";
static const unsigned int MODMANAGER_MAX_SOURCES = 8;
static const unsigned int MODMANAGER_MAX_SOURCE_VALUE = 1024;

enum EModDownloadSourceMode
{
	MODSOURCE_DirectBasename,
	MODSOURCE_FilenameSearch,
	MODSOURCE_HtmlIndex,
	MODSOURCE_IdgamesZip
};

enum EModArchiveMode
{
	MODARCHIVE_None,
	MODARCHIVE_ZipExactEntry
};

struct FModDownloadSource
{
	FString Label;
	FString BaseURL;
	EModDownloadSourceMode Mode;
	EModArchiveMode Archive;
	bool Enabled;
	bool AFSIdentity;
};

// Client recovery setting for moving the trusted static mod directory after an archive migration; server-advertised URLs never select this target.
CVAR( String, cl_mod_download_afs_url, MODMANAGER_AFS_DIRECTORY, CVAR_ARCHIVE | CVAR_NOSETBYACS | CVAR_GLOBALCONFIG )

static std::mutex g_ModManagerMutex;
static std::thread g_ModManagerThread;
static std::atomic<bool> g_ModManagerCancel(false);
static EModQueueState g_ModManagerState = MODQUEUE_Idle;
static bool g_ModManagerJoinAfter = false;
static bool g_ModManagerOfflineAfter = false;
static bool g_ModManagerReturnToRequirements = false;
static bool g_ModManagerPendingRestart = false;
static FString g_ModManagerQueueName;
static FString g_ModManagerLastError;
static uint64_t g_ModManagerReceived = 0;
static uint64_t g_ModManagerTotal = 0;
static int g_ModManagerActiveIndex = -1;
static FModJoinRequest g_ModManagerRequest;
static bool g_ModManagerHasRequest = false;
static FString g_ModManagerRestartAddress;
static FString g_ModManagerRestartIWAD;
static TArray<FString> g_ModManagerRestartFiles;
static bool g_ModManagerRestartConnect = true;
static unsigned int g_ModManagerTempSerial = 0;
static TArray<FModDownloadSource> g_ModManagerSources;
static bool g_ModManagerSourcesLoaded = false;
static bool g_ModManagerLegacyAFSOverride = false;

static bool modmanager_RecheckCurrentRequest(FString &error);

static bool modmanager_IsHexString(const FString &value)
{
	if ( value.Len() != 32 )
		return false;

	for ( int i = 0; i < value.Len(); ++i )
	{
		const char c = value[i];
		if ( !(( c >= '0' && c <= '9' ) || ( c >= 'a' && c <= 'f' ) || ( c >= 'A' && c <= 'F' )) )
			return false;
	}
	return true;
}

static bool modmanager_IsAllowedExtension(const char *name)
{
	const char *dot = strrchr( name, '.' );
	if ( dot == NULL )
		return false;

	return stricmp( dot, ".wad" ) == 0 || stricmp( dot, ".pk3" ) == 0 ||
		stricmp( dot, ".pk7" ) == 0 || stricmp( dot, ".deh" ) == 0 ||
		stricmp( dot, ".bex" ) == 0;
}

bool MODMANAGER_IsSafeBasename(const char *name)
{
	if ( name == NULL || name[0] == 0 || strlen( name ) > 128 )
		return false;
	if ( strcmp( name, "." ) == 0 || strcmp( name, ".." ) == 0 || !modmanager_IsAllowedExtension( name ) )
		return false;

	const size_t length = strlen( name );
	if ( name[length - 1] == '.' || name[length - 1] == ' ' )
		return false;
	for ( const unsigned char *p = reinterpret_cast<const unsigned char *>(name); *p != 0; ++p )
	{
		if ( *p < 32 || *p == 127 || *p == '/' || *p == '\\' || *p == ':' || *p == '%' ||
			*p == '?' || *p == '*' || *p == '"' || *p == '<' || *p == '>' || *p == '|' )
			return false;
	}
	return true;
}

#ifdef _WIN32
static std::wstring modmanager_ToWide(const char *value)
{
	if ( value == NULL )
		return std::wstring();
	int length = MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, NULL, 0 );
	UINT codePage = CP_UTF8;
	if ( length <= 0 )
	{
		length = MultiByteToWideChar( CP_ACP, 0, value, -1, NULL, 0 );
		codePage = CP_ACP;
	}
	if ( length <= 0 )
		return std::wstring();
	std::wstring result( length, L'\0' );
	if ( MultiByteToWideChar( codePage, 0, value, -1, &result[0], length ) <= 0 )
		return std::wstring();
	result.resize( length - 1 );
	return result;
}
#endif

static bool modmanager_CanonicalPath(const char *path, FString &result)
{
	if ( path == NULL || path[0] == 0 )
		return false;
#ifdef _WIN32
	const std::wstring widePath = modmanager_ToWide( path );
	if ( widePath.empty() )
		return false;
	HANDLE handle = CreateFileW( widePath.c_str(), FILE_READ_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS, NULL );
	if ( handle == INVALID_HANDLE_VALUE )
		return false;
	std::vector<wchar_t> buffer( 32768 );
	const DWORD length = GetFinalPathNameByHandleW( handle, &buffer[0], static_cast<DWORD>(buffer.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS );
	CloseHandle( handle );
	if ( length == 0 || length >= buffer.size() )
		return false;
	const int utf8Length = WideCharToMultiByte( CP_UTF8, 0, &buffer[0], static_cast<int>(length), NULL, 0, NULL, NULL );
	if ( utf8Length <= 0 )
		return false;
	std::string utf8( utf8Length, '\0' );
	if ( WideCharToMultiByte( CP_UTF8, 0, &buffer[0], static_cast<int>(length), &utf8[0], utf8Length, NULL, NULL ) <= 0 )
		return false;
	result = utf8.c_str();
#else
	char buffer[PATH_MAX];
	if ( realpath( path, buffer ) == NULL )
		return false;
	result = buffer;
#endif
	FixPathSeperator( result );
	while ( result.Len() > 1 && result[result.Len() - 1] == '/' )
		result = result.Left( result.Len() - 1 );
	return true;
}

static bool modmanager_PathEqual(const FString &left, const FString &right)
{
#ifdef _WIN32
	return left.CompareNoCase( right ) == 0;
#else
	return strcmp( left.GetChars(), right.GetChars() ) == 0;
#endif
}

static bool modmanager_IsWithinDirectory(const FString &directory, const FString &path)
{
	FString prefix = directory;
	if ( prefix.Len() == 0 || prefix[prefix.Len() - 1] != '/' )
		prefix += "/";
	if ( path.Len() <= prefix.Len() )
		return false;
#ifdef _WIN32
	return path.Left( prefix.Len() ).CompareNoCase( prefix ) == 0;
#else
	return strcmp( path.Left( prefix.Len() ).GetChars(), prefix.GetChars() ) == 0;
#endif
}

FString MODMANAGER_GetDirectory(bool create)
{
	FString workingDirectory;
#ifdef __ANDROID__
	workingDirectory = progdir;
	if ( workingDirectory.IsEmpty() )
		return "";
#else
	char cwd[PATH_MAX];
	if ( getcwd( cwd, sizeof( cwd ) ) == NULL )
		return "";
	workingDirectory = cwd;
#endif

	FString canonicalCwd;
	if ( !modmanager_CanonicalPath( workingDirectory.GetChars(), canonicalCwd ) )
		return "";
	FString expected;
	expected = canonicalCwd;
	if ( expected.Len() == 0 || expected[expected.Len() - 1] != '/' )
		expected += "/";
	expected += "mods";
	if ( create )
		CreatePath( expected.GetChars() );
	if ( !DirEntryExists( expected.GetChars() ) )
		return "";
	FString directory;
	if ( !modmanager_CanonicalPath( expected.GetChars(), directory ) || !modmanager_PathEqual( expected, directory ) )
		return "";
	return directory;
}

const char *MODMANAGER_FindManagedFile(const char *name)
{
	static char path[PATH_MAX];
	if ( !MODMANAGER_IsSafeBasename( name ) )
		return NULL;

	const FString directory = MODMANAGER_GetDirectory( false );
	if ( directory.IsEmpty() )
		return NULL;
	FString candidate;
	candidate.Format( "%s/%s", directory.GetChars(), name );
	FString canonicalFile;
	if ( !DirEntryExists( candidate.GetChars() ) || !modmanager_CanonicalPath( candidate.GetChars(), canonicalFile ) || !modmanager_IsWithinDirectory( directory, canonicalFile ) )
		return NULL;
	mysnprintf( path, countof( path ), "%s", canonicalFile.GetChars() );
	return path;
}

static bool modmanager_CanModifyManagedFiles(FString &error)
{
	std::lock_guard<std::mutex> lock( g_ModManagerMutex );
	if ( g_ModManagerState == MODQUEUE_Downloading || g_ModManagerState == MODQUEUE_Verifying )
	{
		error = "Downloads are active.";
		return false;
	}
	return true;
}

static uint64_t modmanager_GetFileSize(const FString &path)
{
	FILE *file = fopen( path.GetChars(), "rb" );
	if ( file == NULL ) return 0;
#ifdef _WIN32
	const bool valid = _fseeki64( file, 0, SEEK_END ) == 0;
	const __int64 size = valid ? _ftelli64( file ) : -1;
#else
	const bool valid = fseeko( file, 0, SEEK_END ) == 0;
	const off_t size = valid ? ftello( file ) : -1;
#endif
	fclose( file );
	return size > 0 ? static_cast<uint64_t>( size ) : 0;
}

void MODMANAGER_ListManagedMods(TArray<FManagedModFile> &files)
{
	files.Clear();
	const FString directory = MODMANAGER_GetDirectory( false );
	if ( directory.IsEmpty() ) return;
	TArray<FFileList> entries;
	ScanDirectory( entries, (directory + "/").GetChars() );
	const FString prefix = directory + "/";
	for ( unsigned int i = 0; i < entries.Size(); ++i )
	{
		if ( entries[i].isDirectory ) continue;
		const char *fullPath = entries[i].Filename.GetChars();
		if ( strncmp( fullPath, prefix.GetChars(), prefix.Len() ) != 0 ) continue;
		const char *name = fullPath + prefix.Len();
		if ( !MODMANAGER_IsSafeBasename( name ) ) continue;
		FString canonical;
		if ( !modmanager_CanonicalPath( fullPath, canonical ) || !modmanager_IsWithinDirectory( directory, canonical ) || !FileExists( canonical.GetChars() ) ) continue;
		FManagedModFile entry;
		entry.Name = name;
		entry.Size = modmanager_GetFileSize( canonical );
		files.Push( entry );
	}
}

bool MODMANAGER_DeleteManagedMod(const char *name, FString &error)
{
	if ( !MODMANAGER_IsSafeBasename( name ) ) { error = "Invalid managed mod name."; return false; }
	if ( !modmanager_CanModifyManagedFiles( error ) ) return false;
	const char *path = MODMANAGER_FindManagedFile( name );
	if ( path == NULL || !FileExists( path ) ) { error = "The mod is no longer available."; return false; }
	if ( std::remove( path ) != 0 ) { error = "Unable to delete the mod."; return false; }
	return true;
}

bool MODMANAGER_ClearManagedMods(FString &error)
{
	if ( !modmanager_CanModifyManagedFiles( error ) ) return false;
	TArray<FManagedModFile> files;
	MODMANAGER_ListManagedMods( files );
	for ( unsigned int i = 0; i < files.Size(); ++i )
	{
		if ( !MODMANAGER_DeleteManagedMod( files[i].Name.GetChars(), error ) ) return false;
	}
	return true;
}

static bool modmanager_IsOwnedTempName(const FString &tempName)
{
	const char *name = tempName.GetChars();
	const size_t length = strlen( name );
	if ( length < 19 || strncmp( name, "zandronum-mod-", 14 ) != 0 || strcmp( name + length - 5, ".part" ) != 0 )
		return false;
	for ( const unsigned char *p = reinterpret_cast<const unsigned char *>(name); *p != 0; ++p )
	{
		if ( *p < 32 || *p == '/' || *p == '\\' || *p == ':' || *p == '%' )
			return false;
	}
	return true;
}

static void modmanager_RemoveOwnedTemp(const FString &tempName)
{
	if ( !modmanager_IsOwnedTempName( tempName ) )
		return;
	const FString directory = MODMANAGER_GetDirectory( false );
	if ( directory.IsEmpty() )
		return;
	FString path;
	path.Format( "%s/%s", directory.GetChars(), tempName.GetChars() );
	FString canonicalPath;
	if ( !DirEntryExists( path.GetChars() ) || !modmanager_CanonicalPath( path.GetChars(), canonicalPath ) || !modmanager_IsWithinDirectory( directory, canonicalPath ) )
		return;
	std::remove( path.GetChars() );
}

static bool modmanager_IsRecognizedAFSDirectory(const char *value)
{
	if ( value == NULL )
		return false;

	FString normalized = value;
	while ( normalized.Len() > 0 && normalized[normalized.Len() - 1] == '/' )
		normalized = normalized.Left( normalized.Len() - 1 );

	return normalized.CompareNoCase( "https://static.allfearthesentinel.com/wads" ) == 0;
}

static bool modmanager_IsValidAFSOverride(const FString &value)
{
	if ( value.Len() < 9 || value.Len() > MODMANAGER_MAX_SOURCE_VALUE || value.Left( 8 ).CompareNoCase( "https://" ) != 0 )
		return false;
	if ( value[value.Len() - 1] != '/' || value.IndexOf( "?" ) >= 0 || value.IndexOf( "#" ) >= 0 || value.IndexOf( "%" ) >= 0 || value.IndexOf( "\\" ) >= 0 )
		return false;
	for ( int i = 0; i < value.Len(); ++i )
	{
		const unsigned char character = static_cast<unsigned char>(value[i]);
		if ( character <= 32 || character == 127 )
			return false;
	}

	const int pathStart = value.IndexOf( "/", 8 );
	const int authorityEnd = pathStart >= 0 ? pathStart : static_cast<int>(value.Len());
	if ( authorityEnd <= 8 || value.IndexOf( "@", 8 ) >= 0 )
		return false;

	const FString authority = value.Mid( 8, authorityEnd - 8 );
	int portStart = -1;
	if ( authority[0] == '[' )
	{
		const int closingBracket = authority.IndexOf( "]" );
		if ( closingBracket <= 1 )
			return false;
		if ( closingBracket + 1 < authority.Len() )
		{
			if ( authority[closingBracket + 1] != ':' )
				return false;
			portStart = closingBracket + 2;
		}
	}
	else
	{
		const int firstColon = authority.IndexOf( ":" );
		if ( firstColon >= 0 )
		{
			if ( authority.IndexOf( ":", firstColon + 1 ) >= 0 || firstColon == 0 )
				return false;
			portStart = firstColon + 1;
		}
	}
	if ( portStart >= 0 )
	{
		if ( portStart >= authority.Len() )
			return false;
		unsigned long port = 0;
		for ( int i = portStart; i < authority.Len(); ++i )
		{
			if ( authority[i] < '0' || authority[i] > '9' )
				return false;
			port = port * 10 + static_cast<unsigned long>(authority[i] - '0');
			if ( port > 65535 )
				return false;
		}
		if ( port == 0 )
			return false;
	}

	for ( int i = 0; i < authority.Len(); ++i )
	{
		const unsigned char character = static_cast<unsigned char>(authority[i]);
		if ( character <= 32 || character == 127 )
			return false;
	}

	for ( int i = authorityEnd; i + 1 < value.Len(); ++i )
	{
		if ( value[i] == '/' && value[i + 1] == '.' && ( i + 2 == value.Len() || value[i + 2] == '.' || value[i + 2] == '/' ) )
			return false;
	}
	return true;
}

static FString modmanager_EncodeBasename(const char *name)
{
	static const char hex[] = "0123456789ABCDEF";
	FString encoded;
	for ( const unsigned char *p = reinterpret_cast<const unsigned char *>(name); *p != 0; ++p )
	{
		if (( *p >= 'a' && *p <= 'z' ) || ( *p >= 'A' && *p <= 'Z' ) ||
			( *p >= '0' && *p <= '9' ) || *p == '-' || *p == '_' || *p == '.' || *p == '~' )
		{
			encoded += static_cast<char>(*p);
		}
		else
		{
			encoded.AppendFormat( "%%%c%c", hex[*p >> 4], hex[*p & 15] );
		}
	}
	return encoded;
}

static bool modmanager_IsStrictBoolean(const char *value, bool &result)
{
	if ( value == NULL )
		return false;
	if ( stricmp( value, "true" ) == 0 || strcmp( value, "1" ) == 0 )
	{
		result = true;
		return true;
	}
	if ( stricmp( value, "false" ) == 0 || strcmp( value, "0" ) == 0 )
	{
		result = false;
		return true;
	}
	return false;
}

static bool modmanager_IsValidFilenameSearchTemplate(const FString &value)
{
	static const char *token = "%WADNAME%";
	if ( value.IsEmpty() || value.Len() > MODMANAGER_MAX_SOURCE_VALUE || value.Left( 8 ).CompareNoCase( "https://" ) != 0 )
		return false;
	for ( int i = 0; i < value.Len(); ++i )
	{
		const unsigned char character = static_cast<unsigned char>(value[i]);
		if ( character <= 32 || character == 127 || character == '\\' || character == '#' )
			return false;
	}
	const int query = value.IndexOf( "?" );
	const int firstToken = value.IndexOf( token );
	if ( query < 0 || firstToken <= query || firstToken < 0 )
		return false;
	if ( value.IndexOf( token, firstToken + static_cast<int>(strlen( token )) ) >= 0 )
		return false;
	for ( int i = 0; i < value.Len(); ++i )
	{
		if ( value[i] == '%' && ( i < firstToken || i >= firstToken + static_cast<int>(strlen( token )) ) )
			return false;
	}
	FString authorityAndPath = value.Left( query );
	if ( authorityAndPath[authorityAndPath.Len() - 1] != '/' )
		authorityAndPath += "/";
	return modmanager_IsValidAFSOverride( authorityAndPath );
}

static bool modmanager_IsValidHTTPSPage(const FString &value)
{
	if ( value.Len() < 9 || value.Len() > MODMANAGER_MAX_SOURCE_VALUE || value.Left( 8 ).CompareNoCase( "https://" ) != 0 )
		return false;
	if ( value.IndexOf( "#" ) >= 0 || value.IndexOf( "%" ) >= 0 || value.IndexOf( "\\" ) >= 0 )
		return false;
	for ( int i = 0; i < value.Len(); ++i )
	{
		const unsigned char character = static_cast<unsigned char>(value[i]);
		if ( character <= 32 || character == 127 )
			return false;
	}
	const int pathStart = value.IndexOf( "/", 8 );
	if ( pathStart < 0 )
		return false;
	const FString origin = value.Left( pathStart );
	if ( !modmanager_IsValidAFSOverride( origin + "/" ) )
		return false;
	int pathEnd = value.IndexOf( "?" );
	if ( pathEnd < 0 )
		pathEnd = static_cast<int>( value.Len() );
	const FString path = value.Left( pathEnd );
	for ( int i = pathStart; i + 1 < path.Len(); ++i )
	{
		if ( path[i] == '/' && path[i + 1] == '.' && ( i + 2 == path.Len() || path[i + 2] == '.' || path[i + 2] == '/' ) )
			return false;
	}
	return true;
}

static bool modmanager_IsSafeArchiveBasename(const char *name)
{
	if ( name == NULL || name[0] == 0 || strlen( name ) > 128 )
		return false;
	const char *dot = strrchr( name, '.' );
	if ( dot == NULL || stricmp( dot, ".zip" ) != 0 )
		return false;
	const size_t length = strlen( name );
	if ( name[length - 1] == '.' || name[length - 1] == ' ' )
		return false;
	for ( const unsigned char *p = reinterpret_cast<const unsigned char *>(name); *p != 0; ++p )
	{
		if ( *p < 32 || *p == 127 || *p == '/' || *p == '\\' || *p == ':' || *p == '%' ||
			*p == '?' || *p == '*' || *p == '"' || *p == '<' || *p == '>' || *p == '|' )
			return false;
	}
	return true;
}

static bool modmanager_BuildIdgamesArchiveName(const char *name, FString &archiveName)
{
	if ( !MODMANAGER_IsSafeBasename( name ) )
		return false;
	const char *dot = strrchr( name, '.' );
	if ( dot == NULL )
		return false;
	archiveName = FString( name ).Left( static_cast<int>( dot - name ) ) + ".zip";
	return modmanager_IsSafeArchiveBasename( archiveName.GetChars() );
}

static const char *modmanager_SourceModeName(EModDownloadSourceMode mode)
{
	switch ( mode )
	{
	case MODSOURCE_FilenameSearch: return "filename-search";
	case MODSOURCE_HtmlIndex: return "html-index";
	case MODSOURCE_IdgamesZip: return "idgames-zip";
	default: return "direct-basename";
	}
}

static const char *modmanager_ArchiveModeName(EModArchiveMode mode)
{
	return mode == MODARCHIVE_ZipExactEntry ? "zip-exact-entry" : "none";
}

static bool modmanager_IsValidSourceURL(const FString &value, EModDownloadSourceMode mode)
{
	if ( mode == MODSOURCE_DirectBasename )
		return modmanager_IsValidAFSOverride( value );
	if ( mode == MODSOURCE_FilenameSearch || mode == MODSOURCE_IdgamesZip )
		return modmanager_IsValidFilenameSearchTemplate( value );
	return modmanager_IsValidHTTPSPage( value );
}

static void modmanager_AddDefaultSource(const char *label, const char *url, EModDownloadSourceMode mode, EModArchiveMode archive, bool afsIdentity)
{
	FModDownloadSource source;
	source.Label = label;
	source.BaseURL = url;
	source.Mode = mode;
	source.Archive = archive;
	source.Enabled = true;
	source.AFSIdentity = afsIdentity;
	g_ModManagerSources.Push( source );
}

static void modmanager_SetDefaultSourceConfig(unsigned int index, const FModDownloadSource &source)
{
	if ( GameConfig == NULL )
		return;
	FString key;
	key.Format( "Source%u.Enabled", index + 1 );
	GameConfig->SetValueForKey( key.GetChars(), source.Enabled ? "true" : "false" );
	key.Format( "Source%u.Mode", index + 1 );
	GameConfig->SetValueForKey( key.GetChars(), modmanager_SourceModeName( source.Mode ) );
	key.Format( "Source%u.BaseURL", index + 1 );
	GameConfig->SetValueForKey( key.GetChars(), source.BaseURL.GetChars() );
	key.Format( "Source%u.Archive", index + 1 );
	GameConfig->SetValueForKey( key.GetChars(), modmanager_ArchiveModeName( source.Archive ) );
}

static void modmanager_DisableSource(unsigned int index, const char *reason)
{
	if ( index >= g_ModManagerSources.Size() )
		return;
	g_ModManagerSources[index].Enabled = false;
	if ( developer ) Printf( "ModDownloads: disabled source %u (%s): %s\n", index + 1, g_ModManagerSources[index].Label.GetChars(), reason );
}

static void modmanager_LoadSources()
{
	if ( g_ModManagerSourcesLoaded )
		return;
	g_ModManagerSourcesLoaded = true;
	g_ModManagerSources.Clear();
	modmanager_AddDefaultSource( "AFS", MODMANAGER_AFS_DIRECTORY, MODSOURCE_DirectBasename, MODARCHIVE_None, true );
	modmanager_AddDefaultSource( "Firestick", "https://wads.firestick.games/", MODSOURCE_DirectBasename, MODARCHIVE_None, false );
	modmanager_AddDefaultSource( "Euroboros", "https://euroboros.net/zandronum/wads/", MODSOURCE_DirectBasename, MODARCHIVE_None, false );
	modmanager_AddDefaultSource( "Audrealms", "https://static.audrealms.org/wads/", MODSOURCE_DirectBasename, MODARCHIVE_None, false );
	modmanager_AddDefaultSource( "DogSoft", "https://doom.dogsoft.net/getwad.php?search=%WADNAME%", MODSOURCE_FilenameSearch, MODARCHIVE_None, false );
	modmanager_AddDefaultSource( "Doomshack", "https://doomshack.org/wadlist.php", MODSOURCE_HtmlIndex, MODARCHIVE_None, false );
	modmanager_AddDefaultSource( "idgames", "https://www.doomworld.com/idgames/api/api.php?out=json&action=search&query=%WADNAME%&dir=desc", MODSOURCE_IdgamesZip, MODARCHIVE_ZipExactEntry, false );

	const bool hadSection = GameConfig != NULL && GameConfig->SetSection( "ModDownloads" );
	if ( GameConfig == NULL )
	{
		const FString legacyAFS = cl_mod_download_afs_url.GetGenericRep( CVAR_String ).String;
		if ( modmanager_IsValidAFSOverride( legacyAFS ) && legacyAFS.CompareNoCase( MODMANAGER_AFS_DIRECTORY ) != 0 && g_ModManagerSources.Size() > 0 && g_ModManagerSources[0].Enabled && g_ModManagerSources[0].BaseURL.CompareNoCase( MODMANAGER_AFS_DIRECTORY ) == 0 )
		{
			g_ModManagerSources[0].BaseURL = legacyAFS;
			g_ModManagerLegacyAFSOverride = true;
		}
		return;
	}
	if ( !hadSection )
	{
		GameConfig->SetSection( "ModDownloads", true );
		for ( unsigned int i = 0; i < g_ModManagerSources.Size(); ++i )
			modmanager_SetDefaultSourceConfig( i, g_ModManagerSources[i] );
	}
	else for ( unsigned int i = 0; i < MODMANAGER_MAX_SOURCES; ++i )
	{
		FString key;
		key.Format( "Source%u.Enabled", i + 1 );
		const char *enabledValue = GameConfig->GetValueForKey( key.GetChars() );
		key.Format( "Source%u.Mode", i + 1 );
		const char *modeValue = GameConfig->GetValueForKey( key.GetChars() );
		key.Format( "Source%u.BaseURL", i + 1 );
		const char *baseValue = GameConfig->GetValueForKey( key.GetChars() );
		key.Format( "Source%u.Archive", i + 1 );
		const char *archiveValue = GameConfig->GetValueForKey( key.GetChars() );
		const bool hasRecord = enabledValue != NULL || modeValue != NULL || baseValue != NULL || archiveValue != NULL;
		if ( !hasRecord )
			continue;
		if ( i >= g_ModManagerSources.Size() )
		{
			FModDownloadSource source;
			source.Label.Format( "Source%u", i + 1 );
			source.BaseURL = "";
			source.Mode = MODSOURCE_DirectBasename;
			source.Archive = MODARCHIVE_None;
			source.Enabled = false;
			source.AFSIdentity = false;
			g_ModManagerSources.Push( source );
		}

		bool enabled = false;
		if ( !modmanager_IsStrictBoolean( enabledValue, enabled ) || modeValue == NULL || baseValue == NULL || archiveValue == NULL )
		{
			modmanager_DisableSource( i, "incomplete or unsupported record" );
			continue;
		}
		EModDownloadSourceMode mode;
		if ( stricmp( modeValue, "direct-basename" ) == 0 )
			mode = MODSOURCE_DirectBasename;
		else if ( stricmp( modeValue, "filename-search" ) == 0 )
			mode = MODSOURCE_FilenameSearch;
		else if ( stricmp( modeValue, "html-index" ) == 0 )
			mode = MODSOURCE_HtmlIndex;
		else if ( stricmp( modeValue, "idgames-zip" ) == 0 )
			mode = MODSOURCE_IdgamesZip;
		else
		{
			modmanager_DisableSource( i, "unknown mode" );
			continue;
		}
		EModArchiveMode archive;
		if ( stricmp( archiveValue, "none" ) == 0 )
			archive = MODARCHIVE_None;
		else if ( stricmp( archiveValue, "zip-exact-entry" ) == 0 )
			archive = MODARCHIVE_ZipExactEntry;
		else
		{
			modmanager_DisableSource( i, "unknown archive mode" );
			continue;
		}
		if ( ( mode == MODSOURCE_IdgamesZip ) != ( archive == MODARCHIVE_ZipExactEntry ) )
		{
			modmanager_DisableSource( i, "archive mode does not match source mode" );
			continue;
		}
		const FString base = baseValue;
		if ( base.Len() > MODMANAGER_MAX_SOURCE_VALUE || !modmanager_IsValidSourceURL( base, mode ) )
		{
			modmanager_DisableSource( i, "invalid HTTPS source URL" );
			continue;
		}
		g_ModManagerSources[i].Enabled = enabled;
		g_ModManagerSources[i].Mode = mode;
		g_ModManagerSources[i].Archive = archive;
		g_ModManagerSources[i].BaseURL = base;
	}

	const FString legacyAFS = cl_mod_download_afs_url.GetGenericRep( CVAR_String ).String;
	if ( modmanager_IsValidAFSOverride( legacyAFS ) && legacyAFS.CompareNoCase( MODMANAGER_AFS_DIRECTORY ) != 0 && g_ModManagerSources.Size() > 0 && g_ModManagerSources[0].Enabled )
	{
		if ( g_ModManagerSources[0].BaseURL.CompareNoCase( MODMANAGER_AFS_DIRECTORY ) == 0 )
		{
			g_ModManagerSources[0].BaseURL = legacyAFS;
			g_ModManagerLegacyAFSOverride = true;
			if ( GameConfig != NULL )
				GameConfig->SetValueForKey( "Source1.BaseURL", legacyAFS.GetChars() );
		}
	}
}

static bool modmanager_HasCandidate(const TArray<FString> &urls, const FString &url)
{
	for ( unsigned int i = 0; i < urls.Size(); ++i )
	{
		if ( urls[i].CompareNoCase( url ) == 0 )
			return true;
	}
	return false;
}

static bool modmanager_BuildSourceCandidates(const char *serverURL, const char *name, TArray<FString> &urls, TArray<FString> &labels, TArray<int> &modes)
{
	urls.Clear();
	labels.Clear();
	modes.Clear();
	if ( !MODMANAGER_IsSafeBasename( name ) )
		return false;
	modmanager_LoadSources();
	const FString encodedName = modmanager_EncodeBasename( name );
	for ( unsigned int pass = 0; pass < 2; ++pass )
	{
		for ( unsigned int i = 0; i < g_ModManagerSources.Size(); ++i )
		{
			const FModDownloadSource &source = g_ModManagerSources[i];
			const bool recognizedAFS = modmanager_IsRecognizedAFSDirectory( serverURL );
			if ( source.AFSIdentity && g_ModManagerLegacyAFSOverride && !recognizedAFS )
				continue;
			if ( !source.Enabled || ( pass == 0 && ( !source.AFSIdentity || !recognizedAFS ) ) || ( pass == 1 && source.AFSIdentity && recognizedAFS ) )
				continue;
			FString candidate;
			if ( source.Mode == MODSOURCE_DirectBasename )
				candidate = source.BaseURL + encodedName;
			else if ( source.Mode == MODSOURCE_HtmlIndex )
				candidate = source.BaseURL;
			else
			{
				const int token = source.BaseURL.IndexOf( "%WADNAME%" );
				if ( token < 0 )
					continue;
				FString replacement = encodedName;
				if ( source.Mode == MODSOURCE_IdgamesZip )
				{
					FString archiveName;
					if ( !modmanager_BuildIdgamesArchiveName( name, archiveName ) )
						continue;
					replacement = modmanager_EncodeBasename( archiveName.GetChars() );
				}
				candidate = source.BaseURL.Left( token ) + replacement + source.BaseURL.Mid( token + 9 );
			}
			if ( !modmanager_HasCandidate( urls, candidate ) )
			{
				urls.Push( candidate );
				labels.Push( source.Label );
				modes.Push( static_cast<int>(source.Mode) );
			}
		}
	}
	return urls.Size() > 0;
}

static void modmanager_InitializeRequirement(FModRequirement &requirement)
{
	requirement.Status = MODREQ_Unknown;
	requirement.DownloadState = MODDL_NotNeeded;
	requirement.DownloadedBytes = 0;
	requirement.DownloadTotal = 0;
	requirement.Optional = false;
	requirement.Downloadable = false;
	requirement.SourceURL = "";
	requirement.SourceName = "";
	requirement.SourceURLs.Clear();
	requirement.SourceNames.Clear();
	requirement.SourceModes.Clear();
}

static bool modmanager_ResolveRequest(FModJoinRequest &request, FString &error)
{
	TArray<FString> resolvedFiles;
	for ( unsigned int i = 0; i < request.Requirements.Size(); ++i )
	{
		FModRequirement &requirement = request.Requirements[i];
		requirement.ResolvedPath = "";
		requirement.SourceURL = "";
		requirement.SourceName = "";
		requirement.SourceURLs.Clear();
		requirement.SourceNames.Clear();
		requirement.SourceModes.Clear();
		requirement.Downloadable = false;
		requirement.Error = "";
		requirement.DownloadState = MODDL_NotNeeded;
		requirement.DownloadedBytes = 0;
		requirement.DownloadTotal = 0;

		if ( !MODMANAGER_IsSafeBasename( requirement.Name.GetChars() ) )
		{
			requirement.Status = MODREQ_Incompatible;
			requirement.Error = "unsafe filename";
			continue;
		}

		if ( D_AddFile( resolvedFiles, requirement.Name.GetChars() ) )
		{
			requirement.ResolvedPath = resolvedFiles[resolvedFiles.Size() - 1];
			if ( !modmanager_IsHexString( requirement.ExpectedMD5 ) )
			{
				requirement.Status = MODREQ_Unknown;
				requirement.Error = "missing or invalid server checksum";
			}
			else
			{
				char digest[33];
				if ( MD5SumOfFile( requirement.ResolvedPath.GetChars(), digest ) && stricmp( digest, requirement.ExpectedMD5.GetChars() ) == 0 )
					requirement.Status = MODREQ_Found;
				else
				{
					requirement.Status = MODREQ_Incompatible;
					requirement.Error = "checksum mismatch";
				}
			}
		}
		else
			requirement.Status = MODREQ_Missing;
		if ( requirement.Status != MODREQ_Found && !modmanager_IsHexString( requirement.ExpectedMD5 ) )
		{
			requirement.Status = MODREQ_Unknown;
			requirement.Error = "missing or invalid server checksum";
		}
		if ( requirement.Status == MODREQ_Found )
			requirement.DownloadState = MODDL_Downloaded;
		if ( requirement.Status != MODREQ_Found && modmanager_IsHexString( requirement.ExpectedMD5 ) )
		{
			const char *managed = MODMANAGER_FindManagedFile( requirement.Name.GetChars() );
			if ( managed != NULL )
			{
				char digest[33];
				if ( MD5SumOfFile( managed, digest ) && stricmp( digest, requirement.ExpectedMD5.GetChars() ) == 0 )
				{
					requirement.ResolvedPath = managed;
					requirement.Status = MODREQ_Found;
					requirement.Error = "";
					requirement.DownloadState = MODDL_Downloaded;
				}
			}
		}

		if ( requirement.Status == MODREQ_Missing || requirement.Status == MODREQ_Incompatible )
		{
			modmanager_BuildSourceCandidates( request.ServerURL.GetChars(), requirement.Name.GetChars(), requirement.SourceURLs, requirement.SourceNames, requirement.SourceModes );
			if ( requirement.SourceURLs.Size() > 0 && modmanager_IsHexString( requirement.ExpectedMD5 ) )
			{
				requirement.Downloadable = true;
				requirement.SourceURL = requirement.SourceURLs[0];
				requirement.SourceName = requirement.SourceNames[0];
			}
			else if ( requirement.SourceURLs.Size() == 0 )
			{
				if ( requirement.Error.IsEmpty() )
					requirement.Error = "no approved download source";
				else
					requirement.Error += "; no approved download source";
			}
		}
	}

	if ( request.IWADName.IsEmpty() )
		error = "The server did not provide an IWAD name.";
	return error.IsEmpty();
}

static void modmanager_SetQueueFailure(const FString &error)
{
	std::lock_guard<std::mutex> lock( g_ModManagerMutex );
	g_ModManagerState = MODQUEUE_Failed;
	g_ModManagerLastError = error;
	g_ModManagerQueueName = "";
	if ( g_ModManagerActiveIndex >= 0 && static_cast<unsigned int>(g_ModManagerActiveIndex) < g_ModManagerRequest.Requirements.Size() )
	{
		FModRequirement &requirement = g_ModManagerRequest.Requirements[g_ModManagerActiveIndex];
		requirement.DownloadState = MODDL_Failed;
		requirement.Error = error;
	}
}

static void modmanager_SetItemState(unsigned int index, EModDownloadState state, uint64_t received, uint64_t total, const FString &error)
{
	std::lock_guard<std::mutex> lock( g_ModManagerMutex );
	if ( index >= g_ModManagerRequest.Requirements.Size() )
		return;
	FModRequirement &requirement = g_ModManagerRequest.Requirements[index];
	requirement.DownloadState = state;
	requirement.DownloadedBytes = received;
	requirement.DownloadTotal = total;
	requirement.Error = error;
}

#ifdef _WIN32
static bool modmanager_PromoteFile(const FString &tempPath, const FString &finalPath, FString &error)
{
	const std::wstring wideTempPath = modmanager_ToWide( tempPath.GetChars() );
	const std::wstring wideFinalPath = modmanager_ToWide( finalPath.GetChars() );
	if ( wideTempPath.empty() || wideFinalPath.empty() || !MoveFileExW( wideTempPath.c_str(), wideFinalPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH ) )
	{
		error = "Unable to promote the verified download.";
		return false;
	}
	return true;
}
#else
static bool modmanager_PromoteFile(const FString &tempPath, const FString &finalPath, FString &error)
{
	if ( std::rename( tempPath.GetChars(), finalPath.GetChars() ) != 0 )
	{
		error = "Unable to promote the verified download.";
		return false;
	}
	return true;
}
#endif

#ifdef _WIN32
static bool modmanager_IsExpectedContentRange(const wchar_t *value, uint64_t start, uint64_t end, uint64_t total)
{
	if ( value == NULL )
		return false;

	unsigned long long parsedStart = 0;
	unsigned long long parsedEnd = 0;
	unsigned long long parsedTotal = 0;
	int consumed = 0;
	if ( swscanf( value, L"bytes %llu-%llu/%llu%n", &parsedStart, &parsedEnd, &parsedTotal, &consumed ) != 3 )
		return false;
	while ( value[consumed] != L'\0' && iswspace( value[consumed] ) )
		++consumed;
	return value[consumed] == L'\0' && parsedStart == static_cast<unsigned long long>(start) &&
		parsedEnd == static_cast<unsigned long long>(end) && parsedTotal == static_cast<unsigned long long>(total);
}

static bool modmanager_DownloadWindowsSingle(const FString &url, const FString &tempName, uint64_t maxBytes, FString &error)
{
	std::wstring wideURL;
	for ( int i = 0; i < url.Len(); ++i )
		wideURL.push_back( static_cast<wchar_t>(static_cast<unsigned char>(url[i])) );

	URL_COMPONENTS components;
	memset( &components, 0, sizeof(components) );
	components.dwStructSize = sizeof(components);
	wchar_t hostBuffer[1024] = {};
	wchar_t pathBuffer[2048] = {};
	wchar_t extraBuffer[2048] = {};
	components.lpszHostName = hostBuffer;
	components.dwHostNameLength = countof( hostBuffer );
	components.lpszUrlPath = pathBuffer;
	components.dwUrlPathLength = countof( pathBuffer );
	components.lpszExtraInfo = extraBuffer;
	components.dwExtraInfoLength = countof( extraBuffer );
	if ( !WinHttpCrackUrl( wideURL.c_str(), 0, 0, &components ) || components.nScheme != INTERNET_SCHEME_HTTPS )
	{
		error = "The download URL is not HTTPS.";
		return false;
	}

	const std::wstring host( components.lpszHostName, components.dwHostNameLength );
	std::wstring path( components.lpszUrlPath, components.dwUrlPathLength );
	path.append( components.lpszExtraInfo, components.dwExtraInfoLength );
	const FString directory = MODMANAGER_GetDirectory( true );
	if ( directory.IsEmpty() )
	{
		error = "The managed mods directory is unavailable.";
		return false;
	}
	FString destination;
	destination.Format( "%s/%s", directory.GetChars(), tempName.GetChars() );

	HINTERNET session = WinHttpOpen( L"Zandronum Mod Manager", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0 );
	if ( session == NULL )
	{
		error = "Unable to initialize HTTPS transport.";
		return false;
	}
	WinHttpSetTimeouts( session, 15000, 15000, 15000, 15000 );
	HINTERNET connection = WinHttpConnect( session, host.c_str(), components.nPort, 0 );
	HINTERNET request = connection != NULL ? WinHttpOpenRequest( connection, L"GET", path.empty() ? L"/" : path.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE ) : NULL;
	if ( request == NULL )
	{
		if ( connection != NULL ) WinHttpCloseHandle( connection );
		WinHttpCloseHandle( session );
		error = "Unable to open HTTPS request.";
		return false;
	}
#ifdef WINHTTP_OPTION_REDIRECT_POLICY
	DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
	WinHttpSetOption( request, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy) );
#endif

	bool success = false;
	FILE *output = NULL;
	if ( WinHttpSendRequest( request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0 ) && WinHttpReceiveResponse( request, NULL ) )
	{
		DWORD status = 0;
		DWORD statusSize = sizeof(status);
		if ( WinHttpQueryHeaders( request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX ) && status == 200 )
		{
			uint64_t expectedSize = 0;
			wchar_t sizeBuffer[64];
			DWORD sizeBufferBytes = sizeof(sizeBuffer);
			if ( WinHttpQueryHeaders( request, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, sizeBuffer, &sizeBufferBytes, WINHTTP_NO_HEADER_INDEX ) )
				expectedSize = _wcstoui64( sizeBuffer, NULL, 10 );
			if ( expectedSize == 0 || expectedSize <= maxBytes )
			{
				output = fopen( destination.GetChars(), "wb" );
				if ( output != NULL )
				{
					uint64_t received = 0;
					BYTE buffer[16384];
					DWORD bytesRead = 0;
					bool readOK = true;
					while ( !g_ModManagerCancel.load() && WinHttpReadData( request, buffer, sizeof(buffer), &bytesRead ) && bytesRead > 0 )
					{
						if ( received > maxBytes - bytesRead || fwrite( buffer, 1, bytesRead, output ) != bytesRead )
						{
							readOK = false;
							break;
						}
						received += bytesRead;
						MODMANAGER_ReportPlatformProgress( received, expectedSize );
					}
					if ( g_ModManagerCancel.load() )
						readOK = false;
					fclose( output );
					output = NULL;
					success = readOK && received > 0 && ( expectedSize == 0 || received == expectedSize );
				}
			}
			else
				error = "The download is larger than the allowed limit.";
		}
		else
			error = "The HTTPS server did not return the requested file.";
	}
	else
		error = "The HTTPS request failed.";

	if ( output != NULL ) fclose( output );
	WinHttpCloseHandle( request );
	WinHttpCloseHandle( connection );
	WinHttpCloseHandle( session );
	if ( !success && error.IsEmpty() )
		error = g_ModManagerCancel.load() ? "Download cancelled." : "The download was incomplete.";
	return success;
}

static bool modmanager_QueryWindowsRangeInfo(const FString &url, uint64_t &size)
{
	std::wstring wideURL;
	for ( int i = 0; i < url.Len(); ++i )
		wideURL.push_back( static_cast<wchar_t>(static_cast<unsigned char>(url[i])) );

	URL_COMPONENTS components;
	memset( &components, 0, sizeof(components) );
	components.dwStructSize = sizeof(components);
	wchar_t hostBuffer[1024] = {};
	wchar_t pathBuffer[2048] = {};
	wchar_t extraBuffer[2048] = {};
	components.lpszHostName = hostBuffer;
	components.dwHostNameLength = countof( hostBuffer );
	components.lpszUrlPath = pathBuffer;
	components.dwUrlPathLength = countof( pathBuffer );
	components.lpszExtraInfo = extraBuffer;
	components.dwExtraInfoLength = countof( extraBuffer );
	if ( !WinHttpCrackUrl( wideURL.c_str(), 0, 0, &components ) || components.nScheme != INTERNET_SCHEME_HTTPS )
		return false;

	const std::wstring host( components.lpszHostName, components.dwHostNameLength );
	std::wstring path( components.lpszUrlPath, components.dwUrlPathLength );
	path.append( components.lpszExtraInfo, components.dwExtraInfoLength );
	HINTERNET session = WinHttpOpen( L"Zandronum Mod Manager", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0 );
	if ( session == NULL )
		return false;
	WinHttpSetTimeouts( session, 15000, 15000, 15000, 15000 );
	HINTERNET connection = WinHttpConnect( session, host.c_str(), components.nPort, 0 );
	HINTERNET request = connection != NULL ? WinHttpOpenRequest( connection, L"HEAD", path.empty() ? L"/" : path.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE ) : NULL;
	if ( request == NULL )
	{
		if ( connection != NULL ) WinHttpCloseHandle( connection );
		WinHttpCloseHandle( session );
		return false;
	}
#ifdef WINHTTP_OPTION_REDIRECT_POLICY
	DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
	WinHttpSetOption( request, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy) );
#endif

	bool supported = false;
	if ( WinHttpSendRequest( request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0 ) && WinHttpReceiveResponse( request, NULL ) )
	{
		DWORD status = 0;
		DWORD statusSize = sizeof(status);
		wchar_t ranges[32] = {};
		DWORD rangesSize = sizeof(ranges);
		wchar_t contentLength[64] = {};
		DWORD contentLengthSize = sizeof(contentLength);
		if ( WinHttpQueryHeaders( request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX ) &&
			status == 200 &&
			WinHttpQueryHeaders( request, WINHTTP_QUERY_ACCEPT_RANGES, WINHTTP_HEADER_NAME_BY_INDEX, ranges, &rangesSize, WINHTTP_NO_HEADER_INDEX ) &&
			_wcsicmp( ranges, L"bytes" ) == 0 &&
			WinHttpQueryHeaders( request, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, contentLength, &contentLengthSize, WINHTTP_NO_HEADER_INDEX ) )
		{
			size = _wcstoui64( contentLength, NULL, 10 );
			supported = size > 0;
		}
	}
	WinHttpCloseHandle( request );
	WinHttpCloseHandle( connection );
	WinHttpCloseHandle( session );
	return supported;
}

static bool modmanager_DownloadWindowsRange(const FString &url, const FString &tempPath, uint64_t start, uint64_t end, uint64_t total,
	std::atomic<uint64_t> &received, std::atomic<bool> &rangeFailed)
{
	std::wstring wideURL;
	for ( int i = 0; i < url.Len(); ++i )
		wideURL.push_back( static_cast<wchar_t>(static_cast<unsigned char>(url[i])) );
	URL_COMPONENTS components;
	memset( &components, 0, sizeof(components) );
	components.dwStructSize = sizeof(components);
	wchar_t hostBuffer[1024] = {};
	wchar_t pathBuffer[2048] = {};
	wchar_t extraBuffer[2048] = {};
	components.lpszHostName = hostBuffer;
	components.dwHostNameLength = countof( hostBuffer );
	components.lpszUrlPath = pathBuffer;
	components.dwUrlPathLength = countof( pathBuffer );
	components.lpszExtraInfo = extraBuffer;
	components.dwExtraInfoLength = countof( extraBuffer );
	if ( !WinHttpCrackUrl( wideURL.c_str(), 0, 0, &components ) || components.nScheme != INTERNET_SCHEME_HTTPS )
	{
		rangeFailed.store( true );
		return false;
	}
	const std::wstring host( components.lpszHostName, components.dwHostNameLength );
	std::wstring path( components.lpszUrlPath, components.dwUrlPathLength );
	path.append( components.lpszExtraInfo, components.dwExtraInfoLength );
	HINTERNET session = WinHttpOpen( L"Zandronum Mod Manager", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0 );
	HINTERNET connection = session != NULL ? WinHttpConnect( session, host.c_str(), components.nPort, 0 ) : NULL;
	HINTERNET request = connection != NULL ? WinHttpOpenRequest( connection, L"GET", path.empty() ? L"/" : path.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE ) : NULL;
	bool success = false;
	FILE *output = NULL;
	if ( request != NULL )
	{
#ifdef WINHTTP_OPTION_REDIRECT_POLICY
		DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
		WinHttpSetOption( request, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy) );
#endif
		wchar_t rangeHeader[128];
		_snwprintf_s( rangeHeader, countof(rangeHeader), _TRUNCATE, L"Range: bytes=%llu-%llu\r\n", static_cast<unsigned long long>(start), static_cast<unsigned long long>(end) );
		if ( WinHttpAddRequestHeaders( request, rangeHeader, static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE ) &&
			WinHttpSendRequest( request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0 ) && WinHttpReceiveResponse( request, NULL ) )
		{
			DWORD status = 0;
			DWORD statusSize = sizeof(status);
			wchar_t contentRange[128] = {};
			DWORD contentRangeSize = sizeof(contentRange);
			const bool validContentRange = WinHttpQueryHeaders( request, WINHTTP_QUERY_CONTENT_RANGE, WINHTTP_HEADER_NAME_BY_INDEX, contentRange, &contentRangeSize, WINHTTP_NO_HEADER_INDEX ) &&
				modmanager_IsExpectedContentRange( contentRange, start, end, total );
			if ( WinHttpQueryHeaders( request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX ) && status == 206 && validContentRange )
			{
				output = fopen( tempPath.GetChars(), "r+b" );
				if ( output != NULL && _fseeki64( output, static_cast<__int64>(start), SEEK_SET ) == 0 )
				{
					uint64_t localReceived = 0;
					BYTE buffer[16384];
					DWORD bytesRead = 0;
					bool readOK = true;
					const uint64_t expectedRange = end - start + 1;
					while ( !g_ModManagerCancel.load() && !rangeFailed.load() && WinHttpReadData( request, buffer, sizeof(buffer), &bytesRead ) && bytesRead > 0 )
					{
						if ( bytesRead > expectedRange - localReceived || fwrite( buffer, 1, bytesRead, output ) != bytesRead )
						{
							readOK = false;
							break;
						}
						localReceived += bytesRead;
						received.fetch_add( bytesRead );
						MODMANAGER_ReportPlatformProgress( received.load(), total );
					}
					if ( g_ModManagerCancel.load() || rangeFailed.load() )
						readOK = false;
					success = readOK && localReceived == expectedRange;
				}
			}
		}
	}
	if ( output != NULL ) fclose( output );
	if ( request != NULL ) WinHttpCloseHandle( request );
	if ( connection != NULL ) WinHttpCloseHandle( connection );
	if ( session != NULL ) WinHttpCloseHandle( session );
	if ( !success ) rangeFailed.store( true );
	return success;
}

static bool modmanager_DownloadWindowsRanged(const FString &url, const FString &tempName, uint64_t total, FString &error)
{
	const FString directory = MODMANAGER_GetDirectory( true );
	if ( directory.IsEmpty() )
	{
		error = "The managed mods directory is unavailable.";
		return false;
	}
	const FString tempPath = directory + "/" + tempName;
	FILE *output = fopen( tempPath.GetChars(), "wb" );
	if ( output == NULL || _chsize_s( _fileno( output ), static_cast<__int64>(total) ) != 0 )
	{
		if ( output != NULL ) fclose( output );
		error = "Unable to prepare the ranged download.";
		return false;
	}
	fclose( output );

	std::atomic<uint64_t> received( 0 );
	std::atomic<bool> rangeFailed( false );
	const uint64_t streamCount = total < 4 ? total : 4;
	std::vector<std::thread> workers;
	workers.reserve( static_cast<size_t>(streamCount) );
	for ( uint64_t i = 0; i < streamCount; ++i )
	{
		const uint64_t start = ( total * i ) / streamCount;
		const uint64_t end = ( total * ( i + 1 ) ) / streamCount - 1;
		workers.push_back( std::thread( [&, start, end]()
		{
			modmanager_DownloadWindowsRange( url, tempPath, start, end, total, received, rangeFailed );
		} ) );
	}
	for ( std::thread &worker : workers )
		worker.join();
	if ( g_ModManagerCancel.load() )
		error = "Download cancelled.";
	else if ( rangeFailed.load() || received.load() != total )
		error = "The ranged download was incomplete.";
	else
		return true;
	modmanager_RemoveOwnedTemp( tempName );
	return false;
}

static bool modmanager_DownloadWindows(const FString &url, const FString &tempName, uint64_t maxBytes, FString &error)
{
	uint64_t total = 0;
	if ( modmanager_QueryWindowsRangeInfo( url, total ) && total <= maxBytes )
	{
		if ( modmanager_DownloadWindowsRanged( url, tempName, total, error ) )
			return true;
		if ( g_ModManagerCancel.load() )
			return false;
	}
	return modmanager_DownloadWindowsSingle( url, tempName, maxBytes, error );
}
#endif

static bool modmanager_DownloadToTemp(const FString &url, const FString &tempName, uint64_t maxBytes, FString &error)
{
#ifdef _WIN32
	return modmanager_DownloadWindows( url, tempName, maxBytes, error );
#elif defined(__ANDROID__)
	char errorBuffer[256] = {};
	const bool result = Zandronum_AndroidHost_DownloadMod( url.GetChars(), tempName.GetChars(), maxBytes, &g_ModManagerCancel, errorBuffer, sizeof(errorBuffer) );
	if ( !result )
		error = errorBuffer[0] != 0 ? errorBuffer : "Android HTTPS request failed.";
	return result;
#else
	error = "No native HTTPS transport is available on this platform.";
	return false;
#endif
}

static size_t modmanager_FindInsensitive(const std::string &text, const char *needle, size_t start)
{
	const size_t needleLength = strlen( needle );
	if ( needleLength == 0 || needleLength > text.size() )
		return std::string::npos;
	for ( size_t i = start; i + needleLength <= text.size(); ++i )
	{
		bool equal = true;
		for ( size_t j = 0; j < needleLength; ++j )
		{
			if ( std::tolower( static_cast<unsigned char>(text[i + j]) ) != std::tolower( static_cast<unsigned char>(needle[j]) ) )
			{
				equal = false;
				break;
			}
		}
		if ( equal )
			return i;
	}
	return std::string::npos;
}

static bool modmanager_ReadBoundedTextFile(const FString &path, uint64_t maxBytes, std::string &text, FString &error)
{
	FILE *input = fopen( path.GetChars(), "rb" );
	if ( input == NULL )
	{
		error = "Unable to read the search response.";
		return false;
	}
	text.clear();
	char buffer[16384];
	while ( !feof( input ) )
	{
		const size_t count = fread( buffer, 1, sizeof( buffer ), input );
		if ( count > 0 )
		{
			if ( text.size() > maxBytes || count > maxBytes - text.size() )
			{
				fclose( input );
				error = "The search response is larger than the allowed limit.";
				return false;
			}
			text.append( buffer, count );
		}
		if ( ferror( input ) )
		{
			fclose( input );
			error = "Unable to read the search response.";
			return false;
		}
	}
	fclose( input );
	if ( text.empty() )
	{
		error = "The search response was empty.";
		return false;
	}
	return true;
}

static int modmanager_HexDigit(char value)
{
	if ( value >= '0' && value <= '9' ) return value - '0';
	if ( value >= 'a' && value <= 'f' ) return value - 'a' + 10;
	if ( value >= 'A' && value <= 'F' ) return value - 'A' + 10;
	return -1;
}

static bool modmanager_DecodeURLBasename(const FString &encoded, FString &decoded, bool archive = false)
{
	std::string value;
	value.reserve( static_cast<size_t>(encoded.Len()) );
	for ( int i = 0; i < encoded.Len(); ++i )
	{
		if ( encoded[i] == '%' )
		{
			if ( i + 2 >= encoded.Len() )
				return false;
			const int high = modmanager_HexDigit( encoded[i + 1] );
			const int low = modmanager_HexDigit( encoded[i + 2] );
			if ( high < 0 || low < 0 )
				return false;
			value.push_back( static_cast<char>(( high << 4 ) | low) );
			i += 2;
		}
		else
			value.push_back( encoded[i] );
	}
	if ( value.empty() || memchr( value.data(), 0, value.size() ) != NULL ||
		( archive ? !modmanager_IsSafeArchiveBasename( value.c_str() ) : !MODMANAGER_IsSafeBasename( value.c_str() ) ) )
		return false;
	decoded = value.c_str();
	return true;
}

static void modmanager_JSONSkipWhitespace(const std::string &json, size_t &position)
{
	while ( position < json.size() && std::isspace( static_cast<unsigned char>(json[position]) ) )
		++position;
}

static bool modmanager_JSONParseString(const std::string &json, size_t &position, std::string &value)
{
	if ( position >= json.size() || json[position] != '"' )
		return false;
	++position;
	value.clear();
	while ( position < json.size() )
	{
		const unsigned char character = static_cast<unsigned char>(json[position++]);
		if ( character == '"' )
			return true;
		if ( character < 32 )
			return false;
		if ( character != '\\' )
		{
			value.push_back( static_cast<char>(character) );
			continue;
		}
		if ( position >= json.size() )
			return false;
		const char escaped = json[position++];
		switch ( escaped )
		{
		case '"': value.push_back( '"' ); break;
		case '\\': value.push_back( '\\' ); break;
		case '/': value.push_back( '/' ); break;
		case 'b': value.push_back( '\b' ); break;
		case 'f': value.push_back( '\f' ); break;
		case 'n': value.push_back( '\n' ); break;
		case 'r': value.push_back( '\r' ); break;
		case 't': value.push_back( '\t' ); break;
		case 'u':
		{
			if ( position + 4 > json.size() )
				return false;
			unsigned int codepoint = 0;
			for ( unsigned int i = 0; i < 4; ++i )
			{
				const int digit = modmanager_HexDigit( json[position++] );
				if ( digit < 0 )
					return false;
				codepoint = ( codepoint << 4 ) | static_cast<unsigned int>(digit);
			}
			if ( codepoint >= 0xd800 && codepoint <= 0xdfff )
				return false;
			if ( codepoint <= 0x7f )
				value.push_back( static_cast<char>(codepoint) );
			else if ( codepoint <= 0x7ff )
			{
				value.push_back( static_cast<char>( 0xc0 | ( codepoint >> 6 ) ) );
				value.push_back( static_cast<char>( 0x80 | ( codepoint & 0x3f ) ) );
			}
			else
			{
				value.push_back( static_cast<char>( 0xe0 | ( codepoint >> 12 ) ) );
				value.push_back( static_cast<char>( 0x80 | ( ( codepoint >> 6 ) & 0x3f ) ) );
				value.push_back( static_cast<char>( 0x80 | ( codepoint & 0x3f ) ) );
			}
			break;
		}
		default:
			return false;
		}
	}
	return false;
}

static bool modmanager_JSONSkipValue(const std::string &json, size_t &position, unsigned int depth);

static bool modmanager_JSONSkipNumber(const std::string &json, size_t &position)
{
	const size_t start = position;
	if ( position < json.size() && json[position] == '-' )
		++position;
	if ( position >= json.size() )
		return false;
	if ( json[position] == '0' )
		++position;
	else
	{
		if ( json[position] < '1' || json[position] > '9' )
			return false;
		while ( position < json.size() && json[position] >= '0' && json[position] <= '9' )
			++position;
	}
	if ( position < json.size() && json[position] == '.' )
	{
		++position;
		const size_t fractionStart = position;
		while ( position < json.size() && json[position] >= '0' && json[position] <= '9' )
			++position;
		if ( position == fractionStart )
			return false;
	}
	if ( position < json.size() && ( json[position] == 'e' || json[position] == 'E' ) )
	{
		++position;
		if ( position < json.size() && ( json[position] == '+' || json[position] == '-' ) )
			++position;
		const size_t exponentStart = position;
		while ( position < json.size() && json[position] >= '0' && json[position] <= '9' )
			++position;
		if ( position == exponentStart )
			return false;
	}
	return position > start;
}

static bool modmanager_JSONSkipValue(const std::string &json, size_t &position, unsigned int depth)
{
	if ( depth > 32 )
		return false;
	modmanager_JSONSkipWhitespace( json, position );
	if ( position >= json.size() )
		return false;
	if ( json[position] == '"' )
	{
		std::string ignored;
		return modmanager_JSONParseString( json, position, ignored );
	}
	if ( json[position] == '{' )
	{
		++position;
		modmanager_JSONSkipWhitespace( json, position );
		if ( position < json.size() && json[position] == '}' )
		{
			++position;
			return true;
		}
		while ( position < json.size() )
		{
			std::string ignored;
			if ( !modmanager_JSONParseString( json, position, ignored ) )
				return false;
			modmanager_JSONSkipWhitespace( json, position );
			if ( position >= json.size() || json[position++] != ':' || !modmanager_JSONSkipValue( json, position, depth + 1 ) )
				return false;
			modmanager_JSONSkipWhitespace( json, position );
			if ( position >= json.size() )
				return false;
			if ( json[position] == '}' )
			{
				++position;
				return true;
			}
			if ( json[position++] != ',' )
				return false;
			modmanager_JSONSkipWhitespace( json, position );
		}
		return false;
	}
	if ( json[position] == '[' )
	{
		++position;
		modmanager_JSONSkipWhitespace( json, position );
		if ( position < json.size() && json[position] == ']' )
		{
			++position;
			return true;
		}
		while ( position < json.size() )
		{
			if ( !modmanager_JSONSkipValue( json, position, depth + 1 ) )
				return false;
			modmanager_JSONSkipWhitespace( json, position );
			if ( position >= json.size() )
				return false;
			if ( json[position] == ']' )
			{
				++position;
				return true;
			}
			if ( json[position++] != ',' )
				return false;
			modmanager_JSONSkipWhitespace( json, position );
		}
		return false;
	}
	if ( json.compare( position, 4, "true" ) == 0 )
	{
		position += 4;
		return true;
	}
	if ( json.compare( position, 5, "false" ) == 0 )
	{
		position += 5;
		return true;
	}
	if ( json.compare( position, 4, "null" ) == 0 )
	{
		position += 4;
		return true;
	}
	return modmanager_JSONSkipNumber( json, position );
}

static bool modmanager_JSONParseFileObject(const std::string &json, size_t &position, const char *expectedArchive, FString &archiveURL, bool &matches)
{
	modmanager_JSONSkipWhitespace( json, position );
	if ( position >= json.size() || json[position++] != '{' )
		return false;
	std::string filename;
	std::string url;
	bool hasFilename = false;
	bool hasURL = false;
	modmanager_JSONSkipWhitespace( json, position );
	if ( position < json.size() && json[position] == '}' )
	{
		++position;
		matches = false;
		return true;
	}
	while ( position < json.size() )
	{
		std::string key;
		if ( !modmanager_JSONParseString( json, position, key ) )
			return false;
		modmanager_JSONSkipWhitespace( json, position );
		if ( position >= json.size() || json[position++] != ':' )
			return false;
		modmanager_JSONSkipWhitespace( json, position );
		if ( key == "filename" || key == "url" )
		{
			std::string value;
			if ( !modmanager_JSONParseString( json, position, value ) )
				return false;
			if ( key == "filename" )
			{
				filename = value;
				hasFilename = true;
			}
			else
			{
				url = value;
				hasURL = true;
			}
		}
		else if ( !modmanager_JSONSkipValue( json, position, 0 ) )
			return false;
		modmanager_JSONSkipWhitespace( json, position );
		if ( position >= json.size() )
			return false;
		if ( json[position] == '}' )
		{
			++position;
			break;
		}
		if ( json[position++] != ',' )
			return false;
		modmanager_JSONSkipWhitespace( json, position );
	}
	matches = hasFilename && hasURL && stricmp( filename.c_str(), expectedArchive ) == 0;
	if ( matches )
		archiveURL = url.c_str();
	return true;
}

static bool modmanager_JSONParseFileCollection(const std::string &json, size_t &position, const char *expectedArchive, FString &archiveURL, FString &error)
{
	modmanager_JSONSkipWhitespace( json, position );
	unsigned int matches = 0;
	if ( position < json.size() && json[position] == '{' )
	{
		bool match = false;
		if ( !modmanager_JSONParseFileObject( json, position, expectedArchive, archiveURL, match ) )
			return false;
		matches = match ? 1 : 0;
	}
	else if ( position < json.size() && json[position] == '[' )
	{
		++position;
		modmanager_JSONSkipWhitespace( json, position );
		if ( position < json.size() && json[position] == ']' )
		{
			++position;
			error = "The /idgames response contained no archive entries.";
			return false;
		}
		while ( position < json.size() )
		{
			bool match = false;
			if ( !modmanager_JSONParseFileObject( json, position, expectedArchive, archiveURL, match ) )
				return false;
			if ( match )
				++matches;
			modmanager_JSONSkipWhitespace( json, position );
			if ( position >= json.size() )
				return false;
			if ( json[position] == ']' )
			{
				++position;
				break;
			}
			if ( json[position++] != ',' )
				return false;
			modmanager_JSONSkipWhitespace( json, position );
		}
	}
	else
		return false;
	if ( matches == 0 )
	{
		error = "The /idgames response did not contain the requested archive.";
		return false;
	}
	if ( matches > 1 )
	{
		error = "The /idgames response contained duplicate archive matches.";
		return false;
	}
	return true;
}

static bool modmanager_JSONParseContent(const std::string &json, size_t &position, const char *expectedArchive, FString &archiveURL, FString &error)
{
	modmanager_JSONSkipWhitespace( json, position );
	if ( position >= json.size() || json[position++] != '{' )
		return false;
	bool foundFile = false;
	modmanager_JSONSkipWhitespace( json, position );
	if ( position < json.size() && json[position] == '}' )
	{
		++position;
		error = "The /idgames response contained no content.";
		return false;
	}
	while ( position < json.size() )
	{
		std::string key;
		if ( !modmanager_JSONParseString( json, position, key ) )
			return false;
		modmanager_JSONSkipWhitespace( json, position );
		if ( position >= json.size() || json[position++] != ':' )
			return false;
		modmanager_JSONSkipWhitespace( json, position );
		if ( key == "file" )
		{
			if ( foundFile || !modmanager_JSONParseFileCollection( json, position, expectedArchive, archiveURL, error ) )
				return false;
			foundFile = true;
		}
		else if ( !modmanager_JSONSkipValue( json, position, 0 ) )
			return false;
		modmanager_JSONSkipWhitespace( json, position );
		if ( position >= json.size() )
			return false;
		if ( json[position] == '}' )
		{
			++position;
			break;
		}
		if ( json[position++] != ',' )
			return false;
		modmanager_JSONSkipWhitespace( json, position );
	}
	return foundFile;
}

static bool modmanager_ParseIdgamesJSON(const std::string &json, const char *expectedArchive, FString &archiveURL, FString &error)
{
	size_t position = 0;
	modmanager_JSONSkipWhitespace( json, position );
	if ( position >= json.size() || json[position++] != '{' )
	{
		error = "The /idgames response was not valid JSON.";
		return false;
	}
	bool foundContent = false;
	while ( position < json.size() )
	{
		std::string key;
		if ( !modmanager_JSONParseString( json, position, key ) )
		{
			error = "The /idgames response was not valid JSON.";
			return false;
		}
		modmanager_JSONSkipWhitespace( json, position );
		if ( position >= json.size() || json[position++] != ':' )
		{
			error = "The /idgames response was not valid JSON.";
			return false;
		}
		modmanager_JSONSkipWhitespace( json, position );
		if ( key == "content" )
		{
			if ( foundContent || !modmanager_JSONParseContent( json, position, expectedArchive, archiveURL, error ) )
			{
				if ( error.IsEmpty() ) error = "The /idgames response was not valid JSON.";
				return false;
			}
			foundContent = true;
		}
		else if ( !modmanager_JSONSkipValue( json, position, 0 ) )
		{
			error = "The /idgames response was not valid JSON.";
			return false;
		}
		modmanager_JSONSkipWhitespace( json, position );
		if ( position >= json.size() )
		{
			error = "The /idgames response was not valid JSON.";
			return false;
		}
		if ( json[position] == '}' )
		{
			++position;
			break;
		}
		if ( json[position++] != ',' )
		{
			error = "The /idgames response was not valid JSON.";
			return false;
		}
		modmanager_JSONSkipWhitespace( json, position );
	}
	modmanager_JSONSkipWhitespace( json, position );
	if ( !foundContent || position != json.size() )
	{
		if ( error.IsEmpty() ) error = "The /idgames response did not contain a valid result.";
		return false;
	}
	return true;
}

static bool modmanager_IsValidHTTPSArchiveURL(const FString &value, const char *expectedArchive, const FString &sourceURL)
{
	if ( value.Len() < 9 || value.Len() > MODMANAGER_MAX_SOURCE_VALUE || value.Left( 8 ).CompareNoCase( "https://" ) != 0 || value.IndexOf( "#" ) >= 0 || value.IndexOf( "\\" ) >= 0 )
		return false;
	for ( int i = 0; i < value.Len(); ++i )
	{
		const unsigned char character = static_cast<unsigned char>(value[i]);
		if ( character <= 32 || character == 127 )
			return false;
	}
	const int pathStart = value.IndexOf( "/", 8 );
	const int sourcePathStart = sourceURL.IndexOf( "/", 8 );
	if ( pathStart < 0 || sourcePathStart < 0 || !modmanager_IsValidAFSOverride( value.Left( pathStart ) + "/" ) || value.Left( pathStart ).CompareNoCase( sourceURL.Left( sourcePathStart ) ) != 0 || value.IndexOf( "@", 8 ) >= 0 )
		return false;
	int pathEnd = value.IndexOf( "?" );
	if ( pathEnd < 0 ) pathEnd = static_cast<int>( value.Len() );
	const FString path = value.Left( pathEnd );
	for ( int i = pathStart; i + 2 < path.Len(); ++i )
	{
		if ( path[i] == '/' && path[i + 1] == '.' && ( path[i + 2] == '.' || path[i + 2] == '/' ) )
			return false;
		if ( path[i] == '%' )
		{
			if ( i + 2 >= path.Len() ) return false;
			const int high = modmanager_HexDigit( path[i + 1] );
			const int low = modmanager_HexDigit( path[i + 2] );
			const int decoded = high >= 0 && low >= 0 ? ( high << 4 ) | low : -1;
			if ( decoded < 0 || decoded == '.' || decoded == '/' || decoded == '\\' )
				return false;
		}
	}
	const int lastSlash = path.LastIndexOf( "/" );
	if ( lastSlash < pathStart || lastSlash + 1 >= path.Len() )
		return false;
	FString decodedName;
	return modmanager_DecodeURLBasename( path.Mid( lastSlash + 1 ), decodedName, true ) && decodedName.CompareNoCase( expectedArchive ) == 0;
}

static bool modmanager_BuildSearchResultURL(const FString &searchURL, const FString &href, const char *name, FString &result)
{
	if ( href.IsEmpty() || href.Len() > MODMANAGER_MAX_SOURCE_VALUE )
		return false;
	for ( int i = 0; i < href.Len(); ++i )
	{
		const unsigned char character = static_cast<unsigned char>(href[i]);
		if ( character <= 32 || character == 127 || character == '\\' || character == '#' )
			return false;
	}
	const int searchPathStart = searchURL.IndexOf( "/", 8 );
	if ( searchPathStart < 0 )
		return false;
	const FString origin = searchURL.Left( searchPathStart );
	FString originCheck = origin + "/";
	if ( !modmanager_IsValidAFSOverride( originCheck ) )
		return false;
	FString searchPath = searchURL;
	const int searchQuery = searchPath.IndexOf( "?" );
	if ( searchQuery >= 0 )
		searchPath = searchPath.Left( searchQuery );
	int lastSlash = -1;
	for ( int i = searchPathStart; i < searchPath.Len(); ++i )
	{
		if ( searchPath[i] == '/' )
			lastSlash = i;
	}
	FString baseDirectory = lastSlash >= searchPathStart ? searchPath.Left( lastSlash + 1 ) : origin + "/";

	FString candidate;
	if ( href.Left( 8 ).CompareNoCase( "https://" ) == 0 )
		candidate = href;
	else if ( href.Left( 2 ).CompareNoCase( "//" ) == 0 || href.Left( 7 ).CompareNoCase( "http://" ) == 0 )
		return false;
	else if ( href[0] == '/' )
		candidate = origin + href;
	else
		candidate = baseDirectory + href;

	const int candidatePathStart = candidate.IndexOf( "/", 8 );
	if ( candidatePathStart < 0 || candidate.Left( candidatePathStart ).CompareNoCase( origin ) != 0 )
		return false;
	int candidatePathEnd = candidate.IndexOf( "?" );
	if ( candidatePathEnd < 0 )
		candidatePathEnd = static_cast<int>(candidate.Len());
	FString path = candidate.Left( candidatePathEnd );
	for ( int i = candidatePathStart; i + 1 < path.Len(); ++i )
	{
		if ( path[i] == '/' && path[i + 1] == '.' && ( i + 2 == path.Len() || path[i + 2] == '.' || path[i + 2] == '/' ) )
			return false;
	}
	lastSlash = -1;
	for ( int i = candidatePathStart; i < path.Len(); ++i )
	{
		if ( path[i] == '/' )
			lastSlash = i;
	}
	if ( lastSlash < 0 || lastSlash + 1 >= path.Len() )
		return false;
	for ( int i = candidatePathStart; i + 2 < lastSlash; ++i )
	{
		if ( path[i] != '%' )
			continue;
		const char first = path[i + 1];
		const char second = path[i + 2];
		if ( ( first == '2' && ( second == 'e' || second == 'E' || second == 'f' || second == 'F' ) ) ||
			( first == '5' && ( second == 'c' || second == 'C' ) ) )
			return false;
	}
	FString decodedName;
	if ( !modmanager_DecodeURLBasename( path.Mid( lastSlash + 1 ), decodedName ) || decodedName.CompareNoCase( name ) != 0 )
		return false;
	result = candidate;
	return true;
}

static bool modmanager_FindSearchResultURLInHTML(const std::string &html, const FString &searchURL, const char *name, FString &result, FString &error)
{
	const unsigned int maxLinks = 256;
	size_t cursor = 0;
	unsigned int linkCount = 0;
	while ( linkCount < maxLinks )
	{
		const size_t hrefPosition = modmanager_FindInsensitive( html, "href", cursor );
		if ( hrefPosition == std::string::npos )
			break;
		size_t valuePosition = hrefPosition + 4;
		while ( valuePosition < html.size() && std::isspace( static_cast<unsigned char>(html[valuePosition]) ) )
			++valuePosition;
		if ( valuePosition >= html.size() || html[valuePosition] != '=' )
		{
			cursor = hrefPosition + 4;
			continue;
		}
		++valuePosition;
		while ( valuePosition < html.size() && std::isspace( static_cast<unsigned char>(html[valuePosition]) ) )
			++valuePosition;
		if ( valuePosition >= html.size() )
			break;
		const char quote = html[valuePosition] == '\'' || html[valuePosition] == '"' ? html[valuePosition++] : 0;
		const size_t valueStart = valuePosition;
		while ( valuePosition < html.size() && ( quote != 0 ? html[valuePosition] != quote : !std::isspace( static_cast<unsigned char>(html[valuePosition]) ) && html[valuePosition] != '>' ) )
			++valuePosition;
		if ( valuePosition > valueStart && valuePosition - valueStart <= MODMANAGER_MAX_SOURCE_VALUE )
		{
			const FString href = html.substr( valueStart, valuePosition - valueStart ).c_str();
			++linkCount;
			if ( modmanager_BuildSearchResultURL( searchURL, href, name, result ) )
				return true;
		}
		cursor = valuePosition + 1;
	}
	if ( linkCount >= maxLinks )
		error = "The search response contains too many links.";
	else
		error = "The search response did not contain the requested file.";
	return false;
}

static bool modmanager_FindSearchResultURL(const FString &htmlPath, const FString &searchURL, const char *name, FString &result, FString &error)
{
	std::string html;
	if ( !modmanager_ReadBoundedTextFile( htmlPath, MODMANAGER_MAX_SEARCH_RESPONSE_SIZE, html, error ) )
		return false;
	return modmanager_FindSearchResultURLInHTML( html, searchURL, name, result, error );
}

struct FModZipEntry
{
	std::string Name;
	uint16_t Flags;
	uint16_t Method;
	uint32_t CRC32;
	uint64_t CompressedSize;
	uint64_t UncompressedSize;
	uint64_t DataOffset;
};

static uint16_t modmanager_ReadLE16(const unsigned char *data)
{
	return static_cast<uint16_t>( data[0] | ( static_cast<uint16_t>(data[1]) << 8 ) );
}

static uint32_t modmanager_ReadLE32(const unsigned char *data)
{
	return static_cast<uint32_t>( data[0] | ( static_cast<uint32_t>(data[1]) << 8 ) |
		( static_cast<uint32_t>(data[2]) << 16 ) | ( static_cast<uint32_t>(data[3]) << 24 ) );
}

static bool modmanager_ReadFileAt(FILE *file, uint64_t offset, void *buffer, size_t size)
{
	if ( offset > static_cast<uint64_t>(LONG_MAX) || fseek( file, static_cast<long>(offset), SEEK_SET ) != 0 )
		return false;
	return size == 0 || fread( buffer, 1, size, file ) == size;
}

static bool modmanager_GetFileLength(FILE *file, uint64_t &length)
{
	if ( fseek( file, 0, SEEK_END ) != 0 )
		return false;
	const long end = ftell( file );
	if ( end < 0 )
		return false;
	length = static_cast<uint64_t>(end);
	return true;
}

static bool modmanager_IsSafeZipEntryName(const std::string &name)
{
	if ( name.empty() || name.size() > 1024 || name[0] == '/' || name[0] == '\\' )
		return false;
	for ( size_t i = 0; i < name.size(); ++i )
	{
		const unsigned char character = static_cast<unsigned char>(name[i]);
		if ( character == 0 || character < 32 || character == 127 || character == '\\' || character == ':' )
			return false;
	}
	size_t componentStart = 0;
	for ( size_t i = 0; i <= name.size(); ++i )
	{
		if ( i != name.size() && name[i] != '/' )
			continue;
		const size_t componentLength = i - componentStart;
		if ( componentLength == 0 && i != name.size() )
			return false;
		if ( componentLength == 1 && name[componentStart] == '.' )
			return false;
		if ( componentLength == 2 && name[componentStart] == '.' && name[componentStart + 1] == '.' )
			return false;
		componentStart = i + 1;
	}
	return true;
}

static bool modmanager_ZipNamesEqual(const std::string &left, const char *right)
{
	if ( right == NULL )
		return false;
	const size_t slash = left.find_last_of( '/' );
	const char *basename = slash == std::string::npos ? left.c_str() : left.c_str() + slash + 1;
	return stricmp( basename, right ) == 0;
}

static bool modmanager_FindZipEntry(FILE *file, const char *expectedName, FModZipEntry &entry, FString &error)
{
	uint64_t fileSize = 0;
	if ( !modmanager_GetFileLength( file, fileSize ) || fileSize > MODMANAGER_MAX_FILE_SIZE || fileSize < 22 )
	{
		error = "The archive has an invalid size.";
		return false;
	}
	const uint64_t tailOffset = fileSize > 0xffffULL + 22ULL ? fileSize - ( 0xffffULL + 22ULL ) : 0;
	const size_t tailSize = static_cast<size_t>( fileSize - tailOffset );
	std::vector<unsigned char> tail( tailSize );
	if ( !modmanager_ReadFileAt( file, tailOffset, tail.data(), tail.size() ) )
	{
		error = "Unable to read the archive directory.";
		return false;
	}
	uint64_t endOffset = 0;
	bool foundEnd = false;
	for ( size_t i = tail.size() - 22; ; --i )
	{
		if ( modmanager_ReadLE32( &tail[i] ) == 0x06054b50U )
		{
			const uint16_t commentLength = modmanager_ReadLE16( &tail[i + 20] );
			if ( tailOffset + i + 22ULL + commentLength == fileSize )
			{
				endOffset = tailOffset + i;
				foundEnd = true;
				break;
			}
		}
		if ( i == 0 )
			break;
	}
	if ( !foundEnd )
	{
		error = "The archive has no valid end directory.";
		return false;
	}
	unsigned char endRecord[22];
	if ( !modmanager_ReadFileAt( file, endOffset, endRecord, sizeof(endRecord) ) )
	{
		error = "Unable to read the archive end directory.";
		return false;
	}
	const uint16_t disk = modmanager_ReadLE16( &endRecord[4] );
	const uint16_t firstDisk = modmanager_ReadLE16( &endRecord[6] );
	const uint16_t entries = modmanager_ReadLE16( &endRecord[8] );
	const uint16_t entriesOnDisk = modmanager_ReadLE16( &endRecord[10] );
	const uint32_t directorySize = modmanager_ReadLE32( &endRecord[12] );
	const uint32_t directoryOffset = modmanager_ReadLE32( &endRecord[16] );
	if ( disk != 0 || firstDisk != 0 || entries != entriesOnDisk || entries == 0xffff || directorySize == 0xffffffffU || directoryOffset == 0xffffffffU ||
		entries > MODMANAGER_MAX_ARCHIVE_ENTRIES || directorySize > MODMANAGER_MAX_ARCHIVE_DIRECTORY_SIZE ||
		static_cast<uint64_t>(directoryOffset) + directorySize > endOffset )
	{
		error = "The archive directory is unsupported or unsafe.";
		return false;
	}
	std::vector<unsigned char> directory( directorySize );
	if ( !modmanager_ReadFileAt( file, directoryOffset, directory.data(), directory.size() ) )
	{
		error = "Unable to read the archive directory.";
		return false;
	}
	std::vector<std::string> names;
	names.reserve( entries );
	bool foundEntry = false;
	uint64_t totalUncompressed = 0;
	size_t position = 0;
	for ( unsigned int index = 0; index < entries; ++index )
	{
		if ( position > directory.size() || directory.size() - position < 46 || modmanager_ReadLE32( &directory[position] ) != 0x02014b50U )
		{
			error = "The archive contains a malformed directory entry.";
			return false;
		}
		const unsigned char *record = &directory[position];
		const uint16_t flags = modmanager_ReadLE16( &record[8] );
		const uint16_t method = modmanager_ReadLE16( &record[10] );
		const uint32_t crc = modmanager_ReadLE32( &record[16] );
		const uint32_t compressedSize = modmanager_ReadLE32( &record[20] );
		const uint32_t uncompressedSize = modmanager_ReadLE32( &record[24] );
		const uint16_t nameLength = modmanager_ReadLE16( &record[28] );
		const uint16_t extraLength = modmanager_ReadLE16( &record[30] );
		const uint16_t commentLength = modmanager_ReadLE16( &record[32] );
		const uint32_t externalAttributes = modmanager_ReadLE32( &record[38] );
		const uint32_t localOffset = modmanager_ReadLE32( &record[42] );
		const size_t recordSize = 46ULL + nameLength + extraLength + commentLength;
		if ( recordSize > directory.size() - position )
		{
			error = "The archive contains a truncated directory entry.";
			return false;
		}
		const std::string name( reinterpret_cast<const char *>(record + 46), nameLength );
		if ( !modmanager_IsSafeZipEntryName( name ) )
		{
			error = "The archive contains an unsafe path.";
			return false;
		}
		for ( const std::string &seen : names )
		{
			if ( stricmp( seen.c_str(), name.c_str() ) == 0 )
			{
				error = "The archive contains duplicate entries.";
				return false;
			}
		}
		names.push_back( name );
		if ( ( flags & 1 ) != 0 || ( ( externalAttributes >> 16 ) & 0xf000U ) == 0xa000U )
		{
			error = "The archive contains encrypted or linked content.";
			return false;
		}
		if ( method != 0 && method != 8 )
		{
			error = "The archive uses an unsupported compression method.";
			return false;
		}
		if ( uncompressedSize > MODMANAGER_MAX_FILE_SIZE || compressedSize > MODMANAGER_MAX_FILE_SIZE ||
			( uncompressedSize > 0 && compressedSize == 0 ) ||
			( compressedSize > 0 && static_cast<uint64_t>(uncompressedSize) > static_cast<uint64_t>(compressedSize) * MODMANAGER_MAX_ARCHIVE_EXPANSION_RATIO ) ||
			totalUncompressed > MODMANAGER_MAX_FILE_SIZE - uncompressedSize )
		{
			error = "The archive exceeds the extraction limits.";
			return false;
		}
		totalUncompressed += uncompressedSize;
		unsigned char localHeader[30];
		if ( !modmanager_ReadFileAt( file, localOffset, localHeader, sizeof(localHeader) ) || modmanager_ReadLE32( localHeader ) != 0x04034b50U )
		{
			error = "The archive has an invalid local header.";
			return false;
		}
		const uint16_t localFlags = modmanager_ReadLE16( &localHeader[6] );
		const uint16_t localMethod = modmanager_ReadLE16( &localHeader[8] );
		const uint16_t localNameLength = modmanager_ReadLE16( &localHeader[26] );
		const uint16_t localExtraLength = modmanager_ReadLE16( &localHeader[28] );
		const uint64_t dataOffset = static_cast<uint64_t>(localOffset) + 30ULL + localNameLength + localExtraLength;
		if ( localFlags != flags || localMethod != method || dataOffset > fileSize || static_cast<uint64_t>(compressedSize) > fileSize - dataOffset )
		{
			error = "The archive local header does not match its directory.";
			return false;
		}
		std::vector<unsigned char> localName( localNameLength );
		if ( !modmanager_ReadFileAt( file, static_cast<uint64_t>(localOffset) + 30ULL, localName.data(), localName.size() ) ||
			std::string( reinterpret_cast<const char *>(localName.data()), localName.size() ) != name )
		{
			error = "The archive local filename does not match its directory.";
			return false;
		}
		if ( modmanager_ZipNamesEqual( name, expectedName ) )
		{
			if ( foundEntry )
			{
				error = "The archive contains duplicate requested basenames.";
				return false;
			}
			entry.Name = name;
			entry.Flags = flags;
			entry.Method = method;
			entry.CRC32 = crc;
			entry.CompressedSize = compressedSize;
			entry.UncompressedSize = uncompressedSize;
			entry.DataOffset = dataOffset;
			foundEntry = true;
		}
		position += recordSize;
	}
	if ( !foundEntry )
	{
		error = "The archive did not contain the requested file.";
		return false;
	}
	return true;
}

static bool modmanager_ExtractZipEntry(const FString &archivePath, const char *expectedName, const FString &outputName, FString &error)
{
	if ( !MODMANAGER_IsSafeBasename( expectedName ) || !modmanager_IsOwnedTempName( outputName ) )
	{
		error = "The archive extraction target is invalid.";
		return false;
	}
	const FString directory = MODMANAGER_GetDirectory( false );
	if ( directory.IsEmpty() )
	{
		error = "The managed mods directory is unavailable.";
		return false;
	}
	const FString outputPath = directory + "/" + outputName;
	FILE *input = fopen( archivePath.GetChars(), "rb" );
	if ( input == NULL )
	{
		error = "Unable to open the downloaded archive.";
		return false;
	}
	FModZipEntry entry;
	if ( !modmanager_FindZipEntry( input, expectedName, entry, error ) )
	{
		fclose( input );
		return false;
	}
	FILE *output = fopen( outputPath.GetChars(), "wb" );
	if ( output == NULL )
	{
		fclose( input );
		error = "Unable to create the extracted file.";
		return false;
	}
	bool success = false;
	uint64_t written = 0;
	uLong crc = crc32( 0L, Z_NULL, 0 );
	if ( fseek( input, static_cast<long>(entry.DataOffset), SEEK_SET ) == 0 )
	{
		BYTE buffer[16384];
		if ( entry.Method == 0 )
		{
			uint64_t remaining = entry.UncompressedSize;
			while ( remaining > 0 && !g_ModManagerCancel.load() )
			{
				const size_t request = static_cast<size_t>( remaining > sizeof(buffer) ? sizeof(buffer) : remaining );
				const size_t count = fread( buffer, 1, request, input );
				if ( count != request || fwrite( buffer, 1, count, output ) != count )
					break;
				remaining -= count;
				written += count;
				crc = crc32( crc, buffer, static_cast<uInt>(count) );
			}
			success = remaining == 0 && written == entry.UncompressedSize && !g_ModManagerCancel.load();
		}
		else
		{
			z_stream stream;
			BYTE inputBuffer[16384];
			BYTE outputBuffer[16384];
			memset( &stream, 0, sizeof(stream) );
			if ( inflateInit2( &stream, -MAX_WBITS ) == Z_OK )
			{
				uint64_t inputRemaining = entry.CompressedSize;
				bool ended = false;
				bool valid = true;
				while ( !ended && valid && !g_ModManagerCancel.load() )
				{
					if ( stream.avail_in == 0 && inputRemaining > 0 )
					{
						const size_t request = static_cast<size_t>( inputRemaining > sizeof(inputBuffer) ? sizeof(inputBuffer) : inputRemaining );
						const size_t count = fread( inputBuffer, 1, request, input );
						if ( count == 0 )
						{
							valid = false;
							break;
						}
						inputRemaining -= count;
						stream.next_in = inputBuffer;
						stream.avail_in = static_cast<uInt>(count);
					}
					if ( stream.avail_in == 0 && inputRemaining == 0 )
					{
						valid = false;
						break;
					}
					stream.next_out = outputBuffer;
					stream.avail_out = sizeof(outputBuffer);
					const int result = inflate( &stream, Z_NO_FLUSH );
					const size_t count = sizeof(outputBuffer) - stream.avail_out;
					if ( count > 0 )
					{
						if ( written > entry.UncompressedSize || count > entry.UncompressedSize - written || fwrite( outputBuffer, 1, count, output ) != count )
							valid = false;
						else
						{
							written += count;
							crc = crc32( crc, outputBuffer, static_cast<uInt>(count) );
						}
					}
					if ( result == Z_STREAM_END )
					{
						ended = true;
						if ( inputRemaining != 0 || stream.avail_in != 0 )
							valid = false;
					}
					else if ( result != Z_OK )
						valid = false;
				}
				inflateEnd( &stream );
				success = valid && ended && written == entry.UncompressedSize && !g_ModManagerCancel.load();
			}
		}
	}
	fclose( output );
	fclose( input );
	if ( success && crc == entry.CRC32 )
		return true;
	modmanager_RemoveOwnedTemp( outputName );
	if ( error.IsEmpty() )
		error = g_ModManagerCancel.load() ? "Download cancelled." : "The archive entry failed verification.";
	return false;
}

static void modmanager_DownloadWorker()
{
	if ( MODMANAGER_GetDirectory( true ).IsEmpty() )
	{
		modmanager_SetQueueFailure( "The managed mods directory is unavailable." );
		return;
	}
	FModJoinRequest request;
	{
		std::lock_guard<std::mutex> lock( g_ModManagerMutex );
		request = g_ModManagerRequest;
	}

	for ( unsigned int i = 0; i < request.Requirements.Size(); ++i )
	{
		FModRequirement &requirement = request.Requirements[i];
		if ( requirement.Optional || requirement.Status == MODREQ_Found )
			continue;
		if ( !requirement.Downloadable || requirement.SourceURLs.Size() == 0 )
		{
			FString unavailable;
			unavailable.Format( "%s: no approved download source.", requirement.Name.GetChars() );
			modmanager_SetQueueFailure( unavailable );
			return;
		}
		if ( g_ModManagerCancel.load() )
		{
			std::lock_guard<std::mutex> lock( g_ModManagerMutex );
			g_ModManagerState = MODQUEUE_Cancelled;
			return;
		}

		FString attemptedErrors;
		bool downloaded = false;
		for ( unsigned int sourceIndex = 0; sourceIndex < requirement.SourceURLs.Size(); ++sourceIndex )
		{
			if ( g_ModManagerCancel.load() )
				break;
			requirement.SourceURL = requirement.SourceURLs[sourceIndex];
			requirement.SourceName = sourceIndex < requirement.SourceNames.Size() ? requirement.SourceNames[sourceIndex] : "source";
			const int sourceMode = sourceIndex < requirement.SourceModes.Size() ? requirement.SourceModes[sourceIndex] : static_cast<int>(MODSOURCE_DirectBasename);
			if ( developer ) Printf( "Mod download: %s via %s (%s)\n", requirement.Name.GetChars(), requirement.SourceName.GetChars(), requirement.SourceURL.GetChars() );
			FString tempName;
			{
				std::lock_guard<std::mutex> lock( g_ModManagerMutex );
				g_ModManagerActiveIndex = static_cast<int>(i);
				tempName.Format( "zandronum-mod-%u.part", ++g_ModManagerTempSerial );
				g_ModManagerQueueName = requirement.Name;
				g_ModManagerReceived = 0;
				g_ModManagerTotal = 0;
				g_ModManagerState = MODQUEUE_Downloading;
				g_ModManagerLastError = "";
				g_ModManagerRequest.Requirements[i].SourceURL = requirement.SourceURL;
				g_ModManagerRequest.Requirements[i].SourceName = requirement.SourceName;
				g_ModManagerRequest.Requirements[i].DownloadState = MODDL_Downloading;
				g_ModManagerRequest.Requirements[i].DownloadedBytes = 0;
				g_ModManagerRequest.Requirements[i].DownloadTotal = 0;
				g_ModManagerRequest.Requirements[i].Error = "";
			}

			FString error;
			const bool isSearchSource = sourceMode == static_cast<int>(MODSOURCE_FilenameSearch) || sourceMode == static_cast<int>(MODSOURCE_HtmlIndex) || sourceMode == static_cast<int>(MODSOURCE_IdgamesZip);
			if ( !modmanager_DownloadToTemp( requirement.SourceURL, tempName,
				isSearchSource ? MODMANAGER_MAX_SEARCH_RESPONSE_SIZE : MODMANAGER_MAX_FILE_SIZE, error ) )
			{
				modmanager_RemoveOwnedTemp( tempName );
				if ( g_ModManagerCancel.load() )
					break;
				if ( error.IsEmpty() )
					error = "download failed";
				if ( developer ) Printf( "Mod download: %s via %s failed: %s\n", requirement.Name.GetChars(), requirement.SourceName.GetChars(), error.GetChars() );
				attemptedErrors.AppendFormat( "%s: %s; ", requirement.SourceName.GetChars(), error.GetChars() );
				continue;
			}
			if ( sourceMode == static_cast<int>(MODSOURCE_FilenameSearch) || sourceMode == static_cast<int>(MODSOURCE_HtmlIndex) )
			{
				const FString directory = MODMANAGER_GetDirectory( true );
				if ( directory.IsEmpty() )
				{
					modmanager_RemoveOwnedTemp( tempName );
					attemptedErrors.AppendFormat( "%s: managed mods directory unavailable; ", requirement.SourceName.GetChars() );
					break;
				}
				const FString searchPath = directory + "/" + tempName;
				FString directURL;
				if ( !modmanager_FindSearchResultURL( searchPath, requirement.SourceURL, requirement.Name.GetChars(), directURL, error ) )
				{
					modmanager_RemoveOwnedTemp( tempName );
					if ( g_ModManagerCancel.load() )
						break;
					if ( error.IsEmpty() )
						error = "The search response did not identify a safe download.";
					attemptedErrors.AppendFormat( "%s: %s; ", requirement.SourceName.GetChars(), error.GetChars() );
					continue;
				}
				modmanager_RemoveOwnedTemp( tempName );
				if ( g_ModManagerCancel.load() )
					break;
				{
					std::lock_guard<std::mutex> lock( g_ModManagerMutex );
					g_ModManagerReceived = 0;
					g_ModManagerTotal = 0;
				}
				error = "";
				if ( !modmanager_DownloadToTemp( directURL, tempName, MODMANAGER_MAX_FILE_SIZE, error ) )
				{
					modmanager_RemoveOwnedTemp( tempName );
					if ( g_ModManagerCancel.load() )
						break;
					if ( error.IsEmpty() )
						error = "download failed";
					if ( developer ) Printf( "Mod download: %s via %s result failed: %s\n", requirement.Name.GetChars(), requirement.SourceName.GetChars(), error.GetChars() );
					attemptedErrors.AppendFormat( "%s: %s; ", requirement.SourceName.GetChars(), error.GetChars() );
					continue;
				}
			}
			else if ( sourceMode == static_cast<int>(MODSOURCE_IdgamesZip) )
			{
				const FString directory = MODMANAGER_GetDirectory( true );
				FString archiveName;
				FString archiveURL;
				FString parseError;
				const FString responsePath = directory.IsEmpty() ? "" : directory + "/" + tempName;
				std::string response;
				bool resolved = modmanager_BuildIdgamesArchiveName( requirement.Name.GetChars(), archiveName ) &&
					!directory.IsEmpty() && modmanager_ReadBoundedTextFile( responsePath, MODMANAGER_MAX_SEARCH_RESPONSE_SIZE, response, parseError ) &&
					modmanager_ParseIdgamesJSON( response, archiveName.GetChars(), archiveURL, parseError ) &&
					modmanager_IsValidHTTPSArchiveURL( archiveURL, archiveName.GetChars(), requirement.SourceURL );
				modmanager_RemoveOwnedTemp( tempName );
				if ( !resolved )
				{
					if ( g_ModManagerCancel.load() )
						break;
					if ( parseError.IsEmpty() )
						parseError = "The /idgames response did not identify a safe archive.";
					attemptedErrors.AppendFormat( "%s: %s; ", requirement.SourceName.GetChars(), parseError.GetChars() );
					continue;
				}
				FString archiveTempName;
				{
					std::lock_guard<std::mutex> lock( g_ModManagerMutex );
					archiveTempName.Format( "zandronum-mod-%u.part", ++g_ModManagerTempSerial );
				}
				error = "";
				if ( !modmanager_DownloadToTemp( archiveURL, archiveTempName, MODMANAGER_MAX_FILE_SIZE, error ) )
				{
					modmanager_RemoveOwnedTemp( archiveTempName );
					modmanager_RemoveOwnedTemp( tempName );
					if ( g_ModManagerCancel.load() )
						break;
					if ( error.IsEmpty() )
						error = "archive download failed";
					attemptedErrors.AppendFormat( "%s: %s; ", requirement.SourceName.GetChars(), error.GetChars() );
					continue;
				}
				const FString archivePath = directory + "/" + archiveTempName;
				error = "";
				const bool extracted = modmanager_ExtractZipEntry( archivePath, requirement.Name.GetChars(), tempName, error );
				modmanager_RemoveOwnedTemp( archiveTempName );
				if ( !extracted )
				{
					modmanager_RemoveOwnedTemp( tempName );
					if ( g_ModManagerCancel.load() )
						break;
					if ( error.IsEmpty() )
						error = "archive extraction failed";
					attemptedErrors.AppendFormat( "%s: %s; ", requirement.SourceName.GetChars(), error.GetChars() );
					continue;
				}
			}

			{
				std::lock_guard<std::mutex> lock( g_ModManagerMutex );
				g_ModManagerState = MODQUEUE_Verifying;
				if ( g_ModManagerActiveIndex == static_cast<int>(i) )
					g_ModManagerRequest.Requirements[i].DownloadState = MODDL_Verifying;
			}
			const FString directory = MODMANAGER_GetDirectory( true );
			if ( directory.IsEmpty() )
			{
				modmanager_RemoveOwnedTemp( tempName );
				attemptedErrors.AppendFormat( "%s: managed mods directory unavailable; ", requirement.SourceName.GetChars() );
				break;
			}
			const FString tempPath = directory + "/" + tempName;
			char digest[33];
			const bool verified = MD5SumOfFile( tempPath.GetChars(), digest ) && stricmp( digest, requirement.ExpectedMD5.GetChars() ) == 0;
			if ( !verified )
			{
				modmanager_RemoveOwnedTemp( tempName );
				attemptedErrors.AppendFormat( "%s: MD5 verification failed; ", requirement.SourceName.GetChars() );
				continue;
			}

			const FString finalPath = directory + "/" + requirement.Name;
			FString promotionError;
			if ( !modmanager_PromoteFile( tempPath, finalPath, promotionError ) )
			{
				modmanager_RemoveOwnedTemp( tempName );
				modmanager_SetQueueFailure( promotionError );
				return;
			}
			FString queueName;
			FString queueError;
			uint64_t queueReceived = 0;
			uint64_t queueTotal = 0;
			MODMANAGER_GetQueueInfo( queueName, queueReceived, queueTotal, queueError );
			requirement.Status = MODREQ_Found;
			requirement.DownloadState = MODDL_Downloaded;
			requirement.DownloadedBytes = queueReceived;
			requirement.DownloadTotal = queueTotal;
			requirement.ResolvedPath = finalPath;
			requirement.Error = "";
			requirement.Downloadable = false;
			{
				std::lock_guard<std::mutex> lock( g_ModManagerMutex );
				g_ModManagerRequest.Requirements[i] = requirement;
				g_ModManagerQueueName = "";
				g_ModManagerActiveIndex = -1;
			}
			downloaded = true;
			break;
		}

		if ( !downloaded )
		{
			if ( g_ModManagerCancel.load() )
			{
				std::lock_guard<std::mutex> lock( g_ModManagerMutex );
				g_ModManagerState = MODQUEUE_Cancelled;
				return;
			}
			if ( attemptedErrors.IsEmpty() )
				attemptedErrors = "all approved sources failed";
			FString failure;
			failure.Format( "%s: %s", requirement.Name.GetChars(), attemptedErrors.GetChars() );
			modmanager_SetQueueFailure( failure );
			return;
		}
	}

	FString error;
	if ( !modmanager_RecheckCurrentRequest( error ) )
	{
		modmanager_SetQueueFailure( error );
		return;
	}
	if ( g_ModManagerJoinAfter )
	{
		if ( !(g_ModManagerOfflineAfter ? MODMANAGER_PrepareOfflineRestart( error ) : MODMANAGER_PrepareJoinRestart( error )) )
		{
			modmanager_SetQueueFailure( error );
			return;
		}
	}
	std::lock_guard<std::mutex> lock( g_ModManagerMutex );
	g_ModManagerState = MODQUEUE_Complete;
	g_ModManagerReturnToRequirements = !g_ModManagerJoinAfter;
}

void MODMANAGER_Initialize()
{
	modmanager_LoadSources();
	const FString directory = MODMANAGER_GetDirectory( true );
	if ( directory.IsEmpty() )
		return;
	TArray<FFileList> entries;
	const FString scanDirectory = directory + "/";
	ScanDirectory( entries, scanDirectory.GetChars() );
	for ( unsigned int i = 0; i < entries.Size(); ++i )
	{
		if ( entries[i].isDirectory )
			continue;
		const FString prefix = directory + "/";
		const char *fullPath = entries[i].Filename.GetChars();
		if ( strncmp( fullPath, prefix.GetChars(), prefix.Len() ) != 0 )
			continue;
		FString canonicalEntry;
		if ( !modmanager_CanonicalPath( fullPath, canonicalEntry ) || !modmanager_IsWithinDirectory( directory, canonicalEntry ) )
			continue;
		const char *name = fullPath + prefix.Len();
		if ( strchr( name, '/' ) != NULL || strchr( name, '\\' ) != NULL )
			continue;
		const size_t nameLength = strlen( name );
		if ( strncmp( name, "zandronum-mod-", 14 ) == 0 && nameLength >= 5 && strcmp( name + nameLength - 5, ".part" ) == 0 )
		{
			std::remove( fullPath );
		}
	}
}

void MODMANAGER_Shutdown()
{
	MODMANAGER_CancelDownloads( false );
}

void MODMANAGER_Tick()
{
}

bool MODMANAGER_BuildServerRequest(ULONG server, FString &error)
{
	FModJoinRequest request;
	request.Address = BROWSER_GetAddress( server );
	request.IWADName = BROWSER_GetIWADName( server );
	request.HostName = BROWSER_GetHostName( server );
	request.ServerURL = BROWSER_GetWadURL( server );
	request.ServerCompatible = BROWSER_IsServerCompatible( server );
	if ( !BROWSER_IsActive( server ) )
	{
		error = "The selected server is no longer available.";
		return false;
	}

	for ( ULONG i = 0; i < static_cast<ULONG>(BROWSER_GetNumPWADs( server )); ++i )
	{
		FModRequirement requirement;
		modmanager_InitializeRequirement( requirement );
		requirement.Name = BROWSER_GetPWADName( server, i );
		requirement.ExpectedMD5 = BROWSER_GetPWADChecksum( server, i );
		requirement.Optional = BROWSER_IsPWADOptional( server, i );
		request.Requirements.Push( requirement );
	}

	if ( !modmanager_ResolveRequest( request, error ) )
		return false;
	{
		std::lock_guard<std::mutex> lock( g_ModManagerMutex );
		g_ModManagerRequest = request;
		g_ModManagerHasRequest = true;
		g_ModManagerLastError = "";
		g_ModManagerState = MODQUEUE_Idle;
		g_ModManagerPendingRestart = false;
		g_ModManagerActiveIndex = -1;
	}
	return true;
}

bool MODMANAGER_GetCurrentRequest(FModJoinRequest &request)
{
	std::lock_guard<std::mutex> lock( g_ModManagerMutex );
	if ( !g_ModManagerHasRequest )
		return false;
	request = g_ModManagerRequest;
	return true;
}

static bool modmanager_RecheckCurrentRequest(FString &error)
{
	FModJoinRequest request;
	if ( !MODMANAGER_GetCurrentRequest( request ) )
	{
		error = "No server requirements are selected.";
		return false;
	}
	if ( !modmanager_ResolveRequest( request, error ) )
		return false;
	std::lock_guard<std::mutex> lock( g_ModManagerMutex );
	g_ModManagerRequest = request;
	return true;
}

static bool modmanager_RequirementsReady(const FModJoinRequest &request)
{
	for ( unsigned int i = 0; i < request.Requirements.Size(); ++i )
	{
		if ( !request.Requirements[i].Optional && request.Requirements[i].Status != MODREQ_Found )
			return false;
	}
	return true;
}

bool MODMANAGER_RequestCanJoin(const FModJoinRequest &request)
{
	return request.ServerCompatible && modmanager_RequirementsReady( request );
}

bool MODMANAGER_RequestCanPlayOffline(const FModJoinRequest &request)
{
	return modmanager_RequirementsReady( request );
}

static bool modmanager_BuildRestartFiles(const FModJoinRequest &request, TArray<FString> &files, FString &error, bool requireServerCompatibility)
{
	if ( requireServerCompatibility && !request.ServerCompatible )
	{
		error = "The server version is incompatible with this build.";
		return false;
	}
	if ( !modmanager_RequirementsReady( request ) )
	{
		error = "Required server content is not ready:";
		for ( unsigned int i = 0; i < request.Requirements.Size(); ++i )
		{
			const FModRequirement &requirement = request.Requirements[i];
			if ( requirement.Optional || requirement.Status == MODREQ_Found )
				continue;
			error.AppendFormat( "\n%s%s", requirement.Name.GetChars(), requirement.Error.IsEmpty() ? "" : ": " );
			if ( requirement.Error.IsNotEmpty() )
				error += requirement.Error;
		}
		return false;
	}
	for ( unsigned int i = 0; i < request.Requirements.Size(); ++i )
	{
		if ( request.Requirements[i].Status == MODREQ_Found )
		{
			if ( !FileExists( request.Requirements[i].ResolvedPath.GetChars() ) )
			{
				error.Format( "Required server content is no longer available:\n%s", request.Requirements[i].Name.GetChars() );
				return false;
			}
			files.Push( request.Requirements[i].ResolvedPath );
		}
	}
	return true;
}

bool MODMANAGER_HasDownloadableMissing(const FModJoinRequest &request)
{
	bool hasRequiredDownload = false;
	for ( unsigned int i = 0; i < request.Requirements.Size(); ++i )
	{
		const FModRequirement &requirement = request.Requirements[i];
		if ( requirement.Optional || requirement.Status == MODREQ_Found )
			continue;
		if ( requirement.Status != MODREQ_Missing && requirement.Status != MODREQ_Incompatible )
			return false;
		if ( !requirement.Downloadable )
			return false;
		hasRequiredDownload = true;
	}
	return hasRequiredDownload;
}

static bool modmanager_PrepareRestart(bool connect, FString &error)
{
	FModJoinRequest request;
	if ( !MODMANAGER_GetCurrentRequest( request ) )
	{
		error = "No server requirements are selected.";
		return false;
	}
	TArray<FString> files;
	if ( !modmanager_BuildRestartFiles( request, files, error, connect ) )
		return false;
	{
		std::lock_guard<std::mutex> lock( g_ModManagerMutex );
		g_ModManagerRestartAddress = request.Address.ToString();
		g_ModManagerRestartIWAD = request.IWADName;
		g_ModManagerRestartFiles = files;
		g_ModManagerRestartConnect = connect;
		g_ModManagerPendingRestart = true;
	}
	return true;
}

bool MODMANAGER_PrepareJoinRestart(FString &error)
{
	return modmanager_PrepareRestart( true, error );
}

bool MODMANAGER_PrepareOfflineRestart(FString &error)
{
	return modmanager_PrepareRestart( false, error );
}

bool MODMANAGER_TakePendingRestart(FString &address, FString &iwad, TArray<FString> &files, bool &connect)
{
	std::lock_guard<std::mutex> lock( g_ModManagerMutex );
	if ( !g_ModManagerPendingRestart )
		return false;
	address = g_ModManagerRestartAddress;
	iwad = g_ModManagerRestartIWAD;
	files = g_ModManagerRestartFiles;
	connect = g_ModManagerRestartConnect;
	g_ModManagerPendingRestart = false;
	return true;
}

static bool modmanager_CheckOfflineFixture()
{
	FModJoinRequest fixture;
	fixture.ServerCompatible = true;
	fixture.IWADName = "doom2.wad";
	const char *names[] = { "fixture-first.wad", "fixture-second.pk3", "fixture-third.deh" };
	for ( unsigned int i = 0; i < countof( names ); ++i )
	{
		FModRequirement requirement;
		modmanager_InitializeRequirement( requirement );
		requirement.Name = names[i];
		requirement.Status = MODREQ_Found;
		requirement.ResolvedPath = names[i];
		fixture.Requirements.Push( requirement );
	}
	TArray<FString> files;
	FString error;
	if ( !modmanager_BuildRestartFiles( fixture, files, error, true ) || files.Size() != countof( names ) )
		return false;
	for ( unsigned int i = 0; i < countof( names ); ++i )
	{
		if ( strcmp( files[i].GetChars(), names[i] ) != 0 )
			return false;
	}
	return true;
}

static void modmanager_StartDownloads(bool joinAfter, bool offlineAfter)
{
	if ( g_ModManagerThread.joinable() )
		g_ModManagerThread.join();

	FModJoinRequest request;
	if ( !MODMANAGER_GetCurrentRequest( request ) )
		return;
	if ( joinAfter && !offlineAfter && !request.ServerCompatible )
	{
		modmanager_SetQueueFailure( "The server version is incompatible with this build." );
		return;
	}

	if ( offlineAfter ? MODMANAGER_RequestCanPlayOffline( request ) : MODMANAGER_RequestCanJoin( request ) )
	{
		if ( joinAfter )
		{
			FString error;
			if ( offlineAfter ? MODMANAGER_PrepareOfflineRestart( error ) : MODMANAGER_PrepareJoinRestart( error ) )
			{
				std::lock_guard<std::mutex> lock( g_ModManagerMutex );
				g_ModManagerState = MODQUEUE_Complete;
			}
			else
				modmanager_SetQueueFailure( error );
		}
		return;
	}

	if ( !MODMANAGER_HasDownloadableMissing( request ) )
	{
		modmanager_SetQueueFailure( "One or more required files have no approved download source." );
		return;
	}
	{
		std::lock_guard<std::mutex> lock( g_ModManagerMutex );
		g_ModManagerCancel.store( false );
		g_ModManagerJoinAfter = joinAfter;
		g_ModManagerOfflineAfter = offlineAfter;
		g_ModManagerReturnToRequirements = false;
		g_ModManagerActiveIndex = -1;
		g_ModManagerState = MODQUEUE_Downloading;
		g_ModManagerLastError = "";
		for ( unsigned int i = 0; i < g_ModManagerRequest.Requirements.Size(); ++i )
		{
			FModRequirement &requirement = g_ModManagerRequest.Requirements[i];
			if ( !requirement.Optional && requirement.Status != MODREQ_Found && requirement.Downloadable )
			{
				requirement.DownloadState = MODDL_Queued;
				requirement.DownloadedBytes = 0;
				requirement.DownloadTotal = 0;
				requirement.Error = "";
			}
		}
	}
	g_ModManagerThread = std::thread( modmanager_DownloadWorker );
}

void MODMANAGER_StartDownloads(bool joinAfter)
{
	modmanager_StartDownloads(joinAfter, false);
}

void MODMANAGER_StartOfflineDownloads()
{
	modmanager_StartDownloads(true, true);
}

void MODMANAGER_CancelDownloads(bool returnToRequirements)
{
	g_ModManagerCancel.store( true );
#ifdef __ANDROID__
	Zandronum_AndroidHost_CancelModDownload();
#endif
	if ( g_ModManagerThread.joinable() )
		g_ModManagerThread.join();
	{
		std::lock_guard<std::mutex> lock( g_ModManagerMutex );
		g_ModManagerState = MODQUEUE_Cancelled;
		g_ModManagerReturnToRequirements = returnToRequirements;
		g_ModManagerQueueName = "";
		g_ModManagerActiveIndex = -1;
		for ( unsigned int i = 0; i < g_ModManagerRequest.Requirements.Size(); ++i )
		{
			EModDownloadState state = g_ModManagerRequest.Requirements[i].DownloadState;
			if ( state == MODDL_Queued || state == MODDL_Downloading || state == MODDL_Verifying )
				g_ModManagerRequest.Requirements[i].DownloadState = MODDL_Cancelled;
		}
	}
}

void MODMANAGER_RetryFailed()
{
	bool joinAfter;
	bool offlineAfter;
	{
		std::lock_guard<std::mutex> lock( g_ModManagerMutex );
		joinAfter = g_ModManagerJoinAfter;
		offlineAfter = g_ModManagerOfflineAfter;
	}
	modmanager_StartDownloads( joinAfter, offlineAfter );
}

void MODMANAGER_CancelActiveDownload()
{
	g_ModManagerCancel.store( true );
}

void MODMANAGER_ReportPlatformProgress(uint64_t received, uint64_t total)
{
	std::lock_guard<std::mutex> lock( g_ModManagerMutex );
	g_ModManagerReceived = received;
	g_ModManagerTotal = total;
	if ( g_ModManagerActiveIndex >= 0 && static_cast<unsigned int>(g_ModManagerActiveIndex) < g_ModManagerRequest.Requirements.Size() )
	{
		FModRequirement &requirement = g_ModManagerRequest.Requirements[g_ModManagerActiveIndex];
		requirement.DownloadedBytes = received;
		requirement.DownloadTotal = total;
	}
}

EModQueueState MODMANAGER_GetQueueState()
{
	std::lock_guard<std::mutex> lock( g_ModManagerMutex );
	return g_ModManagerState;
}

void MODMANAGER_GetQueueInfo(FString &name, uint64_t &received, uint64_t &total, FString &error)
{
	std::lock_guard<std::mutex> lock( g_ModManagerMutex );
	name = g_ModManagerQueueName;
	received = g_ModManagerReceived;
	total = g_ModManagerTotal;
	error = g_ModManagerLastError;
}

bool MODMANAGER_ShouldReturnToRequirements()
{
	std::lock_guard<std::mutex> lock( g_ModManagerMutex );
	return g_ModManagerReturnToRequirements;
}

void MODMANAGER_ClearReturnFlag()
{
	std::lock_guard<std::mutex> lock( g_ModManagerMutex );
	g_ModManagerReturnToRequirements = false;
}

struct FModZipFixtureEntry
{
	std::string Name;
	std::string Data;
	bool Deflate;
	uint16_t Flags;
	uint32_t ExternalAttributes;
	uint32_t UncompressedSizeOverride;
};

static void modmanager_AppendLE16(std::vector<unsigned char> &output, uint16_t value)
{
	output.push_back( static_cast<unsigned char>( value & 0xff ) );
	output.push_back( static_cast<unsigned char>( value >> 8 ) );
}

static void modmanager_AppendLE32(std::vector<unsigned char> &output, uint32_t value)
{
	output.push_back( static_cast<unsigned char>( value & 0xff ) );
	output.push_back( static_cast<unsigned char>( ( value >> 8 ) & 0xff ) );
	output.push_back( static_cast<unsigned char>( ( value >> 16 ) & 0xff ) );
	output.push_back( static_cast<unsigned char>( value >> 24 ) );
}

static bool modmanager_WriteZipFixture(const FString &path, const std::vector<FModZipFixtureEntry> &entries)
{
	std::vector<unsigned char> archive;
	std::vector<unsigned char> directory;
	for ( const FModZipFixtureEntry &fixture : entries )
	{
		std::vector<unsigned char> compressed;
		const uint16_t method = fixture.Deflate ? 8 : 0;
		if ( fixture.Deflate )
		{
			z_stream stream;
			memset( &stream, 0, sizeof(stream) );
			if ( deflateInit2( &stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY ) != Z_OK )
				return false;
			const uLong bound = compressBound( static_cast<uLong>(fixture.Data.size()) );
			compressed.resize( bound );
			stream.next_in = reinterpret_cast<Bytef *>( const_cast<char *>( fixture.Data.data() ) );
			stream.avail_in = static_cast<uInt>( fixture.Data.size() );
			stream.next_out = compressed.data();
			stream.avail_out = static_cast<uInt>( compressed.size() );
			const int result = deflate( &stream, Z_FINISH );
			const bool valid = result == Z_STREAM_END;
			compressed.resize( compressed.size() - stream.avail_out );
			deflateEnd( &stream );
			if ( !valid )
				return false;
		}
		else
			compressed.assign( fixture.Data.begin(), fixture.Data.end() );
		const uint32_t crc = crc32( 0L, reinterpret_cast<const Bytef *>( fixture.Data.data() ), static_cast<uInt>(fixture.Data.size()) );
		const uint32_t compressedSize = static_cast<uint32_t>( compressed.size() );
		const uint32_t uncompressedSize = fixture.UncompressedSizeOverride != 0 ? fixture.UncompressedSizeOverride : static_cast<uint32_t>(fixture.Data.size());
		const uint32_t localOffset = static_cast<uint32_t>( archive.size() );
		modmanager_AppendLE32( archive, 0x04034b50U );
		modmanager_AppendLE16( archive, 20 );
		modmanager_AppendLE16( archive, fixture.Flags );
		modmanager_AppendLE16( archive, method );
		modmanager_AppendLE16( archive, 0 );
		modmanager_AppendLE16( archive, 0 );
		modmanager_AppendLE32( archive, crc );
		modmanager_AppendLE32( archive, compressedSize );
		modmanager_AppendLE32( archive, uncompressedSize );
		modmanager_AppendLE16( archive, static_cast<uint16_t>(fixture.Name.size()) );
		modmanager_AppendLE16( archive, 0 );
		archive.insert( archive.end(), fixture.Name.begin(), fixture.Name.end() );
		archive.insert( archive.end(), compressed.begin(), compressed.end() );

		modmanager_AppendLE32( directory, 0x02014b50U );
		modmanager_AppendLE16( directory, 20 );
		modmanager_AppendLE16( directory, 20 );
		modmanager_AppendLE16( directory, fixture.Flags );
		modmanager_AppendLE16( directory, method );
		modmanager_AppendLE16( directory, 0 );
		modmanager_AppendLE16( directory, 0 );
		modmanager_AppendLE32( directory, crc );
		modmanager_AppendLE32( directory, compressedSize );
		modmanager_AppendLE32( directory, uncompressedSize );
		modmanager_AppendLE16( directory, static_cast<uint16_t>(fixture.Name.size()) );
		modmanager_AppendLE16( directory, 0 );
		modmanager_AppendLE16( directory, 0 );
		modmanager_AppendLE16( directory, 0 );
		modmanager_AppendLE16( directory, 0 );
		modmanager_AppendLE32( directory, fixture.ExternalAttributes );
		modmanager_AppendLE32( directory, localOffset );
		directory.insert( directory.end(), fixture.Name.begin(), fixture.Name.end() );
	}
	const uint32_t directoryOffset = static_cast<uint32_t>( archive.size() );
	archive.insert( archive.end(), directory.begin(), directory.end() );
	modmanager_AppendLE32( archive, 0x06054b50U );
	modmanager_AppendLE16( archive, 0 );
	modmanager_AppendLE16( archive, 0 );
	modmanager_AppendLE16( archive, static_cast<uint16_t>(entries.size()) );
	modmanager_AppendLE16( archive, static_cast<uint16_t>(entries.size()) );
	modmanager_AppendLE32( archive, static_cast<uint32_t>(directory.size()) );
	modmanager_AppendLE32( archive, directoryOffset );
	modmanager_AppendLE16( archive, 0 );
	FILE *output = fopen( path.GetChars(), "wb" );
	if ( output == NULL )
		return false;
	const bool written = fwrite( archive.data(), 1, archive.size(), output ) == archive.size();
	fclose( output );
	return written;
}

CCMD( mod_manager_selftest )
{
	bool passed = true;
	passed = passed && MODMANAGER_IsSafeBasename( "sample.pk3" );
	passed = passed && MODMANAGER_IsSafeBasename( "sample.wad" );
	passed = passed && !MODMANAGER_IsSafeBasename( "../sample.pk3" );
	passed = passed && !MODMANAGER_IsSafeBasename( "sample.txt" );
	passed = passed && !MODMANAGER_IsSafeBasename( "sample%2e.pk3" );
	passed = passed && !MODMANAGER_IsSafeBasename( "sample?.pk3" );
	passed = passed && !MODMANAGER_IsSafeBasename( "sample.pk3." );
	passed = passed && modmanager_IsRecognizedAFSDirectory( "https://static.allfearthesentinel.com/wads/" );
	passed = passed && modmanager_IsValidAFSOverride( "https://static.allfearthesentinel.com/wads/" );
	passed = passed && modmanager_IsValidAFSOverride( "https://downloads.example.test/mods/" );
	passed = passed && !modmanager_IsValidAFSOverride( "http://downloads.example.test/mods/" );
	passed = passed && !modmanager_IsValidAFSOverride( "https://user@example.test/mods/" );
	passed = passed && !modmanager_IsValidAFSOverride( "https://example.test:0/mods/" );
	passed = passed && !modmanager_IsValidAFSOverride( "https://example.test:65536/mods/" );
	passed = passed && !modmanager_IsValidAFSOverride( "https://example.test/mods/?redirect=1" );
	passed = passed && !modmanager_IsValidAFSOverride( "https://example.test/mods/%2e%2e/" );
	passed = passed && modmanager_IsValidFilenameSearchTemplate( "https://doom.dogsoft.net/getwad.php?search=%WADNAME%" );
	passed = passed && !modmanager_IsValidFilenameSearchTemplate( "http://doom.dogsoft.net/getwad.php?search=%WADNAME%" );
	passed = passed && !modmanager_IsValidFilenameSearchTemplate( "https://doom.dogsoft.net/getwad.php?search=%WADNAME%&other=%2F" );
	passed = passed && modmanager_IsValidHTTPSPage( "https://doomshack.org/wadlist.php" );
	passed = passed && !modmanager_IsValidHTTPSPage( "http://doomshack.org/wadlist.php" );
	passed = passed && !modmanager_IsValidHTTPSPage( "https://doomshack.org/../wadlist.php" );
	FString archiveName;
	passed = passed && modmanager_BuildIdgamesArchiveName( "fixture.wad", archiveName ) && archiveName.CompareNoCase( "fixture.zip" ) == 0;
	passed = passed && !modmanager_BuildIdgamesArchiveName( "fixture.txt", archiveName );
	TArray<FString> sourceURLs;
	TArray<FString> sourceNames;
	TArray<int> sourceModes;
	passed = passed && modmanager_BuildSourceCandidates( "https://static.allfearthesentinel.com/wads/", "fixture+name.pk3", sourceURLs, sourceNames, sourceModes );
	passed = passed && sourceURLs.Size() >= 7 && sourceNames.Size() == sourceURLs.Size() && sourceModes.Size() == sourceURLs.Size();
	passed = passed && sourceURLs.Size() > 0 && sourceNames[0].CompareNoCase( "AFS" ) == 0;
	passed = passed && sourceURLs.Size() > 0 && sourceURLs[0].CompareNoCase( "https://static.allfearthesentinel.com/wads/fixture%2Bname.pk3" ) == 0;
	passed = passed && sourceURLs.Size() > 4 && sourceURLs[4].CompareNoCase( "https://doom.dogsoft.net/getwad.php?search=fixture%2Bname.pk3" ) == 0;
	passed = passed && sourceURLs.Size() > 5 && sourceURLs[5].CompareNoCase( "https://doomshack.org/wadlist.php" ) == 0;
	passed = passed && sourceURLs.Size() > 6 && sourceURLs[6].CompareNoCase( "https://www.doomworld.com/idgames/api/api.php?out=json&action=search&query=fixture%2Bname.zip&dir=desc" ) == 0;
	passed = passed && sourceModes.Size() > 4 && sourceModes[4] == static_cast<int>(MODSOURCE_FilenameSearch);
	passed = passed && sourceModes.Size() > 5 && sourceModes[5] == static_cast<int>(MODSOURCE_HtmlIndex);
	passed = passed && sourceModes.Size() > 6 && sourceModes[6] == static_cast<int>(MODSOURCE_IdgamesZip);
	FString searchResult;
	FString searchError;
	const std::string searchHTML = "<html><a href=\"/wads/fixture%2Bname.pk3\">download</a><a href=\"https://evil.example/fixture%2Bname.pk3\">bad</a></html>";
	passed = passed && modmanager_FindSearchResultURLInHTML( searchHTML, "https://doom.dogsoft.net/getwad.php?search=fixture%2Bname.pk3", "fixture+name.pk3", searchResult, searchError );
	passed = passed && searchResult.CompareNoCase( "https://doom.dogsoft.net/wads/fixture%2Bname.pk3" ) == 0;
	passed = passed && !modmanager_FindSearchResultURLInHTML( "<a href=\"http://doom.dogsoft.net/wads/fixture.pk3\">bad</a>", "https://doom.dogsoft.net/getwad.php?search=fixture.pk3", "fixture.pk3", searchResult, searchError );
	passed = passed && !modmanager_FindSearchResultURLInHTML( "<a href=\"/wads/%2e%2e/fixture.pk3\">bad</a>", "https://doom.dogsoft.net/getwad.php?search=fixture.pk3", "fixture.pk3", searchResult, searchError );
	FString idgamesURL;
	FString idgamesError;
	const FString idgamesSource = "https://www.doomworld.com/idgames/api/api.php?out=json&action=search&query=fixture.zip&dir=desc";
	const std::string idgamesJSON = "{\"content\":{\"file\":[{\"filename\":\"other.zip\",\"url\":\"https://www.doomworld.com/other.zip\"},{\"filename\":\"fixture.zip\",\"url\":\"https://www.doomworld.com/files/fixture.zip\"}]}}";
	const bool parsedIdgames = modmanager_ParseIdgamesJSON( idgamesJSON, "fixture.zip", idgamesURL, idgamesError );
	passed = passed && parsedIdgames;
	const bool validIdgamesURL = parsedIdgames && modmanager_IsValidHTTPSArchiveURL( idgamesURL, "fixture.zip", idgamesSource );
	passed = passed && validIdgamesURL;
	passed = passed && !modmanager_ParseIdgamesJSON( "{\"content\":{\"file\":[}", "fixture.zip", idgamesURL, idgamesError );
	passed = passed && !modmanager_ParseIdgamesJSON( "{\"content\":{\"file\":[{\"filename\":\"fixture.zip\",\"url\":\"https://example.test/a.zip\"},{\"filename\":\"fixture.zip\",\"url\":\"https://example.test/b.zip\"}]}}", "fixture.zip", idgamesURL, idgamesError );
	passed = passed && !modmanager_IsValidHTTPSArchiveURL( "http://www.doomworld.com/files/fixture.zip", "fixture.zip", idgamesSource );
	passed = passed && !modmanager_IsValidHTTPSArchiveURL( "https://www.doomworld.com/files/other.zip", "fixture.zip", idgamesSource );
	passed = passed && !modmanager_IsValidHTTPSArchiveURL( "https://example.test/files/fixture.zip", "fixture.zip", idgamesSource );

	FModJoinRequest downloadable;
	downloadable.ServerCompatible = true;
	FModRequirement required;
	modmanager_InitializeRequirement( required );
	required.Status = MODREQ_Missing;
	required.Downloadable = true;
	downloadable.Requirements.Push( required );
	FModRequirement optional;
	modmanager_InitializeRequirement( optional );
	optional.Status = MODREQ_Missing;
	optional.Optional = true;
	downloadable.Requirements.Push( optional );
	passed = passed && MODMANAGER_HasDownloadableMissing( downloadable );
	passed = passed && !MODMANAGER_RequestCanJoin( downloadable );

	downloadable.Requirements[0].Downloadable = false;
	passed = passed && !MODMANAGER_HasDownloadableMissing( downloadable );
	passed = passed && modmanager_CheckOfflineFixture();
	downloadable.Requirements[0].Status = MODREQ_Found;
	downloadable.ServerCompatible = false;
	passed = passed && !MODMANAGER_RequestCanJoin( downloadable );
	downloadable.ServerCompatible = true;
	passed = passed && MODMANAGER_RequestCanJoin( downloadable );
	passed = passed && !MODMANAGER_HasDownloadableMissing( downloadable );
	const FString testDirectory = MODMANAGER_GetDirectory( true );
	if ( !testDirectory.IsEmpty() )
	{
		const FString testArchivePath = testDirectory + "/zandronum-mod-selftest-archive.part";
		const FString testOutputName = "zandronum-mod-selftest-output.part";
		const bool artifactsAbsent = !DirEntryExists( testArchivePath.GetChars() ) && !DirEntryExists( ( testDirectory + "/" + testOutputName ).GetChars() );
		passed = passed && artifactsAbsent;
		if ( artifactsAbsent )
		{
			std::vector<FModZipFixtureEntry> validEntries( 1 );
			validEntries[0].Name = "levels/sample.wad";
			validEntries[0].Data = "native archive fixture";
			validEntries[0].Deflate = true;
			validEntries[0].Flags = 0;
			validEntries[0].ExternalAttributes = 0;
			validEntries[0].UncompressedSizeOverride = 0;
			g_ModManagerCancel.store( false );
			FString extractionError;
			passed = passed && modmanager_WriteZipFixture( testArchivePath, validEntries ) && modmanager_ExtractZipEntry( testArchivePath, "sample.wad", testOutputName, extractionError );
			std::string extracted;
			FString extractedError;
			passed = passed && modmanager_ReadBoundedTextFile( testDirectory + "/" + testOutputName, 1024, extracted, extractedError ) && extracted == "native archive fixture";
			modmanager_RemoveOwnedTemp( testOutputName );
			modmanager_RemoveOwnedTemp( "zandronum-mod-selftest-archive.part" );

			std::vector<FModZipFixtureEntry> hostile( 1 );
			hostile[0].Name = "../evil.wad";
			hostile[0].Data = "bad";
			hostile[0].Deflate = false;
			hostile[0].Flags = 0;
			hostile[0].ExternalAttributes = 0;
			hostile[0].UncompressedSizeOverride = 0;
			passed = passed && modmanager_WriteZipFixture( testArchivePath, hostile ) && !modmanager_ExtractZipEntry( testArchivePath, "sample.wad", testOutputName, extractionError );
			modmanager_RemoveOwnedTemp( "zandronum-mod-selftest-archive.part" );

			hostile[0].Name = "sample.wad";
			hostile.push_back( hostile[0] );
			passed = passed && modmanager_WriteZipFixture( testArchivePath, hostile ) && !modmanager_ExtractZipEntry( testArchivePath, "sample.wad", testOutputName, extractionError );
			modmanager_RemoveOwnedTemp( "zandronum-mod-selftest-archive.part" );

			hostile.resize( 1 );
			hostile[0].Flags = 1;
			passed = passed && modmanager_WriteZipFixture( testArchivePath, hostile ) && !modmanager_ExtractZipEntry( testArchivePath, "sample.wad", testOutputName, extractionError );
			modmanager_RemoveOwnedTemp( "zandronum-mod-selftest-archive.part" );

			hostile[0].Flags = 0;
			hostile[0].ExternalAttributes = 0xa0000000U;
			passed = passed && modmanager_WriteZipFixture( testArchivePath, hostile ) && !modmanager_ExtractZipEntry( testArchivePath, "sample.wad", testOutputName, extractionError );
			modmanager_RemoveOwnedTemp( "zandronum-mod-selftest-archive.part" );

			hostile[0].ExternalAttributes = 0;
			hostile[0].Data.assign( 4096, 'A' );
			hostile[0].Deflate = true;
			hostile[0].UncompressedSizeOverride = 500000;
			passed = passed && modmanager_WriteZipFixture( testArchivePath, hostile ) && !modmanager_ExtractZipEntry( testArchivePath, "sample.wad", testOutputName, extractionError );
			modmanager_RemoveOwnedTemp( "zandronum-mod-selftest-archive.part" );
		}
	}

#ifdef _WIN32
	passed = passed && modmanager_IsExpectedContentRange( L"bytes 0-15/16", 0, 15, 16 );
	passed = passed && modmanager_IsExpectedContentRange( L"bytes 0-15/16 ", 0, 15, 16 );
	passed = passed && !modmanager_IsExpectedContentRange( L"bytes 1-15/16", 0, 15, 16 );
	passed = passed && !modmanager_IsExpectedContentRange( L"bytes 0-15/17", 0, 15, 16 );
	passed = passed && !modmanager_IsExpectedContentRange( L"bytes 0-15/16 extra", 0, 15, 16 );
#endif

	passed = passed && !MODMANAGER_GetDirectory( true ).IsEmpty();
	Printf( "mod_manager_selftest: %s\n", passed ? "ok" : "failed" );
}

CCMD( mod_manager_offline_load )
{
	FString error;
	if ( !MODMANAGER_PrepareOfflineRestart( error ) )
	{
		if ( error.IsEmpty() ) error = "No verified server requirements are available.";
		Printf( "mod_manager_offline_load: %s\n", error.GetChars() );
		return;
	}
	C_DoCommand( "restart" );
}
