#include <locale.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cmdlib.h"
#include "c_console.h"
#include "d_main.h"
#include "doomerrors.h"
#include "errors.h"
#include "g_level.h"
#include "i_system.h"
#include "i_video.h"
#include "m_argv.h"
#include "r_state.h"
#include "version.h"
#include "w_wad.h"
#include "za_misc.h"

void I_ShutdownJoysticks();

static const int MaxTerms = 64;
static void (*TermFuncs[MaxTerms])();
static int NumTerms;

void addterm(void (*func)(), const char *)
{
	for (int i = 0; i < NumTerms; ++i)
		if (TermFuncs[i] == func)
			return;
	if (NumTerms == MaxTerms)
		I_FatalError("Too many exit functions registered.");
	TermFuncs[NumTerms++] = func;
}

void popterm()
{
	if (NumTerms > 0)
		--NumTerms;
}

void STACK_ARGS call_terms()
{
	while (NumTerms > 0)
		TermFuncs[--NumTerms]();
}

static void STACK_ARGS NewFailure()
{
	I_FatalError("Failed to allocate memory from system heap");
}

int main_android(int argc, char **argv)
{
	setlocale(LC_ALL, "C");
	Args = new DArgs(argc, argv);
	if (ZA_PrintVersion())
		return 0;
	M_AppendCommandLineFile("/sdcard/zandronum/commandline.txt");

	printf(GAMENAME " %s - native Android GLES\nCompiled on %s\n",
		GetVersionString(), __DATE__);
	std::set_new_handler(NewFailure);
	atexit(call_terms);
	atterm(I_Quit);
	progdir = "/sdcard/zandronum/";

	try
	{
		C_InitConsole(80 * 8, 25 * 8, false);
		D_DoomMain();
	}
	catch (class CDoomError &error)
	{
		I_ShutdownJoysticks();
		call_terms();
		if (error.GetMessage())
			fprintf(stderr, "%s\n", error.GetMessage());
		return -1;
	}
	catch (...)
	{
		call_terms();
		throw;
	}
	I_ShutdownJoysticks();
	call_terms();
	return 0;
}
