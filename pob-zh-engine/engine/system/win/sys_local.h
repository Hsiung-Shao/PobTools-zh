// SimpleGraphic Engine
// (c) David Gowor, 2014
//
// System Local Header
// Platform: Windows
//

#include "common.h"

#include "system.h"

#ifdef _WIN32
#define _WIN32_WINNT _WIN32_WINNT_WIN7
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shlobj.h>
#include <shellapi.h>
#include <mmsystem.h>
#endif

#include <chrono>
#include <vector>

// =======
// Classes
// =======

// System Main: sys_main.cpp
class sys_main_c: public sys_IMain {
public:
	// Interface
	int		GetTime();
	void	Sleep(int msec);
	bool	IsKeyDown(byte key);
	void	ClipboardCopy(const char* str);
	char*	ClipboardPaste();
	bool	SetWorkDir(std::filesystem::path const& newCwd = {});
	void	SpawnProcess(std::filesystem::path cmdName, const char* argList);
	std::optional<std::string> OpenURL(const char* url); // return value has failure reason
	void	Error(const char* fmt, ...);
	void	Exit(const char* msg = NULL);
	void	Restart();

	// Encapsulated
	sys_main_c();

	bool	Run(int argc, char** argv);

	byte	GlfwKeyToKey(int key, int scancode);
	char	GlfwKeyExtraChar(int key);

#ifdef _WIN32
	HINSTANCE hinst = nullptr;
	HICON	icon = nullptr;
#endif

	class core_IMain* core = nullptr;

	bool	initialised = false;
	bool	minimized = false;
	volatile bool	exitFlag = false;
	volatile bool	restartFlag = false;
	char*	exitMsg = nullptr;
	char*	threadError = nullptr;
	bool	errorRaised = false;
	std::chrono::system_clock::time_point baseTime;
	std::vector<uint8_t> heldKeyState;
};
