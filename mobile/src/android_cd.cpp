#include "i_cd.h"

#include "c_console.h"
#include "doomtype.h"

namespace
{
	void ReportUnsupportedCD()
	{
		static bool reported = false;
		if (!reported)
		{
			reported = true;
			Printf("Android CD audio is unsupported; use the native music backends instead.\n");
		}
	}
}

bool CD_Init() { ReportUnsupportedCD(); return false; }
bool CD_Init(int) { ReportUnsupportedCD(); return false; }
bool CD_InitID(unsigned int, int) { ReportUnsupportedCD(); return false; }
void CD_Close() { ReportUnsupportedCD(); }
void CD_Eject() { ReportUnsupportedCD(); }
bool CD_UnEject() { ReportUnsupportedCD(); return false; }
void CD_Stop() { ReportUnsupportedCD(); }
bool CD_Play(int, bool) { ReportUnsupportedCD(); return false; }
void CD_PlayNoWait(int, bool) { ReportUnsupportedCD(); }
bool CD_PlayCD(bool) { ReportUnsupportedCD(); return false; }
void CD_PlayCDNoWait(bool) { ReportUnsupportedCD(); }
void CD_Pause() { ReportUnsupportedCD(); }
bool CD_Resume() { ReportUnsupportedCD(); return false; }
ECDModes CD_GetMode() { ReportUnsupportedCD(); return CDMode_Unknown; }
bool CD_CheckTrack(int) { ReportUnsupportedCD(); return false; }
