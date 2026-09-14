#include "i_cd.h"

bool CD_Init() { return false; }
bool CD_Init(int) { return false; }
bool CD_InitID(unsigned int, int) { return false; }
void CD_Close() {}
void CD_Eject() {}
bool CD_UnEject() { return false; }
void CD_Stop() {}
bool CD_Play(int, bool) { return false; }
void CD_PlayNoWait(int, bool) {}
bool CD_PlayCD(bool) { return false; }
void CD_PlayCDNoWait(bool) {}
void CD_Pause() {}
bool CD_Resume() { return false; }
ECDModes CD_GetMode() { return CDMode_Unknown; }
bool CD_CheckTrack(int) { return false; }
