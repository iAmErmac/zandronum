//-----------------------------------------------------------------------------
//
// Native server content management.
//
//-----------------------------------------------------------------------------

#ifndef __MOD_MANAGER_H__
#define __MOD_MANAGER_H__

#include <stdint.h>

#include "network.h"

enum EModRequirementStatus
{
	MODREQ_Found,
	MODREQ_Missing,
	MODREQ_Incompatible,
	MODREQ_Unknown
};

enum EModDownloadState
{
	MODDL_NotNeeded,
	MODDL_Queued,
	MODDL_Downloading,
	MODDL_Verifying,
	MODDL_Downloaded,
	MODDL_Failed,
	MODDL_Cancelled
};

enum EModQueueState
{
	MODQUEUE_Idle,
	MODQUEUE_Downloading,
	MODQUEUE_Verifying,
	MODQUEUE_Complete,
	MODQUEUE_Failed,
	MODQUEUE_Cancelled
};

struct FModRequirement
{
	FString Name;
	FString ExpectedMD5;
	FString SourceURL;
	FString SourceName;
	TArray<FString> SourceURLs;
	TArray<FString> SourceNames;
	TArray<int> SourceModes;
	FString ResolvedPath;
	FString Error;
	EModRequirementStatus Status;
	EModDownloadState DownloadState;
	uint64_t DownloadedBytes;
	uint64_t DownloadTotal;
	bool Optional;
	bool Downloadable;
};

struct FModJoinRequest
{
	NETADDRESS_s Address;
	FString IWADName;
	FString HostName;
	FString ServerURL;
	bool ServerCompatible;
	TArray<FModRequirement> Requirements;
};

typedef void (*MODMANAGER_ProgressCallback)(uint64_t received, uint64_t total);

struct FManagedModFile
{
	FString Name;
	uint64_t Size;
};

void MODMANAGER_Initialize();
void MODMANAGER_Shutdown();
void MODMANAGER_Tick();

FString MODMANAGER_GetDirectory(bool create);
const char *MODMANAGER_FindManagedFile(const char *name);
bool MODMANAGER_IsSafeBasename(const char *name);
void MODMANAGER_ListManagedMods(TArray<FManagedModFile> &files);
bool MODMANAGER_DeleteManagedMod(const char *name, FString &error);
bool MODMANAGER_ClearManagedMods(FString &error);

bool MODMANAGER_BuildServerRequest(ULONG server, FString &error);
bool MODMANAGER_GetCurrentRequest(FModJoinRequest &request);
bool MODMANAGER_RequestCanJoin(const FModJoinRequest &request);
bool MODMANAGER_RequestCanPlayOffline(const FModJoinRequest &request);
bool MODMANAGER_HasDownloadableMissing(const FModJoinRequest &request);
bool MODMANAGER_PrepareJoinRestart(FString &error);
bool MODMANAGER_PrepareOfflineRestart(FString &error);
bool MODMANAGER_TakePendingRestart(FString &address, FString &iwad, TArray<FString> &files, bool &connect);

void MODMANAGER_StartDownloads(bool joinAfter);
void MODMANAGER_StartOfflineDownloads();
void MODMANAGER_CancelDownloads(bool returnToRequirements);
void MODMANAGER_RetryFailed();
void MODMANAGER_CancelActiveDownload();
void MODMANAGER_ReportPlatformProgress(uint64_t received, uint64_t total);

EModQueueState MODMANAGER_GetQueueState();
void MODMANAGER_GetQueueInfo(FString &name, uint64_t &received, uint64_t &total, FString &error);
bool MODMANAGER_ShouldReturnToRequirements();
void MODMANAGER_ClearReturnFlag();

#endif
