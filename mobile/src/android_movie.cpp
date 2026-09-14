#include "i_movie.h"

#include "c_console.h"
#include "doomtype.h"

int I_PlayMovie(const char *)
{
	static bool reported = false;
	if (!reported)
	{
		reported = true;
		Printf("Android movie playback is unsupported.\n");
	}
	return MOVIE_Failed;
}
