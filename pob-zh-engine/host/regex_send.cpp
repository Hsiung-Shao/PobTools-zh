// See regex_send.h.
#include "regex_send.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <commdlg.h>
#pragma comment(lib, "comdlg32.lib")

#include <cwchar>
#include <random>

namespace RegexSend {

namespace {

constexpr unsigned long long kTicksPerSecond = 10000000ull;

unsigned long long NowTicks()
{
	FILETIME ft;
	GetSystemTimeAsFileTime(&ft);
	return (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

unsigned long long Ticks(const FILETIME& ft)
{
	return (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

std::wstring Unquote(std::wstring s)
{
	while (!s.empty() && (s.back() == L' ' || s.back() == L'\t')) s.pop_back();
	size_t b = 0;
	while (b < s.size() && (s[b] == L' ' || s[b] == L'\t')) b++;
	s.erase(0, b);
	if (s.size() >= 2 && s.front() == L'"' && s.back() == L'"') s = s.substr(1, s.size() - 2);
	return s;
}

std::wstring BaseName(const std::wstring& p)
{
	const size_t k = p.find_last_of(L"\\/");
	return k == std::wstring::npos ? p : p.substr(k + 1);
}

class WinEnv final : public Env {
public:
	std::wstring RegString(bool hklm, const std::wstring& subkey, const std::wstring& value) override
	{
		HKEY key = nullptr;
		const REGSAM sam = KEY_QUERY_VALUE | (hklm ? KEY_WOW64_64KEY : 0);
		if (RegOpenKeyExW(hklm ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, subkey.c_str(), 0, sam, &key) != ERROR_SUCCESS)
			return L"";
		std::wstring out;
		DWORD bytes = 0;
		const DWORD flags = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ;
		if (RegGetValueW(key, nullptr, value.c_str(), flags, nullptr, nullptr, &bytes) == ERROR_SUCCESS && bytes > 0) {
			std::wstring buf(bytes / sizeof(wchar_t) + 1, L'\0');
			DWORD got = static_cast<DWORD>(buf.size() * sizeof(wchar_t));
			if (RegGetValueW(key, nullptr, value.c_str(), flags, nullptr, &buf[0], &got) == ERROR_SUCCESS)
				out.assign(buf.c_str());
		}
		RegCloseKey(key);
		return out;
	}

	bool FileExists(const std::wstring& path) override
	{
		if (path.empty()) return false;
		const DWORD a = GetFileAttributesW(path.c_str());
		return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
	}

	std::wstring LocalAppData() override
	{
		wchar_t buf[32768];
		const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, 32768);
		return n > 0 && n < 32768 ? std::wstring(buf, n) : std::wstring();
	}

	std::vector<std::wstring> RunningImages(const std::wstring& imageName) override
	{
		std::vector<std::wstring> out;
		HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snap == INVALID_HANDLE_VALUE) return out;
		PROCESSENTRY32W pe{};
		pe.dwSize = sizeof(pe);
		for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
			if (_wcsicmp(pe.szExeFile, imageName.c_str()) != 0) continue;
			HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
			if (!h) continue;
			wchar_t buf[32768];
			DWORD len = 32768;
			if (QueryFullProcessImageNameW(h, 0, buf, &len)) {
				std::wstring p(buf, len);
				bool dup = false;
				for (const std::wstring& q : out) dup = dup || _wcsicmp(q.c_str(), p.c_str()) == 0;
				if (!dup) out.push_back(p);
			}
			CloseHandle(h);
		}
		CloseHandle(snap);
		return out;
	}
};

std::string WinErr(DWORD e)
{
	wchar_t* msg = nullptr;
	FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
	               e, 0, reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
	std::wstring w = msg ? msg : L"";
	if (msg) LocalFree(msg);
	while (!w.empty() && (w.back() == L'\r' || w.back() == L'\n' || w.back() == L' ' || w.back() == L'.'))
		w.pop_back();
	return Narrow(w) + u8"（錯誤碼 " + std::to_string(e) + u8"）";
}

class WinLauncher final : public Launcher {
public:
	bool Launch(const std::wstring& exe, const std::wstring& cmdline, const std::wstring& cwd, std::string* err) override
	{
		std::vector<wchar_t> buf(cmdline.begin(), cmdline.end());
		buf.push_back(L'\0');
		STARTUPINFOW si{};
		si.cb = sizeof(si);
		PROCESS_INFORMATION pi{};
		if (!CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, FALSE, 0, nullptr,
		                    cwd.empty() ? nullptr : cwd.c_str(), &si, &pi)) {
			if (err) *err = WinErr(GetLastError());
			return false;
		}
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		return true;
	}
};

} // namespace

std::string Narrow(const std::wstring& w)
{
	if (w.empty()) return {};
	const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s(n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
	return s;
}

std::wstring Widen(const std::string& s)
{
	if (s.empty()) return {};
	const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
	std::wstring w(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
	return w;
}

std::wstring PickExeDialog(void* owner, const std::wstring& initialDir)
{
	wchar_t buf[32768] = L"";
	OPENFILENAMEW ofn{};
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = owner ? (HWND)owner : GetActiveWindow();
	ofn.lpstrFilter = L"ExileAppraiser.exe\0ExileAppraiser.exe\0程式 (*.exe)\0*.exe\0\0";
	ofn.lpstrFile = buf;
	ofn.nMaxFile = 32768;
	ofn.lpstrTitle = L"指定 ExileAppraiser.exe";
	ofn.lpstrInitialDir = initialDir.empty() ? nullptr : initialDir.c_str();
	ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
	return GetOpenFileNameW(&ofn) ? std::wstring(buf) : std::wstring();
}

Env& RealEnv()
{
	static WinEnv env;
	return env;
}

Launcher& RealLauncher()
{
	static WinLauncher l;
	return l;
}

std::wstring StripIconIndex(const std::wstring& displayIcon)
{
	std::wstring s = Unquote(displayIcon);
	const size_t comma = s.find_last_of(L',');
	if (comma != std::wstring::npos && comma + 1 < s.size()) {
		size_t i = comma + 1;
		if (s[i] == L'-') i++;
		bool digits = i < s.size();
		for (size_t k = i; k < s.size(); k++) digits = digits && s[k] >= L'0' && s[k] <= L'9';
		if (digits) s = s.substr(0, comma);
	}
	return Unquote(s);
}

std::wstring JoinPath(const std::wstring& dir, const std::wstring& name)
{
	std::wstring d = Unquote(dir);
	if (d.empty()) return L"";
	if (d.back() != L'\\' && d.back() != L'/') d += L'\\';
	return d + name;
}

std::wstring DirOf(const std::wstring& path)
{
	const size_t k = path.find_last_of(L"\\/");
	if (k == std::wstring::npos) return L"";
	if (k == 2 && path.size() > 1 && path[1] == L':') return path.substr(0, 3);   // "C:\"
	return path.substr(0, k);
}

const char* SourceLabel(Source s)
{
	switch (s) {
	case Source::Manual: return u8"手動指定";
	case Source::HkcuInstall: return u8"登錄（個人安裝 InstallLocation）";
	case Source::HkcuUninstall: return u8"登錄（個人安裝 DisplayIcon）";
	case Source::HklmInstall: return u8"登錄（全機安裝 InstallLocation）";
	case Source::HklmUninstall: return u8"登錄（全機安裝 DisplayIcon）";
	case Source::LocalAppData: return u8"預設安裝路徑";
	case Source::Process: return u8"執行中的 ExileAppraiser";
	default: return u8"找不到";
	}
}

Located Locate(Env& env, const std::wstring& manual)
{
	Located r;
	const std::wstring m = Unquote(manual);
	if (!m.empty()) {
		if (env.FileExists(m)) {
			r.exe = m;
			r.source = Source::Manual;
			r.step = 5;
			return r;
		}
		r.manualMissing = true;
	}
	const std::wstring appKey = std::wstring(L"Software\\") + kAppGuid;
	const std::wstring unKey = std::wstring(L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\") + kAppGuid;
	for (int pass = 0; pass < 2; pass++) {
		const bool hklm = pass == 1;
		const std::wstring loc = env.RegString(hklm, appKey, L"InstallLocation");
		const std::wstring a = loc.empty() ? L"" : JoinPath(loc, kExeName);
		if (!a.empty() && env.FileExists(a)) {
			r.exe = a;
			r.source = hklm ? Source::HklmInstall : Source::HkcuInstall;
			r.step = hklm ? 2 : 1;
			return r;
		}
		const std::wstring icon = env.RegString(hklm, unKey, L"DisplayIcon");
		const std::wstring b = icon.empty() ? L"" : StripIconIndex(icon);
		if (!b.empty() && env.FileExists(b)) {
			r.exe = b;
			r.source = hklm ? Source::HklmUninstall : Source::HkcuUninstall;
			r.step = hklm ? 2 : 1;
			return r;
		}
	}
	const std::wstring lad = env.LocalAppData();
	if (!lad.empty()) {
		const std::wstring c = JoinPath(JoinPath(lad, L"Programs\\ExileAppraiser"), kExeName);
		if (env.FileExists(c)) {
			r.exe = c;
			r.source = Source::LocalAppData;
			r.step = 3;
			return r;
		}
	}
	for (const std::wstring& p : env.RunningImages(kExeName)) {
		if (env.FileExists(p)) {
			r.exe = p;
			r.source = Source::Process;
			r.step = 4;
			return r;
		}
	}
	return r;
}

std::wstring QuoteArg(const std::wstring& arg)
{
	if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
	std::wstring out = L"\"";
	for (size_t i = 0;; i++) {
		size_t slashes = 0;
		while (i < arg.size() && arg[i] == L'\\') {
			slashes++;
			i++;
		}
		if (i == arg.size()) {
			out.append(slashes * 2, L'\\');   // they precede the closing quote
			break;
		}
		if (arg[i] == L'"') {
			out.append(slashes * 2 + 1, L'\\');
			out += L'"';
		} else {
			out.append(slashes, L'\\');
			out += arg[i];
		}
	}
	out += L'"';
	return out;
}

std::wstring BuildCommandLine(const std::wstring& exe, const std::wstring& arg)
{
	return L"\"" + exe + L"\" " + QuoteArg(arg);
}

bool IsCodeCharset(const std::string& code)
{
	if (code.empty()) return false;
	for (char c : code) {
		const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
		if (!ok) return false;
	}
	return true;
}

std::wstring TempFileName(const std::wstring& stamp, uint32_t rnd)
{
	wchar_t hex[16];
	swprintf(hex, 16, L"%08x", rnd);
	return L"regex-share-" + stamp + L"-" + hex + L".txt";
}

bool IsTempFileName(const std::wstring& name)
{
	const std::wstring pre = L"regex-share-", suf = L".txt";
	if (name.size() <= pre.size() + suf.size()) return false;
	if (_wcsnicmp(name.c_str(), pre.c_str(), pre.size()) != 0) return false;
	if (_wcsicmp(name.c_str() + name.size() - suf.size(), suf.c_str()) != 0) return false;
	for (size_t i = pre.size(); i < name.size() - suf.size(); i++) {
		const wchar_t c = name[i];
		const bool ok = (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F') || c == L'-';
		if (!ok) return false;
	}
	return true;
}

std::wstring DefaultTempDir()
{
	wchar_t buf[MAX_PATH + 2];
	const DWORD n = GetTempPathW(MAX_PATH + 1, buf);
	if (n == 0 || n > MAX_PATH + 1) return L"";
	return JoinPath(std::wstring(buf, n), L"PobTools");
}

std::wstring NowStamp()
{
	SYSTEMTIME t;
	GetLocalTime(&t);
	wchar_t b[32];
	swprintf(b, 32, L"%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
	return b;
}

uint32_t RandomU32()
{
	std::random_device rd;
	return static_cast<uint32_t>(rd());
}

const char* FlagCode(Kind k) { return k == Kind::Bookmarks ? kFlagBookmarks : kFlagCode; }
const char* FlagFile(Kind k) { return k == Kind::Bookmarks ? kFlagBookmarksFile : kFlagFile; }

namespace {
std::wstring FlagW(const char* f) { return std::wstring(f, f + std::char_traits<char>::length(f)); }
}

bool PlanSend(const std::string& code, const std::wstring& tempDir, const std::wstring& stamp, uint32_t rnd,
              Plan& out, std::string* err, size_t maxInline, Kind kind)
{
	out = Plan{};
	const std::string what = kind == Kind::Bookmarks ? u8"書籤包" : u8"分享碼";
	if (!IsCodeCharset(code)) {
		if (err) *err = code.empty() ? u8"沒有" + what : what + u8"含有 base64url 以外的字元";
		return false;
	}
	if (code.size() <= maxInline) {
		out.arg = FlagW(FlagCode(kind)) + std::wstring(code.begin(), code.end());
		return true;
	}
	if (tempDir.empty()) {
		if (err) *err = u8"找不到暫存資料夾";
		return false;
	}
	CreateDirectoryW(tempDir.c_str(), nullptr);   // exists already: fine
	const DWORD da = GetFileAttributesW(tempDir.c_str());
	if (da == INVALID_FILE_ATTRIBUTES || !(da & FILE_ATTRIBUTE_DIRECTORY)) {
		if (err) *err = u8"無法建立暫存資料夾 " + Narrow(tempDir);
		return false;
	}
	const std::wstring path = JoinPath(tempDir, TempFileName(stamp, rnd));
	HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) {
		if (err) *err = u8"無法寫入暫存檔：" + WinErr(GetLastError());
		return false;
	}
	DWORD wrote = 0;
	const BOOL ok = WriteFile(h, code.data(), static_cast<DWORD>(code.size()), &wrote, nullptr);
	CloseHandle(h);
	if (!ok || wrote != code.size()) {
		DeleteFileW(path.c_str());
		if (err) *err = u8"寫入暫存檔失敗";
		return false;
	}
	out.tempFile = path;
	out.arg = FlagW(FlagFile(kind)) + path;
	return true;
}

int SweepTempDir(const std::wstring& dir, long long maxAgeSeconds, unsigned long long nowFileTime)
{
	if (dir.empty()) return 0;
	const DWORD da = GetFileAttributesW(dir.c_str());
	// A reparse point is not ours to walk into.
	if (da == INVALID_FILE_ATTRIBUTES || !(da & FILE_ATTRIBUTE_DIRECTORY) || (da & FILE_ATTRIBUTE_REPARSE_POINT)) return 0;
	const unsigned long long now = nowFileTime ? nowFileTime : NowTicks();
	const unsigned long long maxAge = static_cast<unsigned long long>(maxAgeSeconds) * kTicksPerSecond;
	WIN32_FIND_DATAW fd;
	HANDLE f = FindFirstFileW(JoinPath(dir, L"regex-share-*.txt").c_str(), &fd);
	if (f == INVALID_HANDLE_VALUE) return 0;
	std::vector<std::wstring> doomed;
	do {
		if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
		if (!IsTempFileName(fd.cFileName)) continue;
		const unsigned long long t = Ticks(fd.ftLastWriteTime);
		if (now > t && now - t > maxAge) doomed.push_back(JoinPath(dir, fd.cFileName));
	} while (FindNextFileW(f, &fd));
	FindClose(f);
	int n = 0;
	for (const std::wstring& p : doomed) n += DeleteFileW(p.c_str()) ? 1 : 0;
	return n;
}

int CleanupSession(std::vector<std::wstring>& files, long long minAgeSeconds, unsigned long long nowFileTime)
{
	const unsigned long long now = nowFileTime ? nowFileTime : NowTicks();
	const unsigned long long minAge = static_cast<unsigned long long>(minAgeSeconds) * kTicksPerSecond;
	int n = 0;
	std::vector<std::wstring> keep;
	for (const std::wstring& p : files) {
		WIN32_FILE_ATTRIBUTE_DATA a;
		if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) continue;   // gone already
		if (!IsTempFileName(BaseName(p)) || (a.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
			continue;   // not ours: forget it, never delete
		const unsigned long long t = Ticks(a.ftLastWriteTime);
		if (now > t && now - t >= minAge && DeleteFileW(p.c_str())) {
			n++;
			continue;
		}
		keep.push_back(p);
	}
	files.swap(keep);
	return n;
}

Result Send(const std::wstring& exe, const std::string& code, const std::wstring& tempDir, Launcher& launcher,
            const std::wstring& stamp, uint32_t rnd, size_t maxInline, Kind kind)
{
	Result r;
	Plan plan;
	std::string err;
	if (!PlanSend(code, tempDir, stamp, rnd, plan, &err, maxInline, kind)) {
		r.message = u8"沒有送出：" + err;
		return r;
	}
	const std::wstring cmd = BuildCommandLine(exe, plan.arg);
	if (!launcher.Launch(exe, cmd, DirOf(exe), &err)) {
		if (!plan.tempFile.empty()) DeleteFileW(plan.tempFile.c_str());
		r.message = u8"ExileAppraiser 啟動失敗：" + err;
		return r;
	}
	r.ok = true;
	r.tempFile = plan.tempFile;
	if (kind == Kind::Bookmarks)
		r.message = plan.tempFile.empty() ? u8"已送出，請到 ExileAppraiser 按「加入」"
		                                  : u8"已送出（書籤包較長，經暫存檔傳遞），請到 ExileAppraiser 按「加入」";
	else
		r.message = plan.tempFile.empty() ? u8"已送出，請到 ExileAppraiser 確認套用"
		                                  : u8"已送出（分享碼較長，經暫存檔傳遞），請到 ExileAppraiser 確認套用";
	return r;
}

} // namespace RegexSend
