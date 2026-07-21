// DyLua: SimpleGraphic
// (c) David Gowor, 2014
//
// Entry Point
// Platform: Windows
//

#include "system/win/sys_local.h"
#if 0
#pragma comment(lib, "winmm")
#pragma comment(lib, "opengl32")
#include "_lib/lua.h"
#if LUA_VERSION_NUM >= 503
#if defined(_WIN64) && defined(_DEBUG)
#pragma comment(lib, "_lib\\lua53_64MT_D")
#elif defined(_WIN64)
#pragma comment(lib, "_lib\\lua53_64MT")
#elif defined(_DEBUG)
#pragma comment(lib, "_lib\\lua53_32MT_D")
#else
#pragma comment(lib, "_lib\\lua53_32MT")
#endif
#else
#if defined(_WIN64) 
#pragma comment(lib, "_lib\\lua51_64")
#else
#pragma comment(lib, "_lib\\lua51_32")
#endif
#endif
#if defined(_WIN64) && defined(_DEBUG)
#pragma comment(lib, "_lib\\zlib64MT_D")
#pragma comment(lib, "_lib\\jpeg64MT_D")
#pragma comment(lib, "_lib\\png64MT_D")
#pragma comment(lib, "_lib\\gif64MT_D")
#elif defined(_WIN64)
#pragma comment(lib, "_lib\\zlib64MT")
#pragma comment(lib, "_lib\\jpeg64MT")
#pragma comment(lib, "_lib\\png64MT")
#pragma comment(lib, "_lib\\gif64MT")
#elif defined(_DEBUG)
#pragma comment(lib, "_lib\\zlib32MT_D")
#pragma comment(lib, "_lib\\jpeg32MT_D")
#pragma comment(lib, "_lib\\png32MT_D")
#pragma comment(lib, "_lib\\gif32MT_D")
#else
#pragma comment(lib, "_lib\\zlib32MT")
#pragma comment(lib, "_lib\\jpeg32MT")
#pragma comment(lib, "_lib\\png32MT")
#pragma comment(lib, "_lib\\gif32MT")
#endif
#endif

#if defined _WIN32 || defined __CYGWIN__
  #ifdef SIMPLEGRAPHIC_EXPORTS
    #ifdef __GNUC__
      #define SIMPLEGRAPHIC_DLL_PUBLIC __attribute__ ((dllexport))
    #else
      #define SIMPLEGRAPHIC_DLL_PUBLIC __declspec(dllexport) // Note: actually gcc seems to also supports this syntax.
    #endif
  #else
    #ifdef __GNUC__
      #define SIMPLEGRAPHIC_DLL_PUBLIC __attribute__ ((dllimport))
    #else
      #define SIMPLEGRAPHIC_DLL_PUBLIC __declspec(dllimport) // Note: actually gcc seems to also supports this syntax.
    #endif
  #endif
  #define SIMPLEGRAPHIC_DLL_LOCAL
#else
  #if __GNUC__ >= 4
    #define SIMPLEGRAPHIC_DLL_PUBLIC __attribute__ ((visibility ("default")))
    #define SIMPLEGRAPHIC_DLL_LOCAL  __attribute__ ((visibility ("hidden")))
  #else
    #define SIMPLEGRAPHIC_DLL_PUBLIC
    #define SIMPLEGRAPHIC_DLL_LOCAL
  #endif
#endif

extern "C" SIMPLEGRAPHIC_DLL_PUBLIC int RunLuaFileAsWin(int argc, char** argv)
{
#ifdef _MEMTRAK_H
	strcpy_s(_memTrak_reportName, 512, "SimpleGraphic/memtrak.log");
#endif

#ifdef _WIN32
	timeBeginPeriod(1);
#endif

	sys_main_c* sys = new sys_main_c;

	while (sys->Run(argc, argv));

	delete sys;

#ifdef _WIN32
	timeEndPeriod(1);
#endif
	return 0;
}
