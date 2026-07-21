#include "filter_data.h"
#include "filter_parser.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>      // SHGetKnownFolderPath, FOLDERID_Documents, KF_FLAG_DEFAULT

#include <algorithm>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")  // CoTaskMemFree

// ---- small Win32 / encoding helpers (self-contained, mirrors editor_data.cpp) ----

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

static bool dir_exists(const std::wstring& p)
{
	DWORD a = GetFileAttributesW(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool read_file_utf8(const std::wstring& path, std::string& out)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	LARGE_INTEGER size{};
	bool ok = false;
	if (GetFileSizeEx(h, &size) && size.QuadPart >= 0 && size.QuadPart < (1ll << 30)) {
		out.resize((size_t)size.QuadPart);
		DWORD read = 0;
		ok = out.empty() || (ReadFile(h, &out[0], (DWORD)out.size(), &read, nullptr) && read == out.size());
		if (!ok) out.clear();
	}
	CloseHandle(h);
	return ok;
}

static bool write_file_atomic(const std::wstring& path, const std::string& data)
{
	std::wstring tmp = path + L".tmp";
	HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	bool ok = true;
	DWORD written = 0;
	if (!data.empty())
		ok = WriteFile(h, data.data(), (DWORD)data.size(), &written, nullptr) && written == data.size();
	CloseHandle(h);
	if (!ok) { DeleteFileW(tmp.c_str()); return false; }
	if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
		DeleteFileW(tmp.c_str());
		return false;
	}
	return true;
}

static std::wstring basename_w(const std::wstring& path)
{
	size_t slash = path.find_last_of(L"\\/");
	return (slash == std::wstring::npos) ? path : path.substr(slash + 1);
}

// ---- directory discovery ---------------------------------------------------

static std::wstring documents_dir()
{
	PWSTR p = nullptr;
	std::wstring out;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &p)) && p)
		out = p;
	if (p) CoTaskMemFree(p);
	return out;
}

std::vector<std::wstring> Poe1FilterDirs()
{
	std::vector<std::wstring> dirs;
	std::wstring docs = documents_dir();
	if (docs.empty()) return dirs;
	if (docs.back() != L'\\') docs += L'\\';

	std::wstring base = docs + L"My Games\\Path of Exile\\";
	if (dir_exists(base)) dirs.push_back(base);
	std::wstring sub = base + L"ItemFilters\\";
	if (dir_exists(sub)) dirs.push_back(sub);
	return dirs;
}

std::vector<FilterListEntry> ListFilters()
{
	std::vector<FilterListEntry> out;
	std::vector<std::wstring> dirs = Poe1FilterDirs();
	for (const std::wstring& dir : dirs) {
		bool inItemFilters = dir.find(L"ItemFilters") != std::wstring::npos;
		WIN32_FIND_DATAW fd{};
		HANDLE h = FindFirstFileW((dir + L"*.filter").c_str(), &fd);
		if (h == INVALID_HANDLE_VALUE) continue;
		do {
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
			FilterListEntry e;
			e.path = dir + fd.cFileName;
			e.name = narrow(fd.cFileName);
			e.inItemFilters = inItemFilters;
			out.push_back(std::move(e));
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}
	std::sort(out.begin(), out.end(),
		[](const FilterListEntry& a, const FilterListEntry& b) { return a.name < b.name; });
	return out;
}

// ---- load / save -----------------------------------------------------------

FilterFile LoadFilter(const std::wstring& path, bool* ok)
{
	std::string content;
	if (!read_file_utf8(path, content)) {
		if (ok) *ok = false;
		return FilterFile{};
	}
	FilterFile f = ParseFilter(content);
	f.path = path;
	f.name = narrow(basename_w(path));
	if (ok) *ok = true;
	return f;
}

bool SaveFilter(FilterFile& file, std::string* err)
{
	if (file.path.empty()) {
		if (err) *err = "no path";
		return false;
	}
	std::string out = SerializeFilter(file);

	// Best-effort backup of the existing file before overwriting.
	CopyFileW(file.path.c_str(), (file.path + L".bak").c_str(), FALSE);

	if (!write_file_atomic(file.path, out)) {
		if (err) *err = file.name + ": write failed";
		return false;
	}
	for (FilterLine& ln : file.lines) ln.dirty = false;
	file.dirty = false;
	return true;
}
