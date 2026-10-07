// --regex-selftest, "送到 ExileAppraiser" part (regex_send.h): the exe locator
// over a fake registry / file system / process list, command-line quoting
// checked against CommandLineToArgvW, the inline vs temp-file forms, temp-file
// content and both clean-up rules, and the manual pick in regex_ui.json.
// Nothing here starts a process: Send() runs against a recording fake.
// The last line only REPORTS what the real locator finds on this machine
// (read-only registry / file / process queries; not a check).
#include "regex_send.h"
#include "regex_state.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <map>
#include <set>
#include <string>
#include <vector>

#include <json.hpp>

namespace {

namespace RS = RegexSend;

void (*g_check)(bool, const std::string&) = nullptr;
void (*g_line)(const std::string&) = nullptr;
void check(bool ok, const std::string& what) { g_check(ok, what); }
void line(const std::string& s) { g_line(s); }

struct FakeEnv final : RS::Env {
	std::map<std::wstring, std::wstring> reg;   // "HKCU|subkey|value" -> data
	std::set<std::wstring> files;
	std::wstring lad;
	std::vector<std::wstring> procs;
	std::vector<std::wstring> asked;            // registry lookups, in order

	static std::wstring K(bool hklm, const std::wstring& sub, const std::wstring& v)
	{
		return std::wstring(hklm ? L"HKLM|" : L"HKCU|") + sub + L"|" + v;
	}
	std::wstring RegString(bool hklm, const std::wstring& sub, const std::wstring& v) override
	{
		asked.push_back(K(hklm, sub, v));
		auto it = reg.find(K(hklm, sub, v));
		return it == reg.end() ? L"" : it->second;
	}
	bool FileExists(const std::wstring& p) override { return files.count(p) > 0; }
	std::wstring LocalAppData() override { return lad; }
	std::vector<std::wstring> RunningImages(const std::wstring&) override { return procs; }
};

const std::wstring kApp = std::wstring(L"Software\\") + RS::kAppGuid;
const std::wstring kUn = std::wstring(L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\") + RS::kAppGuid;

struct FakeLauncher final : RS::Launcher {
	int calls = 0;
	bool fail = false;
	std::wstring exe, cmd, cwd;
	bool fileExistedAtLaunch = false;
	std::wstring watchFile;
	bool Launch(const std::wstring& e, const std::wstring& c, const std::wstring& d, std::string* err) override
	{
		calls++;
		exe = e;
		cmd = c;
		cwd = d;
		fileExistedAtLaunch = !watchFile.empty() && GetFileAttributesW(watchFile.c_str()) != INVALID_FILE_ATTRIBUTES;
		if (fail && err) *err = u8"假的失敗";
		return !fail;
	}
};

std::vector<std::wstring> Argv(const std::wstring& cmd)
{
	std::vector<std::wstring> out;
	int n = 0;
	LPWSTR* a = CommandLineToArgvW(cmd.c_str(), &n);
	for (int i = 0; a && i < n; i++) out.push_back(a[i]);
	if (a) LocalFree(a);
	return out;
}

std::string ReadBytes(const std::wstring& p)
{
	std::string s;
	HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) return "<missing>";
	char buf[65536];
	DWORD got = 0;
	while (ReadFile(h, buf, sizeof buf, &got, nullptr) && got) s.append(buf, got);
	CloseHandle(h);
	return s;
}

bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

void Touch(const std::wstring& p, long long ageSeconds)
{
	HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return;
	DWORD w = 0;
	WriteFile(h, "x", 1, &w, nullptr);
	FILETIME now;
	GetSystemTimeAsFileTime(&now);
	unsigned long long t = (static_cast<unsigned long long>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
	t -= static_cast<unsigned long long>(ageSeconds) * 10000000ull;
	FILETIME ft{static_cast<DWORD>(t & 0xffffffffu), static_cast<DWORD>(t >> 32)};
	SetFileTime(h, nullptr, nullptr, &ft);
	CloseHandle(h);
}

void LocatorTests()
{
	line("[send-locate] ExileAppraiser.exe locator (fake registry / files / processes)");
	const std::wstring cu = L"C:\\Users\\me\\AppData\\Local\\Programs\\ExileAppraiser";
	const std::wstring cuExe = cu + L"\\ExileAppraiser.exe";
	{
		FakeEnv e;
		const RS::Located r = RS::Locate(e, L"");
		check(r.exe.empty() && r.source == RS::Source::None && r.step == 0 && !r.manualMissing, "nothing anywhere: not found");
		const std::vector<std::wstring> want = {FakeEnv::K(false, kApp, L"InstallLocation"), FakeEnv::K(false, kUn, L"DisplayIcon"),
		                                        FakeEnv::K(true, kApp, L"InstallLocation"), FakeEnv::K(true, kUn, L"DisplayIcon")};
		check(e.asked == want, "registry order: HKCU InstallLocation, HKCU DisplayIcon, HKLM InstallLocation, HKLM DisplayIcon");
	}
	{
		FakeEnv e;
		e.reg[FakeEnv::K(false, kApp, L"InstallLocation")] = cu + L"\\";   // trailing separator
		e.files.insert(cuExe);
		e.reg[FakeEnv::K(true, kApp, L"InstallLocation")] = L"C:\\Program Files\\ExileAppraiser";
		e.files.insert(L"C:\\Program Files\\ExileAppraiser\\ExileAppraiser.exe");
		const RS::Located r = RS::Locate(e, L"");
		check(r.exe == cuExe && r.source == RS::Source::HkcuInstall && r.step == 1,
		      "step 1: HKCU InstallLocation + \\ExileAppraiser.exe (no doubled separator), wins over HKLM");
		check(e.asked.size() == 1, "and stops at the first hit");
	}
	{
		FakeEnv e;
		e.reg[FakeEnv::K(false, kApp, L"InstallLocation")] = L"D:\\gone";   // file not there
		e.reg[FakeEnv::K(false, kUn, L"DisplayIcon")] = cuExe + L",0";
		e.files.insert(cuExe);
		const RS::Located r = RS::Locate(e, L"");
		check(r.exe == cuExe && r.source == RS::Source::HkcuUninstall && r.step == 1,
		      "step 1 fallback: InstallLocation's exe missing -> HKCU DisplayIcon minus \",0\"");
	}
	{
		FakeEnv e;
		e.reg[FakeEnv::K(true, kApp, L"InstallLocation")] = L"\"C:\\Program Files\\ExileAppraiser\"";
		e.files.insert(L"C:\\Program Files\\ExileAppraiser\\ExileAppraiser.exe");
		const RS::Located r = RS::Locate(e, L"");
		check(r.exe == L"C:\\Program Files\\ExileAppraiser\\ExileAppraiser.exe" && r.source == RS::Source::HklmInstall &&
		          r.step == 2,
		      "step 2: HKLM (64-bit view) InstallLocation, surrounding quotes dropped");
	}
	{
		FakeEnv e;
		e.reg[FakeEnv::K(true, kUn, L"DisplayIcon")] = L"\"C:\\Program Files\\ExileAppraiser\\ExileAppraiser.exe\",0";
		e.files.insert(L"C:\\Program Files\\ExileAppraiser\\ExileAppraiser.exe");
		const RS::Located r = RS::Locate(e, L"");
		check(r.source == RS::Source::HklmUninstall && r.step == 2 &&
		          r.exe == L"C:\\Program Files\\ExileAppraiser\\ExileAppraiser.exe",
		      "step 2 fallback: HKLM DisplayIcon, quoted, \",0\" dropped");
	}
	{
		FakeEnv e;
		e.reg[FakeEnv::K(false, kUn, L"DisplayIcon")] = L"C:\\nowhere\\ExileAppraiser.exe,0";   // stale key
		e.lad = L"C:\\Users\\王小明\\AppData\\Local";
		e.files.insert(L"C:\\Users\\王小明\\AppData\\Local\\Programs\\ExileAppraiser\\ExileAppraiser.exe");
		const RS::Located r = RS::Locate(e, L"");
		check(r.source == RS::Source::LocalAppData && r.step == 3 &&
		          r.exe == L"C:\\Users\\王小明\\AppData\\Local\\Programs\\ExileAppraiser\\ExileAppraiser.exe",
		      "step 3: stale registry -> %LOCALAPPDATA%\\Programs\\ExileAppraiser (Chinese user name)");
	}
	{
		FakeEnv e;
		e.lad = L"C:\\Users\\me\\AppData\\Local";   // nothing there
		e.procs = {L"C:\\Temp\\vanished\\ExileAppraiser.exe", L"C:\\Users\\me\\AppData\\Local\\Temp\\2abc\\ExileAppraiser.exe"};
		e.files.insert(e.procs[1]);
		const RS::Located r = RS::Locate(e, L"");
		check(r.source == RS::Source::Process && r.step == 4 && r.exe == e.procs[1],
		      "step 4: a running ExileAppraiser.exe (portable, temp folder); an image path that is gone is skipped");
	}
	{
		FakeEnv e;
		e.reg[FakeEnv::K(false, kApp, L"InstallLocation")] = cu;
		e.files.insert(cuExe);
		const std::wstring manual = L"E:\\可攜 版\\ExileAppraiser.exe";
		e.files.insert(manual);
		RS::Located r = RS::Locate(e, manual);
		check(r.source == RS::Source::Manual && r.step == 5 && r.exe == manual && e.asked.empty(),
		      "a manual pick whose file exists wins over every automatic step (no registry read)");
		e.files.erase(manual);
		r = RS::Locate(e, manual);
		check(r.source == RS::Source::HkcuInstall && r.exe == cuExe && r.manualMissing,
		      "a manual pick whose file is gone falls back to 1-4 and says so (manualMissing)");
		e.files.clear();
		r = RS::Locate(e, manual);
		check(r.exe.empty() && r.manualMissing && r.source == RS::Source::None, "gone and nothing else: not found, manualMissing");
	}

	check(RS::StripIconIndex(L"C:\\x\\ExileAppraiser.exe,0") == L"C:\\x\\ExileAppraiser.exe", "DisplayIcon: \",0\" dropped");
	check(RS::StripIconIndex(L"C:\\x\\ExileAppraiser.exe,-101") == L"C:\\x\\ExileAppraiser.exe", "DisplayIcon: \",-101\" dropped");
	check(RS::StripIconIndex(L"C:\\x\\ExileAppraiser.exe") == L"C:\\x\\ExileAppraiser.exe", "DisplayIcon without an index: unchanged");
	check(RS::StripIconIndex(L"C:\\a,b\\ExileAppraiser.exe") == L"C:\\a,b\\ExileAppraiser.exe",
	      "DisplayIcon: a comma inside the folder name is not an index");
	check(RS::StripIconIndex(L"\"C:\\a b\\ExileAppraiser.exe\",0") == L"C:\\a b\\ExileAppraiser.exe", "DisplayIcon: quoted + index");
	check(RS::DirOf(L"C:\\a b\\ExileAppraiser.exe") == L"C:\\a b" && RS::DirOf(L"C:\\ExileAppraiser.exe") == L"C:\\",
	      "the working directory is the exe's folder (drive root keeps its separator)");
}

void CommandLineTests()
{
	line("[send-cmdline] command line (checked against CommandLineToArgvW)");
	check(RS::QuoteArg(L"--regex-share=H4sI_-x") == L"--regex-share=H4sI_-x", "a share code needs no quoting");
	check(RS::QuoteArg(L"") == L"\"\"", "an empty argument is \"\"");
	const std::wstring exe = L"C:\\Users\\王 小明\\AppData\\Local\\Programs\\ExileAppraiser\\ExileAppraiser.exe";
	const std::vector<std::wstring> args = {
		L"--regex-share=H4sIAAAAAAAACq2Rz2rDMAzG3",
		L"--regex-share-file=C:\\Users\\王 小明\\AppData\\Local\\Temp\\PobTools\\regex-share-1-0a.txt",
		L"--regex-share-file=C:\\a b\\",          // trailing backslash before the closing quote
		L"--regex-share-file=C:\\a\\\\b c\\\\",   // runs of backslashes
		L"x\"y z",                                // an embedded quote
		L"tab\there",
	};
	for (const std::wstring& a : args) {
		const std::vector<std::wstring> v = Argv(RS::BuildCommandLine(exe, a));
		check(v.size() == 2 && v[0] == exe && v[1] == a, "round trip: " + RS::Narrow(a));
	}
	check(RS::BuildCommandLine(L"C:\\x\\ExileAppraiser.exe", L"--regex-share=abc") ==
	          L"\"C:\\x\\ExileAppraiser.exe\" --regex-share=abc",
	      "exact form: quoted exe, bare --regex-share=<code>");
	check(RS::IsCodeCharset("AZaz09-_") && !RS::IsCodeCharset("") && !RS::IsCodeCharset("ab=") &&
	          !RS::IsCodeCharset("a b") && !RS::IsCodeCharset("a+b") && !RS::IsCodeCharset("a\"b"),
	      "code charset: A-Z a-z 0-9 - _ only, non-empty");
}

std::wstring MakeTestDir()
{
	wchar_t tmp[MAX_PATH + 1];
	GetTempPathW(MAX_PATH, tmp);
	const std::wstring root = RS::JoinPath(tmp, L"PobTools-send-selftest-" + std::to_wstring(GetCurrentProcessId()));
	CreateDirectoryW(root.c_str(), nullptr);
	const std::wstring dir = RS::JoinPath(root, L"暫存 資料夾");
	CreateDirectoryW(dir.c_str(), nullptr);
	return dir;
}

// Non-recursive: deletes the files we know we made, then the (now empty) folders.
void RemoveTestDir(const std::wstring& dir)
{
	WIN32_FIND_DATAW fd;
	HANDLE f = FindFirstFileW(RS::JoinPath(dir, L"*").c_str(), &fd);
	std::vector<std::wstring> files, dirs;
	if (f != INVALID_HANDLE_VALUE) {
		do {
			const std::wstring n = fd.cFileName;
			if (n == L"." || n == L"..") continue;
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;   // never follow / delete
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) dirs.push_back(RS::JoinPath(dir, n));
			else files.push_back(RS::JoinPath(dir, n));
		} while (FindNextFileW(f, &fd));
		FindClose(f);
	}
	for (const std::wstring& p : files) DeleteFileW(p.c_str());
	for (const std::wstring& d : dirs) RemoveDirectoryW(d.c_str());   // only the empty ones we made
	RemoveDirectoryW(dir.c_str());
	RemoveDirectoryW(RS::DirOf(dir).c_str());
}

void PlanAndSendTests()
{
	line("[send-plan] inline vs temp file, Send() against a fake launcher");
	const std::wstring dir = MakeTestDir();
	const std::wstring exe = L"C:\\Users\\王 小明\\Apps\\Exile Appraiser\\ExileAppraiser.exe";

	RS::Plan p;
	std::string err;
	check(RS::PlanSend("H4sIAAA_-z", dir, L"20261007-120000", 1, p, &err) && p.tempFile.empty() &&
	          p.arg == L"--regex-share=H4sIAAA_-z",
	      "a short code goes inline as --regex-share=<code>, no file");
	const std::string at(RS::kMaxInlineChars, 'A');
	check(RS::PlanSend(at, dir, L"20261007-120000", 2, p, &err) && p.tempFile.empty() &&
	          p.arg.size() == std::wstring(L"--regex-share=").size() + RS::kMaxInlineChars,
	      "exactly 30000 characters: still inline");
	const std::string over = at + "B";
	check(RS::PlanSend(over, dir, L"20261007-120000", 0xabcdef01u, p, &err) && !p.tempFile.empty(),
	      "30001 characters: written to a temp file");
	const std::wstring wantPath = RS::JoinPath(dir, L"regex-share-20261007-120000-abcdef01.txt");
	check(p.tempFile == wantPath && p.arg == L"--regex-share-file=" + wantPath,
	      "named regex-share-<stamp>-<8 hex>.txt in the temp folder; --regex-share-file=<absolute path>");
	check(ReadBytes(p.tempFile) == over, "file content = the code exactly (UTF-8, no BOM, no newline)");
	check(p.arg.find(L"--regex-share=") == std::wstring::npos, "never both flags: the file form has no --regex-share=");
	const std::wstring first = p.tempFile;
	check(!RS::PlanSend(over, dir, L"20261007-120000", 0xabcdef01u, p, &err) && ReadBytes(first) == over,
	      "a name clash never overwrites an existing file (CREATE_NEW)");
	DeleteFileW(first.c_str());
	check(!RS::PlanSend("abc=", dir, L"x", 3, p, &err) && !RS::PlanSend("", dir, L"x", 3, p, &err),
	      "a code outside base64url, or empty, is refused");
	check(!RS::PlanSend(over + "+", dir, L"20261007-120000", 9, p, &err) &&
	          !Exists(RS::JoinPath(dir, RS::TempFileName(L"20261007-120000", 9))),
	      "a refused long code leaves no file behind");

	{
		FakeLauncher l;
		const RS::Result r = RS::Send(exe, "H4sIabc", dir, l, L"s", 1);
		const std::vector<std::wstring> v = Argv(l.cmd);
		check(r.ok && l.calls == 1 && l.exe == exe && l.cwd == L"C:\\Users\\王 小明\\Apps\\Exile Appraiser" &&
		          v.size() == 2 && v[0] == exe && v[1] == L"--regex-share=H4sIabc" && r.tempFile.empty(),
		      "Send: CreateProcessW(exe, \"<exe>\" --regex-share=<code>, cwd = exe folder) -- spaces and Chinese in the path");
	}
	{
		FakeLauncher l;
		l.watchFile = RS::JoinPath(dir, RS::TempFileName(L"s", 2));
		const RS::Result r = RS::Send(exe, std::string(20, 'Q'), dir, l, L"s", 2, 10);
		const std::vector<std::wstring> v = Argv(l.cmd);
		check(r.ok && l.fileExistedAtLaunch && r.tempFile == l.watchFile && v.size() == 2 &&
		          v[1] == L"--regex-share-file=" + l.watchFile && ReadBytes(r.tempFile) == std::string(20, 'Q'),
		      "Send, long code: file written before launch, path argument survives the quoting, file kept after");
		DeleteFileW(r.tempFile.c_str());
	}
	{
		FakeLauncher l;
		l.fail = true;
		const RS::Result r = RS::Send(exe, std::string(20, 'Q'), dir, l, L"s", 3, 10);
		check(!r.ok && r.message.find(u8"假的失敗") != std::string::npos && r.tempFile.empty() &&
		          !Exists(RS::JoinPath(dir, RS::TempFileName(L"s", 3))),
		      "Send, launch fails: message carries the reason, and the temp file is deleted");
	}
	{
		FakeLauncher l;
		const RS::Result r = RS::Send(exe, "bad code", dir, l, L"s", 4);
		check(!r.ok && l.calls == 0, "Send, bad code: nothing launched");
	}

	line("[send-bookmarks] bookmark packs: --regex-bookmarks / --regex-bookmarks-file (exile-appraiser c9a7aae)");
	// The four flags (--regex-share, --regex-share-file, --regex-bookmarks, --regex-bookmarks-file):
	// exactly one on any command line we build, or ExileAppraiser answers both-flags.
	auto oneFlag = [](const std::vector<std::wstring>& v, const std::wstring& want) {
		int n = 0;
		for (size_t i = 1; i < v.size(); i++)
			for (const wchar_t* f : {L"--regex-share=", L"--regex-share-file=", L"--regex-bookmarks=", L"--regex-bookmarks-file="})
				if (v[i].rfind(f, 0) == 0) n++;
		return v.size() == 2 && n == 1 && v[1].rfind(want, 0) == 0;
	};
	check(std::string(RS::FlagCode(RS::Kind::Bookmarks)) == "--regex-bookmarks=" &&
	          std::string(RS::FlagFile(RS::Kind::Bookmarks)) == "--regex-bookmarks-file=" &&
	          std::string(RS::FlagCode(RS::Kind::Share)) == "--regex-share=" &&
	          std::string(RS::FlagFile(RS::Kind::Share)) == "--regex-share-file=",
	      "flag names as docs/regex-share-cli.md");
	check(RS::PlanSend("H4sIAAA_-z", dir, L"20261007-120000", 11, p, &err, RS::kMaxInlineChars, RS::Kind::Bookmarks) &&
	          p.tempFile.empty() && p.arg == L"--regex-bookmarks=H4sIAAA_-z",
	      "bookmark pack, short: inline as --regex-bookmarks=<code>");
	check(RS::PlanSend(at, dir, L"20261007-120000", 12, p, &err, RS::kMaxInlineChars, RS::Kind::Bookmarks) && p.tempFile.empty(),
	      "bookmark pack, exactly 30000 characters: still inline");
	check(RS::PlanSend(over, dir, L"20261007-120000", 13, p, &err, RS::kMaxInlineChars, RS::Kind::Bookmarks) &&
	          !p.tempFile.empty() && p.arg == L"--regex-bookmarks-file=" + p.tempFile && ReadBytes(p.tempFile) == over &&
	          RS::IsTempFileName(p.tempFile.substr(p.tempFile.find_last_of(L'\\') + 1)),
	      "bookmark pack, 30001 characters: --regex-bookmarks-file=<temp file> (same name pattern, so the same sweep)");
	DeleteFileW(p.tempFile.c_str());
	check(!RS::PlanSend("", dir, L"x", 14, p, &err, RS::kMaxInlineChars, RS::Kind::Bookmarks) && err == u8"沒有書籤包" &&
	          !RS::PlanSend("a b", dir, L"x", 14, p, &err, RS::kMaxInlineChars, RS::Kind::Bookmarks) &&
	          err.find(u8"書籤包") != std::string::npos,
	      "bookmark pack: empty / bad charset refused, the message names the pack");
	{
		FakeLauncher l;
		const RS::Result r = RS::Send(exe, "H4sIbm", dir, l, L"s", 15, RS::kMaxInlineChars, RS::Kind::Bookmarks);
		const std::vector<std::wstring> v = Argv(l.cmd);
		check(r.ok && l.calls == 1 && oneFlag(v, L"--regex-bookmarks=H4sIbm") && r.message.find(u8"加入") != std::string::npos,
		      "Send(bookmarks): one argument, --regex-bookmarks=<code>, no other regex flag; result asks to press 加入");
	}
	{
		FakeLauncher l;
		l.watchFile = RS::JoinPath(dir, RS::TempFileName(L"s", 16));
		const RS::Result r = RS::Send(exe, std::string(20, 'Q'), dir, l, L"s", 16, 10, RS::Kind::Bookmarks);
		const std::vector<std::wstring> v = Argv(l.cmd);
		check(r.ok && l.fileExistedAtLaunch && oneFlag(v, L"--regex-bookmarks-file=") && v[1] == L"--regex-bookmarks-file=" + l.watchFile,
		      "Send(bookmarks), long: one argument, --regex-bookmarks-file=<path> only");
		DeleteFileW(r.tempFile.c_str());
	}
	{
		FakeLauncher l;
		const RS::Result r = RS::Send(exe, "H4sIabc", dir, l, L"s", 17);
		check(r.ok && oneFlag(Argv(l.cmd), L"--regex-share=H4sIabc"), "Send(share) unchanged: --regex-share=<code> only");
	}

	line("[send-cleanup] temp file sweep / session clean-up");
	check(RS::IsTempFileName(L"regex-share-20261007-120000-abcdef01.txt") && !RS::IsTempFileName(L"regex-share-.txt") &&
	          !RS::IsTempFileName(L"regex-share-a.txt.bak") && !RS::IsTempFileName(L"other.txt") &&
	          !RS::IsTempFileName(L"regex-share-x y.txt"),
	      "only regex-share-<digits/hex/->.txt counts as ours");
	const std::wstring oldOurs = RS::JoinPath(dir, L"regex-share-20261001-000000-00000001.txt");
	const std::wstring newOurs = RS::JoinPath(dir, L"regex-share-20261007-000000-00000002.txt");
	const std::wstring oldOther = RS::JoinPath(dir, L"keep-me.txt");
	const std::wstring oldLook = RS::JoinPath(dir, L"regex-share-notes.txt");   // not hex: not ours
	const std::wstring subDir = RS::JoinPath(dir, L"regex-share-20261001-000000-00000003.txt");
	Touch(oldOurs, 7200);
	Touch(newOurs, 60);
	Touch(oldOther, 7200);
	Touch(oldLook, 7200);
	CreateDirectoryW(subDir.c_str(), nullptr);
	const int swept = RS::SweepTempDir(dir, RS::kTempMaxAgeSeconds);
	check(swept == 1 && !Exists(oldOurs) && Exists(newOurs) && Exists(oldOther) && Exists(oldLook) && Exists(subDir),
	      "sweep: only our pattern older than 1 hour; newer, foreign files and a folder of that name stay");
	RemoveDirectoryW(subDir.c_str());
	check(RS::SweepTempDir(RS::JoinPath(dir, L"no such folder"), 0) == 0, "sweep of a missing folder: nothing");

	const std::wstring s1 = RS::JoinPath(dir, L"regex-share-20261007-000000-0000000a.txt");
	const std::wstring s2 = RS::JoinPath(dir, L"regex-share-20261007-000000-0000000b.txt");
	Touch(s1, 600);
	Touch(s2, 10);
	std::vector<std::wstring> session = {s1, s2, RS::JoinPath(dir, L"regex-share-20261007-000000-0000000c.txt"), oldOther};
	const int cleaned = RS::CleanupSession(session, RS::kSessionMinAgeSeconds);
	check(cleaned == 1 && !Exists(s1) && Exists(s2) && Exists(oldOther) && session.size() == 1 && session[0] == s2,
	      "close: this session's files older than 2 minutes go; a fresh one (maybe unread yet) stays, listed");
	DeleteFileW(s2.c_str());
	DeleteFileW(newOurs.c_str());
	DeleteFileW(oldOther.c_str());
	DeleteFileW(oldLook.c_str());
	RemoveTestDir(dir);
	check(!Exists(dir), "test folder removed (non-recursive, no junctions involved)");
}

void ManualStateTests()
{
	line("[send-state] manual pick in regex_ui.json (PobTools-only key, schema unchanged)");
	RegexUiState a;
	const nlohmann::json d0 = nlohmann::json::parse(a.Serialize());
	check(!d0.contains("exileAppraiserExe") && d0["schema"] == 5, "unset: no key written, schema 5");
	a.exileAppraiserExe = u8"E:\\可攜 版\\ExileAppraiser.exe";
	const std::string text = a.Serialize();
	const nlohmann::json d1 = nlohmann::json::parse(text);
	check(d1["schema"] == 5 && d1["exileAppraiserExe"] == a.exileAppraiserExe, "set: written as exileAppraiserExe, still schema 5");
	RegexUiState b;
	check(b.Parse(text) && b.exileAppraiserExe == a.exileAppraiserExe && b.Serialize() == text, "and read back byte-identically");
}

void RealProbe()
{
	const RS::Located r = RS::Locate(RS::RealEnv(), L"");
	line("    (this machine, read-only probe: " +
	     (r.exe.empty() ? std::string("ExileAppraiser.exe not found") : RS::Narrow(r.exe)) + "  step " +
	     std::to_string(r.step) + " = " + RS::SourceLabel(r.source) + ")");
}

} // namespace

void RegexSendTests(void (*chk)(bool, const std::string&), void (*ln)(const std::string&))
{
	g_check = chk;
	g_line = ln;
	LocatorTests();
	CommandLineTests();
	PlanAndSendTests();
	ManualStateTests();
	RealProbe();
}
