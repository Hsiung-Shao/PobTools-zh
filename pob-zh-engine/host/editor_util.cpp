#include "editor_util.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>     // GetOpenFileNameW / GetSaveFileNameW

#pragma comment(lib, "comdlg32.lib")

std::string EdNarrow(const std::wstring& w)
{
	if (w.empty()) return std::string();
	int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s(n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
	return s;
}

std::wstring EdWiden(const std::string& s)
{
	if (s.empty()) return std::wstring();
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
	std::wstring w(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
	return w;
}

std::vector<unsigned char> EdReadFile(const std::wstring& path)
{
	std::vector<unsigned char> data;
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) return data;
	LARGE_INTEGER size{};
	if (GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart < (1ll << 30)) {
		data.resize((size_t)size.QuadPart);
		DWORD read = 0;
		if (!ReadFile(h, data.data(), (DWORD)data.size(), &read, nullptr) || read != data.size())
			data.clear();
	}
	CloseHandle(h);
	return data;
}

bool EdContainsCI(const std::string& hay, const std::string& needleLower)
{
	if (needleLower.empty()) return true;
	const size_t n = needleLower.size();
	if (hay.size() < n) return false;
	for (size_t i = 0; i + n <= hay.size(); i++) {
		size_t j = 0;
		for (; j < n; j++) {
			char c = hay[i + j];
			if (c >= 'A' && c <= 'Z') c += 32;
			if (c != needleLower[j]) break;
		}
		if (j == n) return true;
	}
	return false;
}

std::string EdToLowerAscii(const std::string& s)
{
	std::string r = s;
	for (char& c : r) if (c >= 'A' && c <= 'Z') c += 32;
	return r;
}

std::wstring EdFilterDialog(const std::wstring& initialDir, bool save)
{
	wchar_t buf[MAX_PATH] = L"";
	OPENFILENAMEW ofn{};
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = GetActiveWindow(); // the GLFW window on this thread
	ofn.lpstrFilter = L"過濾器 (*.filter)\0*.filter\0所有檔案 (*.*)\0*.*\0\0";
	ofn.lpstrFile = buf;
	ofn.nMaxFile = MAX_PATH;
	ofn.lpstrDefExt = L"filter";
	ofn.lpstrInitialDir = initialDir.empty() ? nullptr : initialDir.c_str();
	ofn.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
	BOOL ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
	return ok ? std::wstring(buf) : std::wstring();
}

std::string BlockSummary(const FilterFile& f, const FilterBlock& b)
{
	std::string s;
	int shown = 0;
	for (int li : b.lineIdx) {
		const FilterLine& ln = f.lines[li];
		if (ln.kind != FilterLineKind::Condition) continue;
		if (!s.empty()) s += u8"  ·  ";
		s += ln.keyword;
		if (!ln.op.empty()) { s += ' '; s += ln.op; }
		for (const FilterToken& v : ln.values) {
			s += ' ';
			if (v.quoted) { s += '"'; s += v.text; s += '"'; }
			else s += v.text;
		}
		if (++shown >= 4) { s += u8" …"; break; }
	}
	if (s.empty()) s = u8"（無條件）";
	return s;
}
