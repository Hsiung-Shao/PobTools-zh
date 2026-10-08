// Test aid shared by the launcher (POBTOOLS_LAUNCHER_SHOT) and the tool windows
// (POBTOOLS_TOOL_SHOT): the frame just drawn, read back from the GL back buffer.
// The window stays hidden the whole time, so a screenshot never puts anything on
// anyone's screen and needs no input (see agent-data error_win_gui_test_capture).
#pragma once

#include <string>

// The back buffer as a 32-bit top-down BMP. Call between RenderDrawData and
// SwapBuffers, with the GL context current.
void WriteFramebufferBmp(const std::wstring& path, int w, int h);

// The value of an environment variable, "" when unset (or longer than MAX_PATH).
std::wstring ShotEnv(const wchar_t* name);
