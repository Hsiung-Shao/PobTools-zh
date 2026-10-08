#include "pob_protocol.h"
#include "error_log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")

namespace PobProtocol {
namespace {

// poe.ninja's real links carry the character in a query string, and a
// non-Latin character name arrives double percent-encoded (each byte becomes
// "%25XX", 5 chars): a 23-character Cyrillic name alone is ~230 characters.
const size_t kMaxUri = 512;
const wchar_t* const kSchemes[] = { L"pob", L"pob2" };

std::string narrow(const std::wstring& w)
{
	if (w.empty()) return std::string();
	int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s(n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
	return s;
}

bool id_char(wchar_t c)
{
	return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') ||
	       c == L'_' || c == L'-';
}

bool hex_char(wchar_t c)
{
	return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
}

// One query value character run: unreserved characters plus "%XX" escapes
// (kept encoded; nothing is decoded here). Advances `i`; false on anything else.
bool scan_query_value(const std::wstring& s, size_t& i)
{
	while (i < s.size() && s[i] != L'&') {
		wchar_t c = s[i];
		if (id_char(c) || c == L'.' || c == L'~' || c == L'+') {
			i++;
		} else if (c == L'%' && i + 2 < s.size() && hex_char(s[i + 1]) && hex_char(s[i + 2])) {
			i += 3;
		} else {
			return false;
		}
	}
	return true;
}

wchar_t lower_ascii(wchar_t c)
{
	return (c >= L'A' && c <= L'Z') ? (wchar_t)(c - L'A' + L'a') : c;
}

// Case-insensitive ASCII prefix test.
bool starts_with_ci(const std::wstring& s, const wchar_t* prefix)
{
	size_t n = wcslen(prefix);
	if (s.size() < n) return false;
	for (size_t i = 0; i < n; i++)
		if (lower_ascii(s[i]) != lower_ascii(prefix[i])) return false;
	return true;
}

std::wstring key_path(const std::wstring& scheme)
{
	return L"Software\\Classes\\" + scheme;
}

// The program a shell\open\command string runs: the first token, quoted or not.
std::wstring command_program(const std::wstring& cmd)
{
	size_t i = 0;
	while (i < cmd.size() && (cmd[i] == L' ' || cmd[i] == L'\t')) i++;
	if (i < cmd.size() && cmd[i] == L'"') {
		size_t end = cmd.find(L'"', i + 1);
		return end == std::wstring::npos ? cmd.substr(i + 1) : cmd.substr(i + 1, end - i - 1);
	}
	size_t end = cmd.find_first_of(L" \t", i);
	return end == std::wstring::npos ? cmd.substr(i) : cmd.substr(i, end - i);
}

// Paths compare case-insensitively and after resolving . / .. and slashes, so
// "D:\x\pob-zh.exe" and "d:/X/pob-zh.exe" are the same program.
std::wstring canonical(const std::wstring& p)
{
	if (p.empty()) return p;
	wchar_t buf[MAX_PATH * 2];
	DWORD n = GetFullPathNameW(p.c_str(), (DWORD)(sizeof(buf) / sizeof(buf[0])), buf, nullptr);
	std::wstring r = (n > 0 && n < sizeof(buf) / sizeof(buf[0])) ? std::wstring(buf, n) : p;
	for (auto& c : r) {
		if (c == L'/') c = L'\\';
		c = (wchar_t)towlower(c);
	}
	return r;
}

bool same_program(const std::wstring& a, const std::wstring& b)
{
	return !a.empty() && !b.empty() && canonical(a) == canonical(b);
}

// The default value of HKCU\Software\Classes\<scheme>\shell\open\command, or
// false when the key/value is absent.
bool read_command(const std::wstring& scheme, std::wstring* out)
{
	HKEY k = nullptr;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, (key_path(scheme) + L"\\shell\\open\\command").c_str(), 0,
	                  KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
		return false;
	DWORD type = 0, bytes = 0;
	LONG rc = RegQueryValueExW(k, nullptr, nullptr, &type, nullptr, &bytes);
	if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || bytes == 0) {
		RegCloseKey(k);
		return false;
	}
	std::vector<wchar_t> buf(bytes / sizeof(wchar_t) + 2, L'\0');
	rc = RegQueryValueExW(k, nullptr, nullptr, &type, (LPBYTE)buf.data(), &bytes);
	RegCloseKey(k);
	if (rc != ERROR_SUCCESS) return false;
	std::wstring v(buf.data());
	if (type == REG_EXPAND_SZ) {
		wchar_t exp[2048];
		DWORD n = ExpandEnvironmentStringsW(v.c_str(), exp, 2048);
		if (n > 0 && n <= 2048) v = exp;
	}
	if (out) *out = v;
	return true;
}

State query_scheme(const std::wstring& scheme, const std::wstring& exePath)
{
	std::wstring cmd;
	if (!read_command(scheme, &cmd)) {
		// A bare key with no command still belongs to someone; only "nothing
		// there at all" is None.
		HKEY k = nullptr;
		if (RegOpenKeyExW(HKEY_CURRENT_USER, key_path(scheme).c_str(), 0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
			RegCloseKey(k);
			return State::Other;
		}
		return State::None;
	}
	return same_program(command_program(cmd), exePath) ? State::Ours : State::Other;
}

bool set_string(HKEY root, const std::wstring& sub, const wchar_t* name, const std::wstring& value)
{
	HKEY k = nullptr;
	if (RegCreateKeyExW(root, sub.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &k,
	                    nullptr) != ERROR_SUCCESS)
		return false;
	LONG rc = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)value.c_str(),
	                         (DWORD)((value.size() + 1) * sizeof(wchar_t)));
	RegCloseKey(k);
	return rc == ERROR_SUCCESS;
}

bool register_scheme(const std::wstring& scheme, const std::wstring& exePath)
{
	const std::wstring base = key_path(scheme);
	return set_string(HKEY_CURRENT_USER, base, nullptr, L"URL:Path of Building") &&
	       set_string(HKEY_CURRENT_USER, base, L"URL Protocol", L"") &&
	       set_string(HKEY_CURRENT_USER, base + L"\\DefaultIcon", nullptr, L"\"" + exePath + L"\",0") &&
	       set_string(HKEY_CURRENT_USER, base + L"\\shell\\open\\command", nullptr,
	                  L"\"" + exePath + L"\" \"%1\"");
}

// Deletes the scheme only when it runs this exe. True when, afterwards, the
// scheme no longer points at this exe (deleted, absent, or someone else's).
bool unregister_scheme(const std::wstring& scheme, const std::wstring& exePath, bool* deleted)
{
	if (deleted) *deleted = false;
	if (query_scheme(scheme, exePath) != State::Ours) return true;
	LONG rc = RegDeleteTreeW(HKEY_CURRENT_USER, key_path(scheme).c_str());
	if (rc != ERROR_SUCCESS && rc != ERROR_FILE_NOT_FOUND) return false;
	// The parent key itself (RegDeleteTreeW leaves it).
	rc = RegDeleteKeyW(HKEY_CURRENT_USER, key_path(scheme).c_str());
	if (rc != ERROR_SUCCESS && rc != ERROR_FILE_NOT_FOUND) return false;
	if (deleted) *deleted = true;
	return true;
}

void notify_shell()
{
	SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

bool ends_with(const std::wstring& s, const wchar_t* suffix)
{
	size_t n = wcslen(suffix);
	return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// Full image path of the process owning `hwnd`, or "".
std::wstring window_image(HWND hwnd, DWORD* pidOut = nullptr)
{
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	if (pidOut) *pidOut = pid;
	HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!p) return L"";
	wchar_t buf[MAX_PATH * 2];
	DWORD n = (DWORD)(sizeof(buf) / sizeof(buf[0]));
	std::wstring r = QueryFullProcessImageNameW(p, 0, buf, &n) ? std::wstring(buf, n) : L"";
	CloseHandle(p);
	return r;
}

std::wstring class_of(HWND hwnd)
{
	wchar_t cls[64] = {};
	GetClassNameW(hwnd, cls, 64);
	return cls;
}

std::wstring title_of(HWND hwnd)
{
	wchar_t t[512] = {};
	GetWindowTextW(hwnd, t, 512);
	return t;
}

// Top-level windows in z-order (GetWindow GW_HWNDNEXT from the top), first
// match wins.
template <typename Pred>
HWND first_in_zorder(Pred pred)
{
	for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT)) {
		if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER)) continue;
		if (pred(h)) return h;
	}
	return nullptr;
}

} // namespace

std::wstring GameFromPobTitle(const std::wstring& title)
{
	if (ends_with(title, L"Path of Building (PoE2)")) return L"poe2";
	if (ends_with(title, L"Path of Building")) return L"poe1";
	return L"";
}

bool ParseLinkCopyData(unsigned long dwData, const void* data, unsigned long bytes, std::wstring* uri)
{
	if (dwData != kLinkCopyDataMagic || !data || bytes == 0 || (bytes % sizeof(wchar_t)) != 0) return false;
	if (bytes > (kMaxUri + 1) * sizeof(wchar_t)) return false;
	std::wstring s((const wchar_t*)data, bytes / sizeof(wchar_t));
	if (!s.empty() && s.back() == L'\0') s.pop_back();
	return ParsePobUri(s, nullptr, uri);
}

void* FindLastPobWindow(const std::wstring& game, const std::wstring& exePath)
{
	return first_in_zorder([&](HWND h) {
		return class_of(h).rfind(L"GLFW", 0) == 0 && GameFromPobTitle(title_of(h)) == game &&
		       same_program(window_image(h), exePath);
	});
}

void* FindModernUiWindow(const std::wstring& game, const std::wstring& exePath)
{
	const HANDLE want = (HANDLE)(INT_PTR)(game == L"poe2" ? 2 : 1);
	return first_in_zorder([&](HWND h) {
		return class_of(h) == L"PobToolsModernUi" && GetPropW(h, kPropGame) == want &&
		       same_program(window_image(h), exePath);
	});
}

static void bring_to_front(HWND hwnd, DWORD pid)
{
	AllowSetForegroundWindow(pid);
	if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
	SetForegroundWindow(hwnd);
}

CloseResult ClosePobWindowAndWait(void* hwndV, unsigned long graceMs)
{
	HWND hwnd = (HWND)hwndV;
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	HANDLE proc = OpenProcess(SYNCHRONIZE, FALSE, pid);
	if (!proc) return CloseResult::NoAnswer;
	bring_to_front(hwnd, pid);
	PostMessageW(hwnd, WM_CLOSE, 0, 0);
	const ULONGLONG start = GetTickCount64();
	bool pendingSeen = false;
	CloseResult r = CloseResult::NoAnswer;
	for (;;) {
		if (WaitForSingleObject(proc, 250) == WAIT_OBJECT_0) { r = CloseResult::Closed; break; }
		if (!IsWindow(hwnd)) continue; // window gone, process finishing
		if (!pendingSeen && GetPropW(hwnd, kPropExitPending)) pendingSeen = true;
		// Cancelled only counts after Pending was seen: the engine clears a stale
		// Cancelled before it raises Pending.
		if (pendingSeen && GetPropW(hwnd, kPropExitCancelled)) { r = CloseResult::Cancelled; break; }
		if (!pendingSeen && GetTickCount64() - start > graceMs) { r = CloseResult::NoAnswer; break; }
	}
	CloseHandle(proc);
	return r;
}

bool SendLinkToModernUi(void* hwndV, const std::wstring& uri)
{
	HWND hwnd = (HWND)hwndV;
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	AllowSetForegroundWindow(pid);
	COPYDATASTRUCT cds{};
	cds.dwData = kLinkCopyDataMagic;
	cds.cbData = (DWORD)((uri.size() + 1) * sizeof(wchar_t));
	cds.lpData = (void*)uri.c_str();
	DWORD_PTR res = 0;
	return SendMessageTimeoutW(hwnd, WM_COPYDATA, 0, (LPARAM)&cds, SMTO_ABORTIFHUNG, 5000, &res) && res == 1;
}

bool LooksLikePobUri(const std::wstring& arg)
{
	return starts_with_ci(arg, L"pob:") || starts_with_ci(arg, L"pob2:");
}

bool ParsePobUri(const std::wstring& uri, std::wstring* game, std::wstring* normalized)
{
	if (uri.empty() || uri.size() > kMaxUri) return false;
	size_t i = 0;
	std::wstring scheme;
	if (starts_with_ci(uri, L"pob2:")) {
		scheme = L"pob2";
		i = 5;
	} else if (starts_with_ci(uri, L"pob:")) {
		scheme = L"pob";
		i = 4;
	} else {
		return false;
	}
	while (i < uri.size() && uri[i] == L'/') i++;
	size_t siteStart = i;
	while (i < uri.size() && id_char(uri[i])) i++;
	if (i == siteStart || i >= uri.size() || uri[i] != L'/') return false;
	std::wstring site = uri.substr(siteStart, i - siteStart);
	i++; // the one slash between site and id
	// The id: one or more path segments of [A-Za-z0-9_-] ("<id>" for pobb.in,
	// "overview/code" for poe.ninja), no empty segment, no dots.
	const size_t idStart = i;
	int segments = 0;
	for (;;) {
		size_t segStart = i;
		while (i < uri.size() && id_char(uri[i])) i++;
		if (i == segStart) return false;
		if (++segments > 8) return false;
		if (i < uri.size() && uri[i] == L'/' && i + 1 < uri.size() && id_char(uri[i + 1])) {
			i++;
			continue;
		}
		break;
	}
	if (i < uri.size() && uri[i] == L'?') {
		// Query: key=value pairs joined by '&' (poe.ninja: account, name,
		// overview, type). Keys [A-Za-z0-9_-], values unreserved or %XX.
		i++;
		for (;;) {
			size_t keyStart = i;
			while (i < uri.size() && id_char(uri[i])) i++;
			if (i == keyStart || i >= uri.size() || uri[i] != L'=') return false;
			i++;
			if (!scan_query_value(uri, i)) return false;
			if (i < uri.size() && uri[i] == L'&') { i++; continue; }
			break;
		}
	} else if (i < uri.size() && uri[i] == L'/') {
		i++; // one optional trailing slash (no query)
	}
	if (i != uri.size()) return false;
	std::wstring id = uri.substr(idStart);
	if (!id.empty() && id.back() == L'/') id.pop_back();
	for (auto& c : site) c = lower_ascii(c);
	if (game) *game = scheme == L"pob2" ? L"poe2" : L"poe1";
	if (normalized) *normalized = scheme + L"://" + site + L"/" + id;
	return true;
}

bool PobUriCommandLineIsClean(const std::wstring& raw, const std::wstring& arg1)
{
	if (arg1.empty() || arg1.find(L'"') != std::wstring::npos) return false;
	// Skip the program token the way CommandLineToArgvW does: quoted up to the
	// next quote, or up to the first blank.
	size_t i = 0;
	while (i < raw.size() && (raw[i] == L' ' || raw[i] == L'\t')) i++;
	if (i < raw.size() && raw[i] == L'"') {
		size_t end = raw.find(L'"', i + 1);
		if (end == std::wstring::npos) return false;
		i = end + 1;
	} else {
		while (i < raw.size() && raw[i] != L' ' && raw[i] != L'\t') i++;
	}
	while (i < raw.size() && (raw[i] == L' ' || raw[i] == L'\t')) i++;
	size_t e = raw.size();
	while (e > i && (raw[e - 1] == L' ' || raw[e - 1] == L'\t')) e--;
	const std::wstring rest = raw.substr(i, e - i);
	return rest == arg1 || rest == L"\"" + arg1 + L"\"";
}

State QueryPobProtocol(const std::wstring& exePath)
{
	const State a = query_scheme(kSchemes[0], exePath);
	const State b = query_scheme(kSchemes[1], exePath);
	if (a == State::Ours && b == State::Ours) return State::Ours;
	if (a == State::Other || b == State::Other) return State::Other;
	return State::None;
}

bool RegisterPobProtocol(const std::wstring& exePath, std::wstring* err)
{
	for (const wchar_t* s : kSchemes) {
		if (!register_scheme(s, exePath)) {
			const DWORD e = GetLastError();
			PobLog::Error("protocol", "registering " + narrow(s) + ":// in HKCU failed, GetLastError=" +
			                              std::to_string((unsigned long)e));
			if (err) *err = std::wstring(L"HKCU\\Software\\Classes\\") + s;
			notify_shell();
			return false;
		}
	}
	notify_shell();
	return QueryPobProtocol(exePath) == State::Ours;
}

bool UnregisterPobProtocol(const std::wstring& exePath, std::wstring* err)
{
	bool ok = true, any = false;
	for (const wchar_t* s : kSchemes) {
		bool deleted = false;
		if (!unregister_scheme(s, exePath, &deleted)) {
			PobLog::Error("protocol", "removing " + narrow(s) + ":// from HKCU failed");
			if (err) *err = std::wstring(L"HKCU\\Software\\Classes\\") + s;
			ok = false;
		}
		any = any || deleted;
	}
	if (any) notify_shell();
	return ok;
}

int RunPobProtocolSelfTest(const std::wstring& exeDir)
{
	std::string report;
	int failures = 0;
	auto check = [&](std::string name, bool ok, const std::string& detail = "") {
		for (char& c : name) if ((unsigned char)c < 0x20) c = '?'; // one report line per case
		report += std::string(ok ? "PASS " : "FAIL ") + name + (detail.empty() ? "" : "  (" + detail + ")") + "\n";
		if (!ok) failures++;
	};

	// ---- URI whitelist ----
	struct Good { const wchar_t* in; const wchar_t* game; const wchar_t* norm; };
	const Good good[] = {
		{ L"pob://poeninja/AbC123", L"poe1", L"pob://poeninja/AbC123" },
		{ L"pob2://poeninja/Zx-9_q", L"poe2", L"pob2://poeninja/Zx-9_q" },
		{ L"POB://PoeNinja/AbC123", L"poe1", L"pob://poeninja/AbC123" },   // case: scheme + site lowered, id kept
		{ L"Pob2://POBBin/xyz", L"poe2", L"pob2://pobbin/xyz" },
		{ L"pob://poeninja/AbC123/", L"poe1", L"pob://poeninja/AbC123" },  // one trailing slash
		{ L"pob:poeninja/abc", L"poe1", L"pob://poeninja/abc" },           // no slashes after the colon
		{ L"pob:///poeninja/abc", L"poe1", L"pob://poeninja/abc" },        // one extra slash after the colon
		// the real poe.ninja buttons (2026-10): a path plus the character in a query
		{ L"pob://poeninja/overview/code?account=Brainwar-1546&name=BrainAllFlamed&overview=allflame&type=exp", L"poe1",
		  L"pob://poeninja/overview/code?account=Brainwar-1546&name=BrainAllFlamed&overview=allflame&type=exp" },
		{ L"pob2://PoeNinja/overview/code?account=perceptionofreality-7468&name=BABYROSHANCOM&overview=forbidden-rites", L"poe2",
		  L"pob2://poeninja/overview/code?account=perceptionofreality-7468&name=BABYROSHANCOM&overview=forbidden-rites" },
		{ L"pob://poeninja/overview/code?account=Poteitik-3151&name=%25D0%259F%25D0%259E%25D0%25A2&overview=allflame&type=exp", L"poe1",
		  L"pob://poeninja/overview/code?account=Poteitik-3151&name=%25D0%259F%25D0%259E%25D0%25A2&overview=allflame&type=exp" },
		{ L"pob://poeninja/x?a=", L"poe1", L"pob://poeninja/x?a=" },                      // empty value
	};
	for (const Good& g : good) {
		std::wstring game, norm;
		bool ok = ParsePobUri(g.in, &game, &norm);
		check("U accept " + narrow(g.in), ok && game == g.game && norm == g.norm,
		      ok ? narrow(game) + " " + narrow(norm) : "rejected");
	}
	const wchar_t* bad[] = {
		L"",
		L"pob://",
		L"pob://poeninja",
		L"pob://poeninja/",
		L"pob://poeninja//abc",           // empty segment
		L"pob://poeninja/abc//",          // two trailing slashes
		L"pob://poeninja/a\"b",           // quote: command-line injection
		L"pob://poeninja/a\" --engine \"x",
		L"pob://poeninja/a b",            // space
		L"pob://poeninja/a\tb",
		L"pob://poeninja/../x",           // traversal
		L"pob://poe.ninja/abc",           // dot in site
		L"pob://poeninja/a.b",
		L"pob://poeninja/abc/../def",     // dot segments
		L"pob://poeninja/abc/./def",
		L"pob://poeninja/abc#frag",
		L"pob://poeninja/abc?x=1#frag",
		L"pob://poeninja/a%22b",          // percent-escapes only in query values
		L"pob://poeninja/x?name=a\"b",    // quote in a query value
		L"pob://poeninja/x?name=a b",     // space in a query value
		L"pob://poeninja/x?name=%2",      // truncated escape
		L"pob://poeninja/x?name=%zz",     // non-hex escape
		L"pob://poeninja/x?name",         // key without '='
		L"pob://poeninja/x?=v",           // empty key
		L"pob://poeninja/x?a=1&",         // dangling '&'
		L"pob://poeninja/x?a=1&&b=2",
		L"pob://poeninja/x?a=1?b=2",      // second '?'
		L"pob://poeninja/x?a=1/b",        // slash inside the query
		L"pob://poeninja/x/?a=1",         // slash before the query
		L"pob://poeninja/x?a=\\b",       // backslash in a value
		L"pob://poeninja/x?a=1 --engine", // argument smuggling
		L"pob://poeninja/a/b/c/d/e/f/g/h/i", // more than 8 segments
		L"pob:\\\\poeninja\\abc",         // backslashes
		L"pob3://poeninja/abc",           // unknown scheme
		L"pobx://poeninja/abc",
		L"http://poeninja/abc",
		L"pob2//poeninja/abc",            // no colon
		L" pob://poeninja/abc",           // leading space
		L"pob://poeninja/abc ",
		L"pob://poeninja/\x4E2D\x6587",   // non-ASCII
		L"pob://poeninja/abc\r\n",
	};
	for (const wchar_t* b : bad) {
		std::wstring game = L"unset", norm = L"unset";
		bool ok = ParsePobUri(b, &game, &norm);
		check("U reject \"" + narrow(b) + "\"", !ok && game == L"unset" && norm == L"unset");
	}
	{
		std::wstring longId(kMaxUri, L'a');
		check("U reject over 512 chars", !ParsePobUri(L"pob://poeninja/" + longId, nullptr));
		std::wstring fit = L"pob://poeninja/" + std::wstring(kMaxUri - 15, L'a');
		check("U accept exactly 512 chars", fit.size() == kMaxUri && ParsePobUri(fit, nullptr));
	}
	{
		std::wstring s = L"pob://poeninja/abc";
		s[17] = L'\0'; // embedded NUL: the wstring is longer than its C string
		check("U reject embedded NUL", !ParsePobUri(s, nullptr));
	}
	// Raw command line: quote smuggling that CommandLineToArgvW would hide.
	{
		const std::wstring ok1 = L"\"D:\\P T\\pob-zh.exe\" \"pob://poeninja/x?name=ab\"";
		check("C clean: \"<exe>\" \"<uri>\"", PobUriCommandLineIsClean(ok1, L"pob://poeninja/x?name=ab"));
		check("C clean: unquoted exe and uri, trailing blank",
		      PobUriCommandLineIsClean(L"pob-zh.exe pob://poeninja/abc ", L"pob://poeninja/abc"));
		// "<exe>" "pob://poeninja/x?name=a"b" -> argv[1] = pob://poeninja/x?name=ab
		check("C refuse: a quote inside the link (argv would read name=ab)",
		      !PobUriCommandLineIsClean(L"\"D:\\x\\pob-zh.exe\" \"pob://poeninja/x?name=a\"b\"", L"pob://poeninja/x?name=ab"));
		check("C refuse: anything after the link",
		      !PobUriCommandLineIsClean(L"\"D:\\x\\pob-zh.exe\" \"pob://poeninja/a\" --engine x", L"pob://poeninja/a"));
		check("C refuse: unterminated program quote", !PobUriCommandLineIsClean(L"\"D:\\x\\pob-zh.exe pob://a/b", L"pob://a/b"));
		check("C refuse: empty argument", !PobUriCommandLineIsClean(L"pob-zh.exe \"\"", L""));
	}
	// Phase 2: which game a classic POB window title belongs to.
	check("T title poe1", GameFromPobTitle(L"Imported Build (Chieftain) - Path of Building") == L"poe1");
	check("T title poe2", GameFromPobTitle(L"Imported Build (Oracle) - Path of Building (PoE2)") == L"poe2");
	check("T title bare poe1/poe2", GameFromPobTitle(L"Path of Building") == L"poe1" &&
	                                    GameFromPobTitle(L"Path of Building (PoE2)") == L"poe2");
	check("T title others are none", GameFromPobTitle(L"PobTools").empty() && GameFromPobTitle(L"").empty() &&
	                                     GameFromPobTitle(L"Path of Building - notes.txt").empty());
	// Phase 2: the WM_COPYDATA payload a running new-interface window accepts.
	{
		const std::wstring good = L"POB://PoeNinja/overview/code?account=a-1&name=b";
		std::wstring out;
		check("D payload accepted and normalized",
		      ParseLinkCopyData(kLinkCopyDataMagic, good.c_str(), (unsigned long)((good.size() + 1) * 2), &out) &&
		          out == L"pob://poeninja/overview/code?account=a-1&name=b");
		check("D payload without trailing NUL", ParseLinkCopyData(kLinkCopyDataMagic, good.c_str(), (unsigned long)(good.size() * 2), &out));
		check("D wrong magic refused", !ParseLinkCopyData(0x1234, good.c_str(), (unsigned long)((good.size() + 1) * 2), &out));
		check("D odd byte count refused", !ParseLinkCopyData(kLinkCopyDataMagic, good.c_str(), 7, &out));
		check("D empty / null refused", !ParseLinkCopyData(kLinkCopyDataMagic, nullptr, 8, &out) &&
		                                    !ParseLinkCopyData(kLinkCopyDataMagic, good.c_str(), 0, &out));
		const std::wstring bad = L"pob://poeninja/a\"b";
		check("D invalid link refused", !ParseLinkCopyData(kLinkCopyDataMagic, bad.c_str(), (unsigned long)((bad.size() + 1) * 2), &out));
		std::wstring huge(2000, L'a');
		check("D oversized payload refused", !ParseLinkCopyData(kLinkCopyDataMagic, huge.c_str(), (unsigned long)(huge.size() * 2), &out));
		const wchar_t embedded[] = L"pob://poeninja/abc\0xyz";
		check("D embedded NUL refused", !ParseLinkCopyData(kLinkCopyDataMagic, embedded, (unsigned long)sizeof(embedded), &out));
	}
	check("U LooksLikePobUri routes pob:/pob2:/POB2:", LooksLikePobUri(L"pob://x") && LooksLikePobUri(L"POB2:x") &&
	                                                       !LooksLikePobUri(L"pobx://x") && !LooksLikePobUri(L"--engine"));

	// ---- registry round-trip on a test-only scheme ----
	const std::wstring scheme = L"pobtools-selftest";
	const std::wstring me = L"C:\\PobToolsSelfTest\\Folder\\pob-zh.exe";
	const std::wstring other = L"C:\\Program Files\\Path of Building\\Path of Building.exe";
	RegDeleteTreeW(HKEY_CURRENT_USER, key_path(scheme).c_str());
	RegDeleteKeyW(HKEY_CURRENT_USER, key_path(scheme).c_str());
	check("R0 test scheme absent at start", query_scheme(scheme, me) == State::None);
	check("R1 register writes the keys", register_scheme(scheme, me));
	{
		std::wstring cmd;
		read_command(scheme, &cmd);
		check("R2 command is \"<exe>\" \"%1\"", cmd == L"\"" + me + L"\" \"%1\"", narrow(cmd));
		HKEY k = nullptr;
		bool urlProto = false;
		if (RegOpenKeyExW(HKEY_CURRENT_USER, key_path(scheme).c_str(), 0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
			urlProto = RegQueryValueExW(k, L"URL Protocol", nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
			RegCloseKey(k);
		}
		check("R3 URL Protocol value present", urlProto);
	}
	check("R4 query says Ours", query_scheme(scheme, me) == State::Ours);
	check("R5 path compare ignores case and slashes",
	      query_scheme(scheme, L"c:/pobtoolsselftest/FOLDER/POB-ZH.EXE") == State::Ours);
	check("R6 another exe sees Other", query_scheme(scheme, other) == State::Other);
	{
		bool deleted = true;
		bool ok = unregister_scheme(scheme, other, &deleted);
		check("R7 unregister by another exe leaves it alone", ok && !deleted && query_scheme(scheme, me) == State::Ours);
	}
	{
		bool deleted = false;
		bool ok = unregister_scheme(scheme, me, &deleted);
		check("R8 unregister by the owner deletes it", ok && deleted && query_scheme(scheme, me) == State::None);
		HKEY k = nullptr;
		bool gone = RegOpenKeyExW(HKEY_CURRENT_USER, key_path(scheme).c_str(), 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS;
		if (!gone) RegCloseKey(k);
		check("R9 the scheme key itself is gone", gone);
	}
	// Someone else's registration (an official POB install): never deleted.
	set_string(HKEY_CURRENT_USER, key_path(scheme) + L"\\shell\\open\\command", nullptr, L"\"" + other + L"\" \"%1\"");
	{
		bool deleted = true;
		bool ok = unregister_scheme(scheme, me, &deleted);
		check("R10 another program's scheme survives our unregister",
		      ok && !deleted && query_scheme(scheme, other) == State::Ours);
	}
	// Unquoted command (some installers write it that way).
	set_string(HKEY_CURRENT_USER, key_path(scheme) + L"\\shell\\open\\command", nullptr, L"C:\\x\\pob.exe %1");
	check("R11 unquoted command parsed", query_scheme(scheme, L"C:\\x\\pob.exe") == State::Ours);
	RegDeleteTreeW(HKEY_CURRENT_USER, key_path(scheme).c_str());
	RegDeleteKeyW(HKEY_CURRENT_USER, key_path(scheme).c_str());
	// A bare key without a command is someone's, not "free".
	set_string(HKEY_CURRENT_USER, key_path(scheme), nullptr, L"URL:Something");
	check("R12 bare key counts as Other", query_scheme(scheme, me) == State::Other);
	RegDeleteTreeW(HKEY_CURRENT_USER, key_path(scheme).c_str());
	RegDeleteKeyW(HKEY_CURRENT_USER, key_path(scheme).c_str());
	check("R13 test scheme cleaned up", query_scheme(scheme, me) == State::None);

	report += failures ? "RESULT FAIL\n" : "RESULT PASS\n";
	{
		int pass = 0, total = 0;
		size_t pos = 0;
		while (pos < report.size()) {
			size_t nl = report.find('\n', pos);
			std::string ln = report.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
			if (ln.rfind("PASS ", 0) == 0) { pass++; total++; }
			else if (ln.rfind("FAIL ", 0) == 0) total++;
			pos = nl == std::string::npos ? report.size() : nl + 1;
		}
		report += std::to_string(pass) + "/" + std::to_string(total) + " passed\n";
	}
	CreateDirectoryW((exeDir + L"PobTools").c_str(), nullptr);
	HANDLE h = CreateFileW((exeDir + L"PobTools\\pob_protocol_selftest.txt").c_str(), GENERIC_WRITE, 0, nullptr,
	                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h != INVALID_HANDLE_VALUE) {
		DWORD w = 0;
		WriteFile(h, report.data(), (DWORD)report.size(), &w, nullptr);
		CloseHandle(h);
	}
	if (AttachConsole(ATTACH_PARENT_PROCESS)) {
		FILE* f = nullptr;
		freopen_s(&f, "CONOUT$", "w", stdout);
		printf("%s", report.c_str());
	}
	return failures ? 2 : 0;
}

} // namespace PobProtocol
