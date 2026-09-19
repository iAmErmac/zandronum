#include <cstdlib>

#include "st_start.h"

#include "zandronum_android_host.h"

FStartupScreen *StartScreen = nullptr;

FStartupScreen *FStartupScreen::CreateInstance(int max_progress)
{
	return new FStartupScreen(max_progress);
}

void ST_Endoom()
{
	Zandronum_AndroidHost_RequestQuit();
	std::exit(0);
}
