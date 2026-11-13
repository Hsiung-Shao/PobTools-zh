#include "filter_data.h"
#include "filter_parser.h"
#include "filter_edit_journal.h"   // ParseOnlineHeader, FilterBannerValue

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>      // SHGetKnownFolderPath, FOLDERID_Documents, KF_FLAG_DEFAULT
#include "win_util.h"

#include <algorithm>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")  // CoTaskMemFree

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

// directory discovery

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

std::wstring Poe1FilterBaseDir()
{
	std::wstring docs = documents_dir();
	if (docs.empty()) return std::wstring();
	if (docs.back() != L'\\') docs += L'\\';
	const std::wstring base = docs + L"My Games\\Path of Exile\\";
	return dir_exists(base) ? base : std::wstring();
}

std::vector<OnlineFilterEntry> ListOnlineFiltersIn(const std::wstring& dir)
{
	std::vector<OnlineFilterEntry> out;
	WIN32_FIND_DATAW fd{};
	HANDLE h = FindFirstFileW((dir + L"*").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return out;
	do {
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
		const std::wstring path = dir + fd.cFileName;
		// the header and NeverSink's banner are in the first few lines
		std::string head;
		{
			HANDLE fh = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
			                        FILE_ATTRIBUTE_NORMAL, nullptr);
			if (fh == INVALID_HANDLE_VALUE) continue;
			char buf[8192];
			DWORD got = 0;
			if (ReadFile(fh, buf, sizeof(buf), &got, nullptr)) head.assign(buf, got);
			CloseHandle(fh);
		}
		const OnlineFilterHeader oh = ParseOnlineHeader(head);
		if (!oh.online) continue;
		OnlineFilterEntry e;
		e.path = path;
		e.name = oh.name.empty() ? narrow(fd.cFileName) : oh.name;
		e.filterVersion = oh.filterVersion;
		// the banner's own version, cut off at the head's last whole line
		const FilterFile f = ParseFilter(StripOnlineHeader(head.substr(0, head.find_last_of('\n') + 1)));
		e.version = FilterBannerValue(f, "VERSION");
		out.push_back(std::move(e));
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	std::sort(out.begin(), out.end(), [](const OnlineFilterEntry& a, const OnlineFilterEntry& b) { return a.name < b.name; });
	return out;
}

std::vector<OnlineFilterEntry> ListOnlineFilters()
{
	const std::wstring base = Poe1FilterBaseDir();
	return base.empty() ? std::vector<OnlineFilterEntry>() : ListOnlineFiltersIn(base + L"OnlineFilters\\");
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

// load / save

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
	// What was written becomes each edited line's raw bytes: a clean line is
	// emitted from raw, so clearing the flag alone would put the old text back
	// on the next save.
	for (FilterLine& ln : file.lines) {
		if (ln.dirty) ln.raw = FilterSerializeLine(ln);
		ln.dirty = false;
	}
	file.dirty = false;
	return true;
}
