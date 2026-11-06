#include "pob_sandbox.h"

#include "pob_launch.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>

#include <cstring>
#include <vector>

namespace PobSandbox {

bool IsDir(const std::wstring& p)
{
	DWORD a = GetFileAttributesW(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool IsReparse(const std::wstring& p)
{
	DWORD a = GetFileAttributesW(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
}

void Rmtree(const std::wstring& dir)
{
	if (!IsDir(dir)) return;
	if (IsReparse(dir)) { RemoveDirectoryW(dir.c_str()); return; }
	WIN32_FIND_DATAW fd{};
	HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			const std::wstring n = fd.cFileName;
			if (n == L"." || n == L"..") continue;
			const std::wstring p = dir + L"\\" + n;
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
				Rmtree(p);
			} else {
				SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
				DeleteFileW(p.c_str());
			}
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}
	RemoveDirectoryW(dir.c_str());
}

bool MkdirP(const std::wstring& dir)
{
	// Each level explicitly: CreateDirectoryW does not create parents.
	for (size_t i = 3; i < dir.size(); i++) {
		if (dir[i] == L'\\') CreateDirectoryW(dir.substr(0, i).c_str(), nullptr);
	}
	CreateDirectoryW(dir.c_str(), nullptr);
	return IsDir(dir);
}

bool MakeJunction(const std::wstring& link, const std::wstring& target)
{
	if (!CreateDirectoryW(link.c_str(), nullptr)) return false;
	HANDLE h = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
	                       FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	std::wstring subst = L"\\??\\" + target;
	if (subst.back() == L'\\') subst.pop_back();
	const std::wstring print = target.back() == L'\\' ? target.substr(0, target.size() - 1) : target;
	// REPARSE_DATA_BUFFER's MountPointReparseBuffer, laid out by hand: the
	// struct lives in ntifs.h, which user-mode code does not get to include.
	const size_t substBytes = subst.size() * sizeof(wchar_t);
	const size_t printBytes = print.size() * sizeof(wchar_t);
	const size_t pathBytes = substBytes + sizeof(wchar_t) + printBytes + sizeof(wchar_t);
	std::vector<char> buf(8 + 8 + pathBytes, 0);
	char* p = buf.data();
	*(DWORD*)(p + 0) = IO_REPARSE_TAG_MOUNT_POINT;
	*(WORD*)(p + 4) = (WORD)(8 + pathBytes); // ReparseDataLength
	*(WORD*)(p + 8) = 0;                     // SubstituteNameOffset
	*(WORD*)(p + 10) = (WORD)substBytes;     // SubstituteNameLength
	*(WORD*)(p + 12) = (WORD)(substBytes + sizeof(wchar_t)); // PrintNameOffset
	*(WORD*)(p + 14) = (WORD)printBytes;     // PrintNameLength
	memcpy(p + 16, subst.data(), substBytes);
	memcpy(p + 16 + substBytes + sizeof(wchar_t), print.data(), printBytes);
	DWORD ret = 0;
	BOOL ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, buf.data(), (DWORD)buf.size(), nullptr, 0, &ret, nullptr);
	CloseHandle(h);
	if (!ok) RemoveDirectoryW(link.c_str());
	return ok != 0;
}

namespace {

bool ShouldJunction(const std::wstring& rel)
{
	return _wcsicmp(rel.c_str(), L"TreeData") == 0 ||
	       _wcsicmp(rel.c_str(), L"Data\\TimelessJewelData") == 0;
}

bool ShouldSkip(const std::wstring& rel, bool includeBuilds)
{
	// State the caller owns or that a previous POB run left behind.
	// installed.cfg: POB then keeps settings and builds under Documents\Path of
	// Building\ (Launch.lua installedMode) -- the sandbox would write the user's.
	// PobTools: pob-zh.exe may sit inside the POB folder (DetectInstalls case 3),
	// and the sandbox lives under <exeDir>PobTools -- never copy it into itself.
	return (!includeBuilds && _wcsicmp(rel.c_str(), L"Builds") == 0) ||
	       _wcsicmp(rel.c_str(), L"installed.cfg") == 0 ||
	       _wcsicmp(rel.c_str(), L"PobTools") == 0 ||
	       _wcsicmp(rel.c_str(), L"Update") == 0 ||
	       _wcsicmp(rel.c_str(), L"Settings.xml") == 0 ||
	       _wcsicmp(rel.c_str(), L"imgui.ini") == 0;
}

void CopyRel(const std::wstring& src, const std::wstring& dst, const std::wstring& rel, CopyStats& st, bool includeBuilds)
{
	MkdirP(dst);
	st.dirs++;
	WIN32_FIND_DATAW fd{};
	HANDLE h = FindFirstFileW((src + L"\\*").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return;
	do {
		const std::wstring n = fd.cFileName;
		if (n == L"." || n == L"..") continue;
		const std::wstring r = rel.empty() ? n : rel + L"\\" + n;
		if (ShouldSkip(r, includeBuilds)) continue;
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			if (ShouldJunction(r)) {
				// Wine has no mount-point reparse points (FSCTL_SET_REPARSE_POINT
				// fails there): copy the folder instead -- slower, same content.
				if (MakeJunction(dst + L"\\" + n, src + L"\\" + n)) st.junctions++;
				else if (PobLaunch::RunningUnderWine()) { RemoveDirectoryW((dst + L"\\" + n).c_str()); CopyRel(src + L"\\" + n, dst + L"\\" + n, r, st, includeBuilds); }
				else st.errors++;
			} else if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
				CopyRel(src + L"\\" + n, dst + L"\\" + n, r, st, includeBuilds);
			}
		} else {
			if (CopyFileW((src + L"\\" + n).c_str(), (dst + L"\\" + n).c_str(), FALSE)) st.files++; else st.errors++;
		}
	} while (FindNextFileW(h, &fd));
	FindClose(h);
}

} // namespace

void CopyTree(const std::wstring& src, const std::wstring& dst, CopyStats& st, bool includeBuilds)
{
	CopyRel(src, dst, L"", st, includeBuilds);
}

void LinkTree(const std::wstring& src, const std::wstring& dst, CopyStats& st)
{
	MkdirP(dst);
	st.dirs++;
	WIN32_FIND_DATAW fd{};
	HANDLE h = FindFirstFileW((src + L"\\*").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) { st.errors++; return; }
	do {
		const std::wstring n = fd.cFileName;
		if (n == L"." || n == L"..") continue;
		const std::wstring s = src + L"\\" + n, d = dst + L"\\" + n;
		if (_wcsicmp(n.c_str(), L"Builds") == 0) {
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) CopyRel(s, d, n, st, true);
			continue;
		}
		if (ShouldSkip(n, true)) continue;
		// POB's own exe and DLLs: the headless engine loads PobTools' engine\
		// copies (SetDllDirectoryW + cpath), and they are most of the bytes.
		const size_t dot = n.rfind(L'.');
		const std::wstring ext = dot == std::wstring::npos ? L"" : n.substr(dot);
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
		    (_wcsicmp(ext.c_str(), L".dll") == 0 || _wcsicmp(ext.c_str(), L".exe") == 0)) continue;
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			if (MakeJunction(d, s)) st.junctions++;
			else if (PobLaunch::RunningUnderWine()) { RemoveDirectoryW(d.c_str()); CopyRel(s, d, n, st, true); }
			else st.errors++;
		} else {
			if (CopyFileW(s.c_str(), d.c_str(), FALSE)) st.files++; else st.errors++;
		}
	} while (FindNextFileW(h, &fd));
	FindClose(h);
}

} // namespace PobSandbox
