#include "data_updater.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

long long DataUpdaterNowFiletime()
{
	FILETIME ft{};
	GetSystemTimeAsFileTime(&ft);
	ULARGE_INTEGER u{ ft.dwLowDateTime, ft.dwHighDateTime };
	return (long long)u.QuadPart;
}
