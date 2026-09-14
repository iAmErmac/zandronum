#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <android/log.h>

#include "cl_demo.h"
#include "cl_main.h"
#include "cmdlib.h"
#include "d_main.h"
#include "d_net.h"
#include "doomerrors.h"
#include "doomstat.h"
#include "errors.h"
#include "gameconfigfile.h"
#include "g_level.h"
#include "g_game.h"
#include "hardware.h"
#include "i_sound.h"
#include "i_system.h"
#include "m_argv.h"
#include "networkheaders.h"
#include "textures/textures.h"
#include "templates.h"
#include "v_text.h"
#include "x86.h"
#include "zstring.h"
#include "bitmap.h"

#include "LogWritter.h"
#include "zandronum_android_host.h"

DWORD LanguageIDs[4] = {
	MAKE_ID('e', 'n', 'u', 0), MAKE_ID('e', 'n', 'u', 0),
	MAKE_ID('e', 'n', 'u', 0), MAKE_ID('e', 'n', 'u', 0)
};

int (*I_GetTime)(bool saveMS);
int (*I_WaitForTic)(int prevtic);
void (*I_FreezeTime)(bool frozen);

DArgs *Args;
extern FILE *Logfile;
bool gameisdead;

static unsigned int BaseTime;
static unsigned int TicStart;
static int TicFrozen;
static int HasExited;

static unsigned int MonotonicMilliseconds()
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return static_cast<unsigned int>(now.tv_sec * 1000u + now.tv_nsec / 1000000u);
}

void I_Tactile(int on, int off, int total)
{
	Zandronum_AndroidHost_Tactile(on, off, total);
}

static ticcmd_t EmptyCommand;
ticcmd_t *I_BaseTiccmd()
{
	return &EmptyCommand;
}

unsigned int I_MSTime()
{
	return MonotonicMilliseconds() - BaseTime;
}

unsigned int I_FPSTime()
{
	return MonotonicMilliseconds();
}

static int I_GetTimePolled(bool saveMS)
{
	if (TicFrozen != 0)
		return TicFrozen;
	unsigned int now = MonotonicMilliseconds();
	if (saveMS)
	{
		unsigned int currentTic = ((now - BaseTime) * TICRATE) / 1000;
		TicStart = currentTic * 1000 / TICRATE + BaseTime;
	}
	return Scale(now - BaseTime, TICRATE, 1000);
}

static int I_WaitForTicPolled(int previous)
{
	int current;
	while ((current = I_GetTimePolled(false)) <= previous)
	{
		if (Zandronum_AndroidHost_IsStopping())
			return previous + 1;
		usleep(1000);
	}
	return current;
}

static void I_FreezeTimePolled(bool frozen)
{
	if (frozen)
	{
		if (TicFrozen == 0)
			TicFrozen = I_GetTimePolled(false);
	}
	else if (TicFrozen != 0)
	{
		int frozenTime = TicFrozen;
		TicFrozen = 0;
		BaseTime += (I_GetTimePolled(false) - frozenTime) * 1000 / TICRATE;
	}
}

fixed_t I_GetTimeFrac(uint32 *ms)
{
	unsigned int now = MonotonicMilliseconds();
	if (ms)
		*ms = TicStart + 1000 / TICRATE;
	if (TicStart == 0)
		return FRACUNIT;
	return clamp<fixed_t>((now - TicStart) * FRACUNIT * TICRATE / 1000, 0, FRACUNIT);
}

void I_WaitVBL(int count)
{
	usleep(1000000 * count / 70);
}

void SetLanguageIDs()
{
}

void I_Init()
{
	CheckCPUID(&CPU);
	DumpCPUInfo(&CPU);
	BaseTime = MonotonicMilliseconds();
	I_GetTime = I_GetTimePolled;
	I_WaitForTic = I_WaitForTicPolled;
	I_FreezeTime = I_FreezeTimePolled;
	atterm(Zandronum_AndroidHost_CloseAudioJni);
	atterm(I_ShutdownSound);
	I_InitSound();
}

void I_Quit()
{
	if (HasExited++)
		return;
	if (demorecording)
		G_CheckDemoStatus();
	if (CLIENTDEMO_IsRecording())
		CLIENTDEMO_FinishRecording();
}

void STACK_ARGS I_FatalError(const char *error, ...)
{
	static bool alreadyThrown;
	gameisdead = true;
	if (alreadyThrown)
		exit(-1);
	alreadyThrown = true;

	char errorText[MAX_ERRORTEXT];
	va_list args;
	va_start(args, error);
	vsnprintf(errorText, sizeof(errorText), error, args);
	va_end(args);
	__android_log_print(ANDROID_LOG_ERROR, "Zandronum", "FATAL ERROR: %s", errorText);
	LogWritter_Write(errorText);
	if (Logfile)
	{
		fprintf(Logfile, "\n**** DIED WITH FATAL ERROR:\n%s\n", errorText);
		fflush(Logfile);
	}
	if (NETWORK_GetState() == NETSTATE_CLIENT)
		CLIENT_QuitNetworkGame(nullptr);
	fprintf(stderr, "%s\n", errorText);
	exit(-1);
}

void STACK_ARGS I_Error(const char *error, ...)
{
	char errorText[MAX_ERRORTEXT];
	va_list args;
	va_start(args, error);
	vsnprintf(errorText, sizeof(errorText), error, args);
	va_end(args);
	throw CRecoverableError(errorText);
}

void I_SetIWADInfo()
{
}

void I_PrintStr(const char *text)
{
	while (*text)
	{
		if (*text == 0x1c || *text == 0x1d || *text == 0x1e || *text == 0x1f)
		{
			text += text[1] != 0 ? 2 : 1;
			continue;
		}
		fputc(*text++, stdout);
	}
	fflush(stdout);
}

int I_PickIWad(WadStuff *, int, bool, int defaultiwad)
{
	return defaultiwad;
}

bool I_WriteIniFailed()
{
	Printf("The config file %s could not be saved: %s\n", GameConfig->GetPathName(), strerror(errno));
	return false;
}

static const char *FindPattern;

static int MatchFile(const struct dirent *entry)
{
	return fnmatch(FindPattern, entry->d_name, FNM_NOESCAPE) == 0;
}

void *I_FindFirst(const char *filespec, findstate_t *fileinfo)
{
	FString directory;
	const char *slash = strrchr(filespec, '/');
	if (slash)
	{
		FindPattern = slash + 1;
		directory = FString(filespec, slash - filespec + 1);
	}
	else
	{
		FindPattern = filespec;
		directory = ".";
	}
	fileinfo->current = 0;
	fileinfo->count = scandir(directory.GetChars(), &fileinfo->namelist, MatchFile, alphasort);
	return fileinfo->count > 0 ? fileinfo : reinterpret_cast<void *>(-1);
}

int I_FindNext(void *handle, findstate_t *fileinfo)
{
	findstate_t *state = static_cast<findstate_t *>(handle);
	if (state->current < fileinfo->count)
		return ++state->current < fileinfo->count ? 0 : -1;
	return -1;
}

int I_FindClose(void *handle)
{
	findstate_t *state = static_cast<findstate_t *>(handle);
	if (handle != reinterpret_cast<void *>(-1) && state->count > 0)
	{
		for (int i = 0; i < state->count; ++i)
			free(state->namelist[i]);
		free(state->namelist);
		state->count = 0;
		state->namelist = nullptr;
	}
	return 0;
}

int I_FindAttr(findstate_t *fileinfo)
{
	struct stat info;
	if (stat(fileinfo->namelist[fileinfo->current]->d_name, &info) == 0)
		return S_ISDIR(info.st_mode) ? FA_DIREC : 0;
	return 0;
}

void I_PutInClipboard(const char *text)
{
	Zandronum_AndroidHost_SetClipboard(text);
}

FString I_GetFromClipboard(bool primary)
{
	return FString(Zandronum_AndroidHost_GetClipboard(primary).c_str());
}

unsigned int I_MakeRNGSeed()
{
	unsigned int seed = static_cast<unsigned int>(time(nullptr));
	int file = open("/dev/urandom", O_RDONLY);
	if (file >= 0)
	{
		read(file, &seed, sizeof(seed));
		close(file);
	}
	return seed;
}

bool I_SetCursor(FTexture *cursorpic)
{
	if (cursorpic == nullptr || cursorpic->UseType == FTexture::TEX_Null)
		return Zandronum_AndroidHost_SetPointerIcon(nullptr, 0, 0, 0, 0);
	if (cursorpic->GetWidth() > 32 || cursorpic->GetHeight() > 32)
		return false;
	FBitmap bitmap;
	if (!bitmap.Create(cursorpic->GetWidth(), cursorpic->GetHeight()))
		return false;
	cursorpic->CopyTrueColorPixels(&bitmap, 0, 0);
	const int hotX = clamp<int>(cursorpic->LeftOffset, 0, cursorpic->GetWidth() - 1);
	const int hotY = clamp<int>(cursorpic->TopOffset, 0, cursorpic->GetHeight() - 1);
	return Zandronum_AndroidHost_SetPointerIcon(
		reinterpret_cast<const int *>(bitmap.GetPixels()), bitmap.GetWidth(), bitmap.GetHeight(), hotX, hotY);
}
