#include "sound_manager.h"
#include "editor_util.h"   // EdBrowseForFolder

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>     // SHBrowseForFolderW

#include <string>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

static std::wstring IniPath()
{
	wchar_t buf[MAX_PATH] = L"";
	GetModuleFileNameW(nullptr, buf, MAX_PATH);
	std::wstring p = buf;
	size_t slash = p.find_last_of(L"\\/");
	return (slash == std::wstring::npos ? std::wstring() : p.substr(0, slash + 1)) + L"pob-zh.ini";
}

std::wstring GetSoundFolder()
{
	wchar_t buf[512] = L"";
	GetPrivateProfileStringW(L"PobTools", L"SoundFolder", L"", buf, 512, IniPath().c_str());
	return buf;
}

void SetSoundFolder(const std::wstring& folder)
{
	WritePrivateProfileStringW(L"PobTools", L"SoundFolder", folder.c_str(), IniPath().c_str());
}

std::wstring BrowseSoundFolder(void* owner)
{
	// Shared implementation (editor_util); only the title differs.
	return EdBrowseForFolder(L"選擇音效資料夾", GetSoundFolder(), owner);
}
