//-----------------------------------------------------------------------------
//
// Zandronum Source
// Copyright (C) 2016 Benjamin Berkels
// Copyright (C) 2016 Zandronum Development Team
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
// 3. Neither the name of the Zandronum Development Team nor the names of its
//    contributors may be used to endorse or promote products derived from this
//    software without specific prior written permission.
// 4. Redistributions in any form must be accompanied by information on how to
//    obtain complete source code for the software and any accompanying
//    software that uses the software. The source code must either be included
//    in the distribution or be available for no more than the cost of
//    distribution plus a nominal fee, and must be freely redistributable
//    under reasonable conditions. For an executable file, complete source
//    code means the source code for all modules it contains. It does not
//    include source code for modules or files that typically accompany the
//    major components of the operating system on which the executable file
//    runs.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
//
//
// Filename: menu/browsermenu.cpp
//
//-----------------------------------------------------------------------------

#include <float.h>

#include "menu/menu.h"
#include "c_dispatch.h"
#include "w_wad.h"
#include "sc_man.h"
#include "v_font.h"
#include "g_level.h"
#include "d_player.h"
#include "v_video.h"
#include "gi.h"
#include "i_system.h"
#include "c_bind.h"
#include "v_palette.h"
#include "d_event.h"
#include "d_gui.h"

#define NO_IMP
#include "menu/optionmenuitems.h"

#include "browser.h"
#include "mod_manager.h"

static	LONG	g_lSelectedServer = -1;
static	int		g_iSortedServers[MAX_BROWSER_SERVERS];
static	int		g_sortedServerListOffest = 0;

void M_RefreshServers( void );
void M_BuildServerList( void );
LONG M_CalcLastSortedIndex( void );
bool M_ShouldShowServer( LONG lServer );

static	void			browsermenu_SortServers( ULONG ulSortType );
static	int	STACK_ARGS	browsermenu_PingCompareFunc( const void *arg1, const void *arg2 );
static	int	STACK_ARGS	browsermenu_ServerNameCompareFunc( const void *arg1, const void *arg2 );
static	int	STACK_ARGS	browsermenu_MapNameCompareFunc( const void *arg1, const void *arg2 );
static	int	STACK_ARGS	browsermenu_PlayersCompareFunc( const void *arg1, const void *arg2 );

#define NUM_COLUMNS			5
#define	NUM_SERVER_SLOTS	8

CUSTOM_CVAR( Int, menu_browser_servers, 0, CVAR_ARCHIVE )
{
	M_BuildServerList();
}
CUSTOM_CVAR( Int, menu_browser_gametype, 0, CVAR_ARCHIVE )
{
	M_BuildServerList();
}
CUSTOM_CVAR( Int, menu_browser_sortby, 0, CVAR_ARCHIVE )
{
	M_BuildServerList();
}
CUSTOM_CVAR( Bool, menu_browser_showempty, true, CVAR_ARCHIVE )
{
	M_BuildServerList();
}
CUSTOM_CVAR( Bool, menu_browser_showfull, true, CVAR_ARCHIVE )
{
	M_BuildServerList();
}
CUSTOM_CVAR( String, menu_browser_filtername, "", CVAR_ARCHIVE ) // [AK]
{
	M_BuildServerList();
}

// =================================================================================================
//
// [BB] FOptionMenuServerBrowserLine
//
// =================================================================================================

bool FOptionMenuServerBrowserLine::Activate()
{
	S_Sound (CHAN_VOICE | CHAN_UI, "menu/choose", snd_menuvolume, ATTN_NONE);
	g_lSelectedServer = g_iSortedServers[ mSlotNum + g_sortedServerListOffest ];
	return true;
}

int FOptionMenuServerBrowserLine::Draw(FOptionMenuDescriptor *desc, int y, int indent, bool selected)
{
	const int serverNum = g_iSortedServers[ mSlotNum + g_sortedServerListOffest ];
	const int localIndent = indent - 80 * CleanXfac_1;

	// [AK] Predetermine the x-positions of every column.
	int columnXPositions[NUM_COLUMNS] = { 16, 48, 160, 224, 272 };
	for ( unsigned int i = 0; i < NUM_COLUMNS; i++ )
		columnXPositions[i] = columnXPositions[i] * CleanXfac_1 + localIndent;

	// [AK] If this is the first server slot on the list, draw the column headers above it.
	if ( mSlotNum == 0 )
	{
		const char *columnNames[NUM_COLUMNS] = { "PING", "NAME", "MAP", "TYPE", "PLYRS" };
		const int headerY = y - 2 * OptionSettings.mLinespacing * CleanYfac_1;

		for ( unsigned int i = 0; i < NUM_COLUMNS; i++ )
			screen->DrawText( SmallFont, CR_UNTRANSLATED, columnXPositions[i], headerY, columnNames[i], DTA_CleanNoMove_1, true, TAG_DONE );
	}

	if ( M_ShouldShowServer ( serverNum ) == false )
		return 0;

	char szString[256];
	const bool compatible = BROWSER_IsServerCompatible( serverNum );
	int color = ( serverNum == g_lSelectedServer ) ? CR_ORANGE : ( compatible ? CR_GRAY : CR_YELLOW );

	// Draw ping.
	sprintf( szString, "%d", static_cast<int> (BROWSER_GetPing( serverNum )));
	screen->DrawText( SmallFont, color, columnXPositions[0], y, szString, DTA_CleanNoMove_1, true, TAG_DONE );

	// Draw name.
	strncpy( szString, BROWSER_GetHostName( serverNum ), 12 );
	szString[12] = 0;
	if ( strlen( BROWSER_GetHostName( serverNum )) > 12 )
		sprintf( szString + strlen ( szString ), "..." );
	screen->DrawText( SmallFont, color, columnXPositions[1], y, szString, DTA_CleanNoMove_1, true, TAG_DONE );

	// Draw map.
	strncpy( szString, BROWSER_GetMapname( serverNum ), 8 );
	screen->DrawText( SmallFont, color, columnXPositions[2], y, szString, DTA_CleanNoMove_1, true, TAG_DONE );
	/*
	// Draw wad.
	if ( BROWSER_Get
	sprintf( szString, "%d", BROWSER_GetPing( lServer ));
	screen->DrawText( SmallFont, CR_GRAY, 160 * CleanXfac_1 + localIndent, y, "WAD", DTA_CleanNoMove_1, true, TAG_DONE );
	*/
	// Draw gametype.
	strncpy( szString, BROWSER_GetGameModeShortName( serverNum ), 8 );
	screen->DrawText( SmallFont, color, columnXPositions[3], y, szString, DTA_CleanNoMove_1, true, TAG_DONE );

	// Draw players.
	sprintf( szString, "%d/%d", static_cast<int> (BROWSER_GetNumPlayers( serverNum )), static_cast<int> (BROWSER_GetMaxClients( serverNum )));
	screen->DrawText( SmallFont, color, columnXPositions[4], y, szString, DTA_CleanNoMove_1, true, TAG_DONE );
	return localIndent;
}

bool FOptionMenuServerBrowserLine::Selectable()
{
	return ( M_ShouldShowServer ( g_iSortedServers[ mSlotNum + g_sortedServerListOffest ] ) );
}

bool FOptionMenuServerBrowserLine::MenuEvent (int mkey, bool fromcontroller)
{
	if ( mkey == MKEY_Left )
	{
		if ( g_sortedServerListOffest >= NUM_SERVER_SLOTS )
		{
			g_sortedServerListOffest -= NUM_SERVER_SLOTS;
			S_Sound (CHAN_VOICE | CHAN_UI, "menu/change", snd_menuvolume, ATTN_NONE);
			return true;
		}
		else
		{
			S_Sound (CHAN_VOICE | CHAN_UI, "menu/invalid", snd_menuvolume, ATTN_NONE);
			return false;
		}
	}
	else if ( mkey == MKEY_Right )
	{
		if ( g_sortedServerListOffest < M_CalcLastSortedIndex( ) - NUM_SERVER_SLOTS )
		{
			g_sortedServerListOffest += NUM_SERVER_SLOTS;
			S_Sound (CHAN_VOICE | CHAN_UI, "menu/change", snd_menuvolume, ATTN_NONE);
			return true;
		}
		else
		{
			S_Sound (CHAN_VOICE | CHAN_UI, "menu/invalid", snd_menuvolume, ATTN_NONE);
			return false;
		}
	}
	else
	{
		return FOptionMenuItem::MenuEvent(mkey, fromcontroller);
	}
}

// =================================================================================================
//
// [BB] DServerInfoMenu
//
// =================================================================================================

class DServerInfoMenu : public DOptionMenu
{
	DECLARE_CLASS( DServerInfoMenu, DOptionMenu )

public:
	DServerInfoMenu(){}

	void Drawer()
	{
		Super::Drawer();

		ULONG	ulIdx;
		ULONG	ulCurYPos;
		ULONG	ulTextHeight;
		char	szString[256];

		if ( g_lSelectedServer == -1 )
			return;

		ulCurYPos = 32;
		ulTextHeight = ( gameinfo.gametype == GAME_Doom ? 8 : 9 );

		sprintf( szString, "Name: \\cc%s", BROWSER_GetHostName( g_lSelectedServer ));
		V_ColorizeString( szString );
		screen->DrawText( SmallFont, CR_UNTRANSLATED, 16, ulCurYPos, szString, DTA_Clean, true, TAG_DONE );

		ulCurYPos += ulTextHeight;

		sprintf( szString, "Version: \\cc%s", BROWSER_GetVersion( g_lSelectedServer ));
		V_ColorizeString( szString );
		screen->DrawText( SmallFont, BROWSER_IsServerCompatible( g_lSelectedServer ) ? CR_UNTRANSLATED : CR_YELLOW, 16, ulCurYPos, szString, DTA_Clean, true, TAG_DONE );

		ulCurYPos += ulTextHeight;

		sprintf( szString, "IP: \\cc%s", BROWSER_GetAddress( g_lSelectedServer ).ToString() );
		V_ColorizeString( szString );
		screen->DrawText( SmallFont, CR_UNTRANSLATED, 16, ulCurYPos, szString, DTA_Clean, true, TAG_DONE );

		ulCurYPos += ulTextHeight;

		sprintf( szString, "Map: \\cc%s", BROWSER_GetMapname( g_lSelectedServer ));
		V_ColorizeString( szString );
		screen->DrawText( SmallFont, CR_UNTRANSLATED, 16, ulCurYPos, szString, DTA_Clean, true, TAG_DONE );

		ulCurYPos += ulTextHeight;

		sprintf( szString, "Gametype: \\cc%s", BROWSER_GetGameModeName( g_lSelectedServer ) );
		V_ColorizeString( szString );
		screen->DrawText( SmallFont, CR_UNTRANSLATED, 16, ulCurYPos, szString, DTA_Clean, true, TAG_DONE );

		ulCurYPos += ulTextHeight;

		sprintf( szString, "IWAD: \\cc%s", BROWSER_GetIWADName( g_lSelectedServer ));
		V_ColorizeString( szString );
		screen->DrawText( SmallFont, CR_UNTRANSLATED, 16, ulCurYPos, szString, DTA_Clean, true, TAG_DONE );

		ulCurYPos += ulTextHeight;

		sprintf( szString, "PWADs: \\cc%d", static_cast<int> (BROWSER_GetNumPWADs( g_lSelectedServer )));
		V_ColorizeString( szString );
		screen->DrawText( SmallFont, CR_UNTRANSLATED, 16, ulCurYPos, szString, DTA_Clean, true, TAG_DONE );

		ulCurYPos += ulTextHeight;

		for ( ulIdx = 0; ulIdx < static_cast<unsigned> (MIN( (int)BROWSER_GetNumPWADs( g_lSelectedServer ), 4 )); ulIdx++ )
		{
			sprintf( szString, "\\cc%s", BROWSER_GetPWADName( g_lSelectedServer, ulIdx ));
			V_ColorizeString( szString );
			screen->DrawText( SmallFont, CR_UNTRANSLATED, 32, ulCurYPos, szString, DTA_Clean, true, TAG_DONE );

			ulCurYPos += ulTextHeight;
		}

		sprintf( szString, "WAD URL: \\cc%s", BROWSER_GetWadURL( g_lSelectedServer ));
		V_ColorizeString( szString );
		screen->DrawText( SmallFont, CR_UNTRANSLATED, 16, ulCurYPos, szString, DTA_Clean, true, TAG_DONE );

		ulCurYPos += ulTextHeight;

		sprintf( szString, "Host e-mail: \\cc%s", BROWSER_GetEmailAddress( g_lSelectedServer ));
		V_ColorizeString( szString );
		screen->DrawText( SmallFont, CR_UNTRANSLATED, 16, ulCurYPos, szString, DTA_Clean, true, TAG_DONE );

		ulCurYPos += ulTextHeight;

		sprintf( szString, "Players: \\cc%d/%d", static_cast<int> (BROWSER_GetNumPlayers( g_lSelectedServer )), static_cast<int> (BROWSER_GetMaxClients( g_lSelectedServer )));
		V_ColorizeString( szString );
		screen->DrawText( SmallFont, CR_UNTRANSLATED, 16, ulCurYPos, szString, DTA_Clean, true, TAG_DONE );

		ulCurYPos += ulTextHeight;

		if ( BROWSER_GetNumPlayers( g_lSelectedServer ))
		{
			ulCurYPos += ulTextHeight;

			screen->DrawText( SmallFont, CR_UNTRANSLATED, 32, ulCurYPos, "NAME", DTA_Clean, true, TAG_DONE );
			screen->DrawText( SmallFont, CR_UNTRANSLATED, 192, ulCurYPos, "FRAGS", DTA_Clean, true, TAG_DONE );
			screen->DrawText( SmallFont, CR_UNTRANSLATED, 256, ulCurYPos, "PING", DTA_Clean, true, TAG_DONE );

			ulCurYPos += ( ulTextHeight * 2 );

			for ( ulIdx = 0; static_cast<signed> (ulIdx) < MIN( (int)BROWSER_GetNumPlayers( g_lSelectedServer ), 4 ); ulIdx++ )
			{
				sprintf( szString, "%s", BROWSER_GetPlayerName( g_lSelectedServer, ulIdx ));
				V_ColorizeString( szString );
				screen->DrawText( SmallFont, CR_GRAY, 32, ulCurYPos, szString, DTA_Clean, true, TAG_DONE );

				sprintf( szString, "%d", static_cast<int> (BROWSER_GetPlayerFragcount( g_lSelectedServer, ulIdx )));
				V_ColorizeString( szString );
				screen->DrawText( SmallFont, CR_GRAY, 192, ulCurYPos, szString, DTA_Clean, true, TAG_DONE );

				sprintf( szString, "%d", static_cast<int> (BROWSER_GetPlayerPing( g_lSelectedServer, ulIdx )));
				V_ColorizeString( szString );
				screen->DrawText( SmallFont, CR_GRAY, 256, ulCurYPos, szString, DTA_Clean, true, TAG_DONE );

				ulCurYPos += ulTextHeight;
			}
		}
	}
};

IMPLEMENT_CLASS( DServerInfoMenu )

// =================================================================================================
//
// [BB] DBrowserMenu
//
// Internal server browser menu
//
// =================================================================================================

class DBrowserMenu : public DOptionMenu
{
	DECLARE_CLASS( DBrowserMenu, DOptionMenu )

	int refreshTicker;
public:
	DBrowserMenu() : refreshTicker ( 0 ) {}

	void Init ( DMenu* parent = NULL, FOptionMenuDescriptor* desc = NULL )
	{
		M_RefreshServers();
		M_BuildServerList();
		Super::Init( parent, desc );
	}

	void Ticker ()
	{
		Super::Ticker();

		// [BB] Query the servers that didn't respond yet a couple of times.

		if ( refreshTicker >= 10 * TICRATE )
			return;

		if ( ( ++refreshTicker % TICRATE ) == 0 )
			BROWSER_QueryAllServers();
	}

	void Drawer()
	{
		Super::Drawer();

		FString str;
		const int numServers = static_cast<int> ( M_CalcLastSortedIndex( ) );
		if ( numServers > NUM_SERVER_SLOTS )
			str.Format( "Currently showing servers %d to %d out of %d", g_sortedServerListOffest + 1, MIN ( g_sortedServerListOffest + NUM_SERVER_SLOTS, numServers ), numServers );
		else
			str.Format( "Currently showing %d servers", numServers );
		screen->DrawText( SmallFont, CR_WHITE, 160 - ( SmallFont->StringWidth( str ) / 2 ), 190, str, DTA_Clean, true, TAG_DONE );
	}
};

IMPLEMENT_CLASS( DBrowserMenu )

//*****************************************************************************
//
void M_RefreshServers( void )
{
	// Don't do anything if we're still waiting for a response from the master server.
	if ( BROWSER_WaitingForMasterResponse( ))
		return;

	g_lSelectedServer = -1;
	g_sortedServerListOffest = 0;

	// First, clear the existing server list.
	BROWSER_ClearServerList( );

	// Then, query the master server.
	BROWSER_QueryMasterServer( );
}

//*****************************************************************************
//
LONG M_CalcLastSortedIndex( void )
{
	ULONG	ulIdx;

	for ( ulIdx = 0; ulIdx < MAX_BROWSER_SERVERS; ulIdx++ )
	{
		if ( M_ShouldShowServer( g_iSortedServers[ulIdx] ) == false )
			return ( ulIdx );
	}

	return ( ulIdx );
}

//*****************************************************************************
//
void M_BuildServerList( void )
{
	browsermenu_SortServers( menu_browser_sortby );
	g_sortedServerListOffest = 0;
}

//*****************************************************************************
//
bool M_ShouldShowServer( LONG lServer )
{
	// Don't show inactive servers.
	if ( BROWSER_IsActive( lServer ) == false )
		return ( false );
/*
	// Don't show servers that don't have the same IWAD we do.
	if ( stricmp( SERVER_MASTER_GetIWADName( ), BROWSER_GetIWADName( lServer )) != 0 )
		return ( false );
*/
	// Don't show Internet servers if we are only showing LAN servers.
	if ( menu_browser_servers == 1 )
	{
		if ( BROWSER_IsLAN( lServer ) == false )
			return ( false );
	}

	// Don't show LAN servers if we are only showing Internet servers.
	if ( menu_browser_servers == 0 )
	{
		if ( BROWSER_IsLAN( lServer ) == true )
			return ( false );
	}

	// Don't show empty servers.
	if ( menu_browser_showempty == false )
	{
		if ( BROWSER_GetNumPlayers( lServer ) == 0 )
			return ( false );
	}

	// Don't show full servers.
	if ( menu_browser_showfull == false )
	{
		if ( BROWSER_GetNumPlayers( lServer ) ==  BROWSER_GetMaxClients( lServer ))
			return ( false );
	}

	// Don't show servers that have the gameplay mode we want.
	if ( menu_browser_gametype != 0 )
	{
		if ( BROWSER_GetGameMode( lServer ) != ( menu_browser_gametype - 1 ))
			return ( false );
	}

	// [AK] Only show servers containing words that we want to filter in.
	if ( strlen( menu_browser_filtername ) > 0 )
	{
		FString hostName = BROWSER_GetHostName( lServer );
		FString filterName = menu_browser_filtername.GetGenericRep( CVAR_String ).String;

		// [AK] A filter string that's longer than the server's name obviously means that it can't be shown.
		if ( filterName.Len( ) > hostName.Len( ))
			return ( false );

		// [AK] Set both strings to lowercase first.
		hostName.ToLower( );
		filterName.ToLower( );

		if ( strstr( hostName, filterName ) == NULL )
			return ( false );
	}

	return ( true );
}

//*****************************************************************************
//
static void browsermenu_SortServers( ULONG ulSortType )
{
	ULONG	ulIdx;

	for ( ulIdx = 0; ulIdx < MAX_BROWSER_SERVERS; ulIdx++ )
		g_iSortedServers[ulIdx] = ulIdx;

	switch ( ulSortType )
	{
	// Ping.
	case 0:

		qsort( g_iSortedServers, MAX_BROWSER_SERVERS, sizeof( int ), browsermenu_PingCompareFunc );
		break;
	// Server name.
	case 1:

		qsort( g_iSortedServers, MAX_BROWSER_SERVERS, sizeof( int ), browsermenu_ServerNameCompareFunc );
		break;
	// Map name.
	case 2:

		qsort( g_iSortedServers, MAX_BROWSER_SERVERS, sizeof( int ), browsermenu_MapNameCompareFunc );
		break;
	// Players.
	case 3:

		qsort( g_iSortedServers, MAX_BROWSER_SERVERS, sizeof( int ), browsermenu_PlayersCompareFunc );
		break;
	}
}

//*****************************************************************************
//
static int STACK_ARGS browsermenu_PingCompareFunc( const void *arg1, const void *arg2 )
{
	if (( M_ShouldShowServer( *(int *)arg1 ) == false ) && ( M_ShouldShowServer( *(int *)arg2 ) == false ))
		return ( 0 );

	if ( M_ShouldShowServer( *(int *)arg1 ) == false )
		return ( 1 );

	if ( M_ShouldShowServer( *(int *)arg2 ) == false )
		return ( -1 );

	return ( BROWSER_GetPing( *(int *)arg1 ) - BROWSER_GetPing( *(int *)arg2 ));
}

//*****************************************************************************
//
static int STACK_ARGS browsermenu_ServerNameCompareFunc( const void *arg1, const void *arg2 )
{
	if (( M_ShouldShowServer( *(int *)arg1 ) == false ) && ( M_ShouldShowServer( *(int *)arg2 ) == false ))
		return ( 0 );

	if ( M_ShouldShowServer( *(int *)arg1 ) == false )
		return ( 1 );

	if ( M_ShouldShowServer( *(int *)arg2 ) == false )
		return ( -1 );

	return ( stricmp( BROWSER_GetHostName( *(int *)arg1 ), BROWSER_GetHostName( *(int *)arg2 )));
}

//*****************************************************************************
//
static int STACK_ARGS browsermenu_MapNameCompareFunc( const void *arg1, const void *arg2 )
{
	if (( M_ShouldShowServer( *(int *)arg1 ) == false ) && ( M_ShouldShowServer( *(int *)arg2 ) == false ))
		return ( 0 );

	if ( M_ShouldShowServer( *(int *)arg1 ) == false )
		return ( 1 );

	if ( M_ShouldShowServer( *(int *)arg2 ) == false )
		return ( -1 );

	return ( stricmp( BROWSER_GetMapname( *(int *)arg1 ), BROWSER_GetMapname( *(int *)arg2 )));
}

//*****************************************************************************
//
static int STACK_ARGS browsermenu_PlayersCompareFunc( const void *arg1, const void *arg2 )
{
	if (( M_ShouldShowServer( *(int *)arg1 ) == false ) && ( M_ShouldShowServer( *(int *)arg2 ) == false ))
		return ( 0 );

	if ( M_ShouldShowServer( *(int *)arg1 ) == false )
		return ( 1 );

	if ( M_ShouldShowServer( *(int *)arg2 ) == false )
		return ( -1 );

	return ( BROWSER_GetNumPlayers( *(int *)arg2 ) - BROWSER_GetNumPlayers( *(int *)arg1 ));
}

static const char *browsermenu_ModDownloadState(const FModRequirement &requirement)
{
	switch ( requirement.DownloadState )
	{
	case MODDL_Queued:
		return "queued";
	case MODDL_Downloading:
		return "downloading";
	case MODDL_Verifying:
		return "verifying";
	case MODDL_Downloaded:
		return "downloaded";
	case MODDL_Failed:
		return "failed";
	case MODDL_Cancelled:
		return "cancelled";
	default:
		break;
	}
	if ( requirement.Status == MODREQ_Found )
		return "verified";
	if ( requirement.Status == MODREQ_Unknown )
		return "unknown";
	return requirement.Downloadable ? "available" : "unavailable";
}

static EColorRange browsermenu_ModDownloadColor(const FModRequirement &requirement)
{
	switch ( requirement.DownloadState )
	{
	case MODDL_Downloaded:
		return CR_GREEN;
	case MODDL_Failed:
		return CR_RED;
	case MODDL_Downloading:
		return CR_WHITE;
	case MODDL_Verifying:
	case MODDL_Queued:
	case MODDL_Cancelled:
		return CR_YELLOW;
	default:
		break;
	}
	return requirement.Status == MODREQ_Found ? CR_GREEN : CR_RED;
}

static FString browsermenu_FormatModDownloadSize(uint64_t bytes)
{
	FString result;
	if (bytes == 0)
		return "size unknown";
	if (bytes >= 1024ULL * 1024ULL * 1024ULL)
		result.Format("%.1f GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
	else if (bytes >= 1024ULL * 1024ULL)
		result.Format("%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
	else if (bytes >= 1024ULL)
		result.Format("%.1f KB", static_cast<double>(bytes) / 1024.0);
	else
		result.Format("%llu B", static_cast<unsigned long long>(bytes));
	return result;
}

static uint64_t browsermenu_GetMissingModDownloadTotal(const FModJoinRequest &request, bool &complete)
{
	uint64_t total = 0;
	complete = true;
	for (unsigned int i = 0; i < request.Requirements.Size(); ++i)
	{
		const FModRequirement &requirement = request.Requirements[i];
		if (requirement.Optional || requirement.Status == MODREQ_Found)
			continue;
		if (requirement.DownloadTotal == 0 || total > UINT64_MAX - requirement.DownloadTotal)
		{
			complete = false;
			continue;
		}
		total += requirement.DownloadTotal;
	}
	return total;
}

static FString browsermenu_ModProgressPrefix(const FModRequirement &requirement)
{
	if (requirement.DownloadState == MODDL_NotNeeded)
		return "";
	unsigned int percent = 0;
	if (requirement.DownloadState == MODDL_Downloaded || requirement.Status == MODREQ_Found)
		percent = 100;
	else if (requirement.DownloadTotal > 0)
	{
		percent = static_cast<unsigned int>((requirement.DownloadedBytes >= requirement.DownloadTotal)
			? 100 : (requirement.DownloadedBytes * 100) / requirement.DownloadTotal);
	}
	FString result;
	result.Format("[%3u%%] ", percent);
	return result;
}

class DLegacyModDownloadMenu : public DMenu
{
	DECLARE_CLASS( DLegacyModDownloadMenu, DMenu )

	bool mJoinAfter;
	bool mRestartIssued;
	unsigned int mFirstItem;

public:
	DLegacyModDownloadMenu()
		: DMenu( NULL ), mJoinAfter( false ), mRestartIssued( false ), mFirstItem( 0 )
	{
	}

	DLegacyModDownloadMenu( DMenu *parent, bool joinAfter )
		: DMenu( parent ), mJoinAfter( joinAfter ), mRestartIssued( false ), mFirstItem( 0 )
	{
	}

	void Ticker()
	{
		Super::Ticker();
		MODMANAGER_Tick();
		if ( !mJoinAfter && MODMANAGER_GetQueueState() == MODQUEUE_Complete && MODMANAGER_ShouldReturnToRequirements() )
		{
			MODMANAGER_ClearReturnFlag();
			Close();
			return;
		}
		if ( mJoinAfter && !mRestartIssued && MODMANAGER_GetQueueState() == MODQUEUE_Complete )
		{
			mRestartIssued = true;
			M_ClearMenus();
			C_DoCommand( "restart" );
		}
	}

	void Drawer()
	{
		Super::Drawer();
		FString name;
		FString error;
		uint64_t received;
		uint64_t total;
		MODMANAGER_GetQueueInfo( name, received, total, error );
		FModJoinRequest request;
		MODMANAGER_GetCurrentRequest( request );

		screen->DrawText( BigFont, CR_WHITE, 160 - BigFont->StringWidth( "MOD DOWNLOAD" ) / 2, 16, "MOD DOWNLOAD", DTA_Clean, true, TAG_DONE );
		int y = 40;
		if ( mFirstItem >= request.Requirements.Size() && request.Requirements.Size() > 0 )
			mFirstItem = request.Requirements.Size() - 1;
		const unsigned int lastItem = mFirstItem + 8 < request.Requirements.Size() ? mFirstItem + 8 : request.Requirements.Size();
		FString line;
		for ( unsigned int i = mFirstItem; i < lastItem; ++i, y += 13 )
		{
			const FModRequirement &requirement = request.Requirements[i];
			const FString progress = browsermenu_ModProgressPrefix(requirement);
			const EColorRange color = browsermenu_ModDownloadColor( requirement );
			screen->DrawText( SmallFont, color, 12, y, progress, DTA_Clean, true, TAG_DONE );
			screen->DrawText( SmallFont, color, 52, y, requirement.Name, DTA_Clean, true, TAG_DONE );
			screen->DrawText( SmallFont, color, 244, y, browsermenu_ModDownloadState( requirement ), DTA_Clean, true, TAG_DONE );
		}
		if ( request.Requirements.Size() > 8 )
		{
			line.Format( "Items %u-%u of %u  Up/Down: scroll", mFirstItem + 1, lastItem, request.Requirements.Size() );
			screen->DrawText( SmallFont, CR_GRAY, 12, 144, line, DTA_Clean, true, TAG_DONE );
		}
		switch ( MODMANAGER_GetQueueState() )
		{
		case MODQUEUE_Downloading:
			line.Format( "Downloading: %s", name.GetChars() );
			screen->DrawText( SmallFont, CR_WHITE, 12, 156, line, DTA_Clean, true, TAG_DONE );
			if (total > 0)
				line.Format( "Received: %s / %s", browsermenu_FormatModDownloadSize(received).GetChars(), browsermenu_FormatModDownloadSize(total).GetChars() );
			else if (received > 0)
				line.Format( "Received: %s", browsermenu_FormatModDownloadSize(received).GetChars() );
			else
				line = "Waiting for server response...";
			screen->DrawText( SmallFont, CR_GRAY, 12, 168, line, DTA_Clean, true, TAG_DONE );
			break;
		case MODQUEUE_Verifying:
			screen->DrawText( SmallFont, CR_YELLOW, 12, 156, "Verifying downloaded content...", DTA_Clean, true, TAG_DONE );
			break;
		case MODQUEUE_Complete:
			screen->DrawText( SmallFont, CR_GREEN, 12, 156, mJoinAfter ? "Download complete. Restarting..." : "Download complete. Returning...", DTA_Clean, true, TAG_DONE );
			break;
		case MODQUEUE_Failed:
			line.Format( "Download failed: %s", error.GetChars() );
			screen->DrawText( SmallFont, CR_RED, 12, 156, line, DTA_Clean, true, TAG_DONE );
			screen->DrawText( SmallFont, CR_WHITE, 12, 168, "Enter: retry", DTA_Clean, true, TAG_DONE );
			break;
		case MODQUEUE_Cancelled:
			screen->DrawText( SmallFont, CR_YELLOW, 12, 156, "Download cancelled.", DTA_Clean, true, TAG_DONE );
			break;
		default:
			screen->DrawText( SmallFont, CR_WHITE, 12, 156, "Preparing download...", DTA_Clean, true, TAG_DONE );
			break;
		}
		screen->DrawText( SmallFont, CR_ORANGE, 12, 192, "Cancel and return to server browser", DTA_Clean, true, TAG_DONE );
	}

	bool MenuEvent( int mkey, bool fromcontroller )
	{
		FModJoinRequest request;
		MODMANAGER_GetCurrentRequest( request );
		if ( mkey == MKEY_Up && mFirstItem > 0 )
		{
			--mFirstItem;
			return true;
		}
		if ( mkey == MKEY_Down && mFirstItem + 8 < request.Requirements.Size() )
		{
			++mFirstItem;
			return true;
		}
		if ( mkey == MKEY_Enter && MODMANAGER_GetQueueState() == MODQUEUE_Failed )
		{
			MODMANAGER_RetryFailed();
			return true;
		}
		if ( mkey == MKEY_Back )
		{
			if ( MODMANAGER_GetQueueState() == MODQUEUE_Downloading || MODMANAGER_GetQueueState() == MODQUEUE_Verifying )
				MODMANAGER_CancelActiveDownload();
			DMenu *requirementsMenu = mParentMenu;
			Close();
			if (requirementsMenu != NULL && DMenu::CurrentMenu == requirementsMenu)
				requirementsMenu->Close();
			return true;
		}
		if ( mkey == MKEY_Enter && MODMANAGER_GetQueueState() == MODQUEUE_Complete && !mJoinAfter )
		{
			Close();
			return true;
		}
		if ( mkey == MKEY_Enter )
		{
			MODMANAGER_CancelActiveDownload();
			DMenu *requirementsMenu = mParentMenu;
			Close();
			if ( requirementsMenu != NULL && DMenu::CurrentMenu == requirementsMenu )
				requirementsMenu->Close();
			return true;
		}
		return Super::MenuEvent( mkey, fromcontroller );
	}
};

IMPLEMENT_CLASS( DLegacyModDownloadMenu )

class DLegacyModRequirementsMenu : public DMenu
{
	DECLARE_CLASS( DLegacyModRequirementsMenu, DMenu )

	int mSelection;
	unsigned int mFirstItem;
	bool mOffline;

	bool CanDownload( const FModJoinRequest &request ) const
	{
		return MODMANAGER_HasDownloadableMissing( request );
	}

public:
	DLegacyModRequirementsMenu()
		: DMenu( NULL ), mSelection( 0 ), mFirstItem( 0 ), mOffline( false )
	{
	}

	DLegacyModRequirementsMenu( DMenu *parent, bool offline = false )
		: DMenu( parent ), mSelection( 0 ), mFirstItem( 0 ), mOffline( offline )
	{
	}

	void Drawer()
	{
		Super::Drawer();
		FModJoinRequest request;
		if ( !MODMANAGER_GetCurrentRequest( request ) )
			return;

		screen->DrawText( BigFont, CR_WHITE, 160 - BigFont->StringWidth( "SERVER REQUIREMENTS" ) / 2, 12, "SERVER REQUIREMENTS", DTA_Clean, true, TAG_DONE );
		FString line;
		line.Format( "Server: %s%s", request.HostName.GetChars(), request.ServerCompatible ? "" : " (incompatible version)" );
		screen->DrawText( SmallFont, request.ServerCompatible ? CR_GRAY : CR_YELLOW, 16, 34, line, DTA_Clean, true, TAG_DONE );

		if ( mFirstItem + 8 > request.Requirements.Size() )
			mFirstItem = request.Requirements.Size() > 8 ? request.Requirements.Size() - 8 : 0;
		const unsigned int lastItem = mFirstItem + 8 < request.Requirements.Size() ? mFirstItem + 8 : request.Requirements.Size();
		int y = 52;
		for ( unsigned int i = mFirstItem; i < lastItem; ++i, y += 14 )
		{
			const FModRequirement &requirement = request.Requirements[i];
			EColorRange color = CR_YELLOW;
			const char *status = "unknown";
			if ( requirement.Status == MODREQ_Found )
			{
				color = CR_GREEN;
				status = "verified";
			}
			else if ( requirement.Status == MODREQ_Missing )
			{
				color = CR_RED;
				status = requirement.Downloadable ? "missing - downloadable" : "missing";
			}
			else if ( requirement.Status == MODREQ_Incompatible )
			{
				color = CR_RED;
				status = requirement.Downloadable ? "incompatible - downloadable" : "incompatible";
			}
			FString sizePrefix;
			if (requirement.Status != MODREQ_Found && requirement.DownloadTotal > 0)
				sizePrefix.Format("[%s] ", browsermenu_FormatModDownloadSize(requirement.DownloadTotal).GetChars());
			if ( !requirement.Downloadable && requirement.Error.IsNotEmpty() )
				line.Format( "  %s%s%s - %s (%s)", sizePrefix.GetChars(), requirement.Name.GetChars(), requirement.Optional ? " (optional)" : "", status, requirement.Error.GetChars() );
			else if ( requirement.Downloadable && requirement.SourceName.IsNotEmpty() )
				line.Format( "  %s%s%s - %s (%s)", sizePrefix.GetChars(), requirement.Name.GetChars(), requirement.Optional ? " (optional)" : "", status, requirement.SourceName.GetChars() );
			else
				line.Format( "  %s%s%s - %s", sizePrefix.GetChars(), requirement.Name.GetChars(), requirement.Optional ? " (optional)" : "", status );
			screen->DrawText( SmallFont, color, 16, y, line, DTA_Clean, true, TAG_DONE );
		}
		if ( request.Requirements.Size() > 8 )
		{
			line.Format( "Items %u-%u of %u  PgUp/PgDn: scroll", mFirstItem + 1, lastItem, request.Requirements.Size() );
			screen->DrawText( SmallFont, CR_GRAY, 16, 164, line, DTA_Clean, true, TAG_DONE );
		}

		const int actionY = 180;
		const bool ready = mOffline ? MODMANAGER_RequestCanPlayOffline( request ) : MODMANAGER_RequestCanJoin( request );
		const bool downloadable = CanDownload( request );
		const bool downloadAndJoin = downloadable && (mOffline || request.ServerCompatible);
		bool sizeKnown = false;
		const uint64_t totalBytes = browsermenu_GetMissingModDownloadTotal(request, sizeKnown);
		const FString totalSize = browsermenu_FormatModDownloadSize(totalBytes);
		FString actions[4] = {
			mOffline ? (ready ? "Play Now" : "Play Now (not ready)") : (ready ? "Join Now" : ( request.ServerCompatible ? "Join Now (not ready)" : "Join Now (incompatible)" )),
			downloadable ? "" : "Download Only (unavailable)",
			mOffline ? (downloadAndJoin ? "" : "Download and Play Offline (unavailable)") : (downloadAndJoin ? "" : ( request.ServerCompatible ? "Download and Join (unavailable)" : "Download and Join (incompatible)" )),
			"Back"
		};
		if (downloadable && sizeKnown)
			actions[1].Format("[%s] Download Only", totalSize.GetChars());
		else if (downloadable)
			actions[1] = "Download Only";
		if (downloadAndJoin && sizeKnown)
			actions[2].Format("[%s] %s", totalSize.GetChars(), mOffline ? "Download and Play Offline" : "Download and Join");
		else if (downloadAndJoin)
			actions[2] = mOffline ? "Download and Play Offline" : "Download and Join";
		for ( int i = 0; i < 4; ++i )
			screen->DrawText( SmallFont, i == mSelection ? CR_ORANGE : CR_WHITE, 24, actionY + i * 12, actions[i], DTA_Clean, true, TAG_DONE );
	}

	bool MenuEvent( int mkey, bool fromcontroller )
	{
		FModJoinRequest request;
		if ( !MODMANAGER_GetCurrentRequest( request ) )
			return Super::MenuEvent( mkey, fromcontroller );

		if ( mkey == MKEY_PageUp && mFirstItem > 0 )
		{
			mFirstItem = mFirstItem > 8 ? mFirstItem - 8 : 0;
			return true;
		}
		if ( mkey == MKEY_PageDown && mFirstItem + 8 < request.Requirements.Size() )
		{
			mFirstItem += 8;
			if ( mFirstItem + 8 > request.Requirements.Size() )
				mFirstItem = request.Requirements.Size() - 8;
			return true;
		}

		if ( mkey == MKEY_Up || mkey == MKEY_Down )
		{
			mSelection += mkey == MKEY_Up ? -1 : 1;
			if ( mSelection < 0 ) mSelection = 3;
			if ( mSelection > 3 ) mSelection = 0;
			S_Sound( CHAN_VOICE | CHAN_UI, "menu/cursor", snd_menuvolume, ATTN_NONE );
			return true;
		}
		if ( mkey == MKEY_Enter )
		{
			const bool ready = MODMANAGER_RequestCanJoin( request );
			const bool downloadable = CanDownload( request );
			if ( mSelection == 3 )
			{
				Close();
				return true;
			}
			if ( mSelection == 0 )
			{
				FString error;
				if ( !mOffline && !request.ServerCompatible )
					error = "The server version is incompatible with this build.";
				if ( (!mOffline && !request.ServerCompatible) || !(mOffline ? MODMANAGER_PrepareOfflineRestart( error ) : MODMANAGER_PrepareJoinRestart( error )) )
				{
					if ( error.IsEmpty() ) error = "Required server content is not ready.";
					error += "\n\nPress a key.";
					M_StartMessage( error.GetChars(), 1 );
				}
				else
				{
					M_ClearMenus();
					C_DoCommand( "restart" );
				}
				return true;
			}
			if ( mSelection == 1 || mSelection == 2 )
			{
				if ( mSelection == 2 && !mOffline && !request.ServerCompatible )
				{
					M_StartMessage( "The server version is incompatible with this build.\n\nPress a key.", 1 );
					return true;
				}
				if ( !downloadable )
				{
					M_StartMessage( "No approved download source is available.\n\nPress a key.", 1 );
					return true;
				}
				if ( mSelection == 2 && mOffline )
					MODMANAGER_StartOfflineDownloads();
				else
					MODMANAGER_StartDownloads( mSelection == 2 );
				M_ActivateMenu( new DLegacyModDownloadMenu( this, mSelection == 2 ) );
				return true;
			}
		}
		return Super::MenuEvent( mkey, fromcontroller );
	}
};

IMPLEMENT_CLASS( DLegacyModRequirementsMenu )

class DManagedModsMenu : public DMenu
{
	DECLARE_CLASS( DManagedModsMenu, DMenu )

	unsigned int mFirstItem;
	int mSelection;
	bool mConfirmClear;
	TArray<FString> mMarked;

	bool IsMarked(const char *name) const
	{
		for (unsigned int i = 0; i < mMarked.Size(); ++i) if (stricmp(mMarked[i], name) == 0) return true;
		return false;
	}

	void ToggleMarked(const char *name)
	{
		for (unsigned int i = 0; i < mMarked.Size(); ++i) if (stricmp(mMarked[i], name) == 0) { mMarked.Delete(i); return; }
		mMarked.Push(name);
	}

	void RefreshSelection(const TArray<FManagedModFile> &files)
	{
		const int last = static_cast<int>(files.Size()) + 3;
		if ( mSelection > last ) mSelection = last;
		if ( mFirstItem >= files.Size() ) mFirstItem = files.Size() > 7 ? files.Size() - 7 : 0;
	}

	void ConfirmDelete(bool clear)
	{
		mConfirmClear = clear;
		M_StartMessage( clear ? "Delete every managed mod?\n\nPress y or n." : "Delete the selected managed mod?\n\nPress y or n.", 0 );
	}

public:
	DManagedModsMenu() : DMenu(NULL), mFirstItem(0), mSelection(0), mConfirmClear(false) {}
	DManagedModsMenu(DMenu *parent) : DMenu(parent), mFirstItem(0), mSelection(0), mConfirmClear(false) {}

	void Drawer()
	{
		Super::Drawer();
		TArray<FManagedModFile> files;
		MODMANAGER_ListManagedMods(files);
		RefreshSelection(files);
		screen->DrawText(BigFont, CR_WHITE, 160 - BigFont->StringWidth("MANAGE MODS") / 2, 12, "MANAGE MODS", DTA_Clean, true, TAG_DONE);
		FString line;
		line.Format("Managed folder: /mods  (%u file%s)", files.Size(), files.Size() == 1 ? "" : "s");
		screen->DrawText(SmallFont, CR_GRAY, 16, 34, line, DTA_Clean, true, TAG_DONE);
		const unsigned int last = MIN<unsigned int>(mFirstItem + 7, files.Size());
		int y = 52;
		for (unsigned int i = mFirstItem; i < last; ++i, y += 14)
		{
			line.Format("%s [%s] %s", IsMarked(files[i].Name.GetChars()) ? "*" : " ", browsermenu_FormatModDownloadSize(files[i].Size).GetChars(), files[i].Name.GetChars());
			screen->DrawText(SmallFont, static_cast<int>(i) == mSelection ? CR_ORANGE : (IsMarked(files[i].Name.GetChars()) ? CR_RED : CR_WHITE), 24, y, line, DTA_Clean, true, TAG_DONE);
		}
		if (files.Size() == 0)
			screen->DrawText(SmallFont, CR_GRAY, 24, y, "No managed mods found.", DTA_Clean, true, TAG_DONE);
		if (files.Size() > 7)
		{
			line.Format("Items %u-%u of %u  Left/Right: page", mFirstItem + 1, last, files.Size());
			screen->DrawText(SmallFont, CR_GRAY, 16, 154, line, DTA_Clean, true, TAG_DONE);
		}
		const int actionY = 170;
		const int deleteAction = static_cast<int>(files.Size());
		line.Format("Delete Mod%s", mMarked.Size() ? " (selected)" : " (none selected)");
		screen->DrawText(SmallFont, mSelection == deleteAction ? CR_ORANGE : CR_WHITE, 24, actionY, line, DTA_Clean, true, TAG_DONE);
		screen->DrawText(SmallFont, mSelection == deleteAction + 1 ? CR_ORANGE : CR_WHITE, 24, actionY + 12, mMarked.Size() ? "Clear Selection" : "Clear Selection (none selected)", DTA_Clean, true, TAG_DONE);
		screen->DrawText(SmallFont, mSelection == deleteAction + 2 ? CR_ORANGE : CR_WHITE, 24, actionY + 24, files.Size() == 0 ? "Clear All Mods (unavailable)" : "Clear All Mods", DTA_Clean, true, TAG_DONE);
		screen->DrawText(SmallFont, mSelection == deleteAction + 3 ? CR_ORANGE : CR_WHITE, 24, actionY + 36, "Back", DTA_Clean, true, TAG_DONE);
	}

	bool MenuEvent(int mkey, bool fromcontroller)
	{
		TArray<FManagedModFile> files;
		MODMANAGER_ListManagedMods(files);
		RefreshSelection(files);
		const int backAction = static_cast<int>(files.Size()) + 3;
		if (mkey == MKEY_MBYes)
		{
			FString error;
			bool ok = true;
			if (mConfirmClear) ok = MODMANAGER_ClearManagedMods(error);
			else for (unsigned int i = 0; i < mMarked.Size() && ok; ++i) ok = MODMANAGER_DeleteManagedMod(mMarked[i].GetChars(), error);
			if (ok) mMarked.Clear();
			if (!ok) { if (error.IsEmpty()) error = "The mod could not be deleted."; error += "\n\nPress a key."; M_StartMessage(error.GetChars(), 1); }
			return true;
		}
		if (mkey == MKEY_Left || mkey == MKEY_Right)
		{
			const unsigned int slot = mSelection >= 0 && static_cast<unsigned int>(mSelection) >= mFirstItem ? static_cast<unsigned int>(mSelection) - mFirstItem : 0;
			if (mkey == MKEY_Left && mFirstItem >= 7) { mFirstItem -= 7; mSelection = MIN<unsigned int>(mFirstItem + slot, files.Size() - 1); }
			else if (mkey == MKEY_Right && mFirstItem + 7 < files.Size()) { mFirstItem += 7; mSelection = MIN<unsigned int>(mFirstItem + slot, files.Size() - 1); }
			else { S_Sound(CHAN_VOICE | CHAN_UI, "menu/invalid", snd_menuvolume, ATTN_NONE); return true; }
			S_Sound(CHAN_VOICE | CHAN_UI, "menu/change", snd_menuvolume, ATTN_NONE);
			return true;
		}
		if (mkey == MKEY_Up || mkey == MKEY_Down)
		{
			const int firstRow = static_cast<int>(mFirstItem);
			const int lastRow = static_cast<int>(MIN<unsigned int>(mFirstItem + 7, files.Size())) - 1;
			const int deleteAction = static_cast<int>(files.Size());
			if (mkey == MKEY_Down)
			{
				if (mSelection >= firstRow && mSelection < lastRow) ++mSelection;
				else if (mSelection >= firstRow && mSelection == lastRow) mSelection = deleteAction;
				else if (mSelection < backAction) ++mSelection;
				else mSelection = files.Size() ? firstRow : deleteAction;
			}
			else
			{
				if (mSelection > deleteAction && mSelection <= backAction) --mSelection;
				else if (mSelection == deleteAction) mSelection = lastRow >= firstRow ? lastRow : backAction;
				else if (mSelection > firstRow && mSelection <= lastRow) --mSelection;
				else mSelection = backAction;
			}
			S_Sound(CHAN_VOICE | CHAN_UI, "menu/cursor", snd_menuvolume, ATTN_NONE);
			return true;
		}
		if (mkey == MKEY_Enter)
		{
			if (mSelection == backAction) { Close(); return true; }
			if (mSelection >= 0 && mSelection < static_cast<int>(files.Size())) { ToggleMarked(files[mSelection].Name.GetChars()); return true; }
			if (mSelection == static_cast<int>(files.Size()) && mMarked.Size() > 0) { ConfirmDelete(false); return true; }
			if (mSelection == static_cast<int>(files.Size()) + 1 && mMarked.Size() > 0) { mMarked.Clear(); return true; }
			if (mSelection == static_cast<int>(files.Size()) + 2 && files.Size() > 0) { ConfirmDelete(true); return true; }
		}
		return Super::MenuEvent(mkey, fromcontroller);
	}
};

IMPLEMENT_CLASS(DManagedModsMenu)

class IModOptionActionTarget
{
public:
	virtual void ActivateModOption(int action) = 0;
};

class FModOptionAction : public FOptionMenuItem
{
	IModOptionActionTarget *mTarget;
	int mAction;
public:
	FModOptionAction(const char *label, IModOptionActionTarget *target, int action)
		: FOptionMenuItem(label), mTarget(target), mAction(action) {}
	int Draw(FOptionMenuDescriptor *desc, int y, int indent, bool selected)
	{
		drawLabel(indent, y, selected ? OptionSettings.mFontColorSelection : OptionSettings.mFontColorMore);
		return indent;
	}
	bool Activate()
	{
		mTarget->ActivateModOption(mAction);
		return true;
	}
};

class FModOptionRow : public FOptionMenuItem
{
	unsigned int mIndex;
	bool mQueue;
public:
	FModOptionRow(unsigned int index, bool queue) : FOptionMenuItem(""), mIndex(index), mQueue(queue) {}
	bool Selectable() { return false; }
	int Draw(FOptionMenuDescriptor *, int y, int indent, bool)
	{
		FModJoinRequest request;
		if (!MODMANAGER_GetCurrentRequest(request) || mIndex >= request.Requirements.Size()) return -1;
		const FModRequirement &requirement = request.Requirements[mIndex];
		const int left = indent - 80 * CleanXfac_1;
		const int nameX = left + 52 * CleanXfac_1;
		const int stateX = left + 228 * CleanXfac_1;
		const EColorRange color = browsermenu_ModDownloadColor(requirement);
		FString state;
		if (mQueue)
		{
			screen->DrawText(SmallFont, color, left + 12 * CleanXfac_1, y, browsermenu_ModProgressPrefix(requirement), DTA_CleanNoMove_1, true, TAG_DONE);
			state = browsermenu_ModDownloadState(requirement);
		}
		else
		{
			if (requirement.Status == MODREQ_Found) state = "verified";
			else if (requirement.Downloadable) state = "downloadable";
			else state = "unavailable";
		}
		screen->DrawText(SmallFont, color, nameX, y, requirement.Name, DTA_CleanNoMove_1, true, TAG_DONE);
		screen->DrawText(SmallFont, color, stateX, y, state, DTA_CleanNoMove_1, true, TAG_DONE);
		return left;
	}
};

class DModDownloadMenu : public DOptionMenu, public IModOptionActionTarget
{
	DECLARE_CLASS(DModDownloadMenu, DOptionMenu)
	FOptionMenuDescriptor mDescriptor;
	bool mJoinAfter;
	bool mRestartIssued;
public:
	DModDownloadMenu() : DOptionMenu(), mJoinAfter(false), mRestartIssued(false) {}
	DModDownloadMenu(DMenu *parent, bool joinAfter) : DOptionMenu(), mJoinAfter(joinAfter), mRestartIssued(false)
	{
		mDescriptor.Reset(); mDescriptor.mTitle = "MOD DOWNLOAD"; mDescriptor.mPosition = 0; mDescriptor.mSelectedItem = -1; mDescriptor.mScrollPos = 0; mDescriptor.mDrawTop = 0;
		FModJoinRequest request; MODMANAGER_GetCurrentRequest(request);
		for (unsigned int i = 0; i < request.Requirements.Size(); ++i) mDescriptor.mItems.Push(new FModOptionRow(i, true));
		mDescriptor.mItems.Push(new FOptionMenuItemStaticText(" ", false));
		mDescriptor.mItems.Push(new FModOptionAction("Cancel and return to server browser", this, 0));
		mDescriptor.mIndent = 80;
		DOptionMenu::Init(parent, &mDescriptor);
	}
	void ActivateModOption(int)
	{
		MODMANAGER_CancelActiveDownload();
		DMenu *requirements = mParentMenu; Close(); if (requirements != NULL && DMenu::CurrentMenu == requirements) requirements->Close();
	}
	void Ticker()
	{
		Super::Ticker(); MODMANAGER_Tick();
		if (!mJoinAfter && MODMANAGER_GetQueueState() == MODQUEUE_Complete && MODMANAGER_ShouldReturnToRequirements()) { MODMANAGER_ClearReturnFlag(); Close(); return; }
		if (mJoinAfter && !mRestartIssued && MODMANAGER_GetQueueState() == MODQUEUE_Complete) { mRestartIssued = true; M_ClearMenus(); C_DoCommand("restart"); }
	}
	bool MenuEvent(int key, bool controller)
	{
		if (key == MKEY_Back) { ActivateModOption(0); return true; }
		if (key == MKEY_Enter && MODMANAGER_GetQueueState() == MODQUEUE_Failed) { MODMANAGER_RetryFailed(); return true; }
		return Super::MenuEvent(key, controller);
	}
};
IMPLEMENT_CLASS(DModDownloadMenu)

class DModRequirementsMenu : public DOptionMenu, public IModOptionActionTarget
{
	DECLARE_CLASS(DModRequirementsMenu, DOptionMenu)
	FOptionMenuDescriptor mDescriptor;
public:
	DModRequirementsMenu() : DOptionMenu() {}
	DModRequirementsMenu(DMenu *parent) : DOptionMenu()
	{
		mDescriptor.Reset(); mDescriptor.mTitle = "SERVER REQUIREMENTS"; mDescriptor.mPosition = 0; mDescriptor.mSelectedItem = -1; mDescriptor.mScrollPos = 0; mDescriptor.mDrawTop = 0;
		FModJoinRequest request; MODMANAGER_GetCurrentRequest(request);
		for (unsigned int i = 0; i < request.Requirements.Size(); ++i) mDescriptor.mItems.Push(new FModOptionRow(i, false));
		mDescriptor.mItems.Push(new FOptionMenuItemStaticText(" ", false));
		mDescriptor.mItems.Push(new FModOptionAction("Join Now", this, 0));
		mDescriptor.mItems.Push(new FModOptionAction("Download Only", this, 1));
		mDescriptor.mItems.Push(new FModOptionAction("Download and Join", this, 2));
		mDescriptor.mItems.Push(new FModOptionAction("Back", this, 3));
		mDescriptor.mIndent = 80;
		DOptionMenu::Init(parent, &mDescriptor);
	}
	void ActivateModOption(int action)
	{
		FModJoinRequest request; MODMANAGER_GetCurrentRequest(request);
		if (action == 3) { Close(); return; }
		if (action == 0) { FString error; if (MODMANAGER_RequestCanJoin(request) && MODMANAGER_PrepareJoinRestart(error)) { M_ClearMenus(); C_DoCommand("restart"); } return; }
		if (!MODMANAGER_HasDownloadableMissing(request)) return;
		M_ActivateMenu(new DModDownloadMenu(this, action == 2));
		MODMANAGER_StartDownloads(action == 2);
	}
};
IMPLEMENT_CLASS(DModRequirementsMenu)

//*****************************************************************************
//
CCMD( querymaster )
{
	M_RefreshServers();
}

//*****************************************************************************
// [AK]
CCMD ( menu_clear_browser_filter )
{
	menu_browser_filtername = "";
}

//*****************************************************************************
//
CCMD ( menu_join_selected_server )
{
	if ( g_lSelectedServer < 0 )
	{
		M_StartMessage( "No server selected.\n\npress a key.", 1 );
		return;
	}

	FString error;
	if ( !MODMANAGER_BuildServerRequest( g_lSelectedServer, error ) )
	{
		if ( error.IsEmpty() ) error = "Unable to read server requirements.";
		error += "\n\nPress a key.";
		M_StartMessage( error.GetChars(), 1 );
		return;
	}
	M_ActivateMenu( new DLegacyModRequirementsMenu( DMenu::CurrentMenu ) );
}

CCMD ( menu_play_selected_server_offline )
{
	if ( g_lSelectedServer < 0 )
	{
		M_StartMessage( "No server selected.\n\npress a key.", 1 );
		return;
	}

	FString error;
	if ( !MODMANAGER_BuildServerRequest( g_lSelectedServer, error ) )
	{
		if ( error.IsEmpty() ) error = "Unable to read server requirements.";
		error += "\n\nPress a key.";
		M_StartMessage( error.GetChars(), 1 );
		return;
	}
	M_ActivateMenu( new DLegacyModRequirementsMenu( DMenu::CurrentMenu, true ) );
}

CCMD ( menu_manage_mods )
{
	M_ActivateMenu( new DManagedModsMenu( DMenu::CurrentMenu ) );
}
