#include "sound_manager.h"
#include "audio_player.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>     // SHBrowseForFolderW

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <string>
#include <vector>
#include <algorithm>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

static std::wstring widen(const std::string& s)
{
	if (s.empty()) return std::wstring();
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
	std::wstring w(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
	return w;
}

static std::string narrow(const std::wstring& w)
{
	if (w.empty()) return std::string();
	int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s(n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
	return s;
}

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

std::wstring BrowseSoundFolder()
{
	BROWSEINFOW bi{};
	bi.lpszTitle = L"選擇音效資料夾";
	bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
	LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
	if (!pidl) return std::wstring();
	wchar_t path[MAX_PATH] = L"";
	std::wstring r;
	if (SHGetPathFromIDListW(pidl, path)) r = path;
	CoTaskMemFree(pidl);
	return r;
}

static void EnumFiles(const std::wstring& folder, std::vector<std::wstring>& out)
{
	out.clear();
	if (folder.empty()) return;
	static const wchar_t* exts[] = { L"*.wav", L"*.mp3", L"*.ogg", L"*.flac", L"*.m4a", L"*.aac" };
	for (const wchar_t* ext : exts) {
		std::wstring pat = folder + L"\\" + ext;
		WIN32_FIND_DATAW fd;
		HANDLE h = FindFirstFileW(pat.c_str(), &fd);
		if (h == INVALID_HANDLE_VALUE) continue;
		do {
			if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(fd.cFileName);
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}
	std::sort(out.begin(), out.end());
}

bool DrawSoundLibrary(std::string& outPickedPathUtf8, float scale)
{
	static std::wstring s_folder;
	static std::vector<std::wstring> s_files;
	static bool s_init = false;
	// Re-sync when another view (音效管理) changed the persisted folder.
	if (!s_init || s_folder != GetSoundFolder()) {
		s_folder = GetSoundFolder();
		EnumFiles(s_folder, s_files);
		s_init = true;
	}

	std::string folderU = narrow(s_folder);
	ImGui::SetNextItemWidth(340 * scale);
	if (ImGui::InputText(u8"資料夾", &folderU)) s_folder = widen(folderU);
	ImGui::SameLine();
	if (ImGui::Button(u8"瀏覽…")) {
		std::wstring f = BrowseSoundFolder();
		if (!f.empty()) { s_folder = f; SetSoundFolder(f); EnumFiles(s_folder, s_files); }
	}
	ImGui::SameLine();
	if (ImGui::Button(u8"重新整理")) { SetSoundFolder(s_folder); EnumFiles(s_folder, s_files); }
	ImGui::Separator();

	bool picked = false;
	ImGui::BeginChild("##sndlist", ImVec2(440 * scale, 260 * scale), true);
	if (s_files.empty()) {
		ImGui::TextDisabled(u8"資料夾內無音效檔（wav / mp3 / ogg / flac / m4a / aac）。");
	}
	for (size_t i = 0; i < s_files.size(); i++) {
		ImGui::PushID((int)i);
		std::wstring full = s_folder + L"\\" + s_files[i];
		if (ImGui::Button(u8"播放")) PlayAudioFile(full);
		ImGui::SameLine();
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(narrow(s_files[i]).c_str());
		ImGui::SameLine(ImGui::GetContentRegionAvail().x - 56 * scale);
		if (ImGui::SmallButton(u8"選用")) { outPickedPathUtf8 = narrow(full); picked = true; }
		ImGui::PopID();
	}
	ImGui::EndChild();
	if (ImGui::Button(u8"停止播放")) StopAudio();
	return picked;
}
