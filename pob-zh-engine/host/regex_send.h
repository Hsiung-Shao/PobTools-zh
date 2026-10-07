// "送到 ExileAppraiser": hand a Poe Regex code to ExileAppraiser on its command
// line. The interface is exile-appraiser's docs/regex-share-cli.md
// (main/src/regex-share.ts there, c9a7aae). Two kinds:
//
//   Kind::Bookmarks -- a bookmark pack (regex_bookmarks_share.h Encode): the
//   bookmarks / folders the player picked; ExileAppraiser ADDS them to its
//   bookmarks (same name -> "name (2)"), its ticks untouched. What the panel sends.
//     ExileAppraiser.exe --regex-bookmarks=<code>            code <= kMaxInlineChars
//     ExileAppraiser.exe --regex-bookmarks-file=<abs path>   longer: UTF-8 text file
//
//   Kind::Share -- a share code (regex_share.h Encode): OVERWRITES the ticks of
//   that game there. Kept as logic (and self-tested) but no longer offered by
//   the panel: "複製分享碼" covers handing over the current ticks.
//     ExileAppraiser.exe --regex-share=<code> / --regex-share-file=<abs path>
//
// The code is base64url (A-Z a-z 0-9 - _), so it never needs quoting; the
// file path does. Exactly one of the four flags, ever (two = "both-flags" there). Exit code means nothing: a process that
// started = sent (it forwards to the running instance, which asks the user).
// Needs ExileAppraiser v0.2.1+ (older ones just show their window); the
// Uninstall key's DisplayVersion is stale after auto-updates, so no version check.
//
// Finding the exe (first existing file wins):
//   0. the user's manual pick (regex_ui.json "exileAppraiserExe"), if the file exists
//   1. HKCU\Software\<GUID> InstallLocation + \ExileAppraiser.exe,
//      then HKCU ...\Uninstall\<GUID> DisplayIcon minus its ",0"
//   2. the same two under HKLM (64-bit view)
//   3. %LOCALAPPDATA%\Programs\ExileAppraiser\ExileAppraiser.exe
//   4. a running ExileAppraiser.exe (QueryFullProcessImageNameW)
// A manual pick whose file is gone falls back to 1-4 (and says so).
//
// Temp files (%TEMP%\PobTools\regex-share-<yyyymmdd-hhmmss>-<rand>.txt for
// both kinds -- one name pattern, one sweep; UTF-8,
// no BOM, ExileAppraiser only reads them): ExileAppraiser may read one many
// seconds after we launch it (cold start), so a file is never deleted right
// after the send. Swept instead: on panel load and before each new file, every
// regex-share-*.txt in that folder older than kTempMaxAgeSeconds; on panel
// close, this session's files older than kSessionMinAgeSeconds. Only plain
// files matching the pattern, never recursive, never a reparse point.
//
// Pure apart from Env / Launcher (registry, files, processes, CreateProcessW),
// which the self-test replaces with fakes; no ImGui.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace RegexSend {

constexpr const wchar_t* kAppGuid = L"c805721a-9270-5327-9a8c-78957523e59b";
constexpr const wchar_t* kExeName = L"ExileAppraiser.exe";
constexpr size_t kMaxInlineChars = 30000;      // longer -> --regex-share-file
enum class Kind { Share, Bookmarks };
constexpr const char* kFlagCode = "--regex-share=";
constexpr const char* kFlagFile = "--regex-share-file=";
constexpr const char* kFlagBookmarks = "--regex-bookmarks=";
constexpr const char* kFlagBookmarksFile = "--regex-bookmarks-file=";
// The flag pair of a kind (inline, file).
const char* FlagCode(Kind k);
const char* FlagFile(Kind k);
constexpr long long kTempMaxAgeSeconds = 3600;        // sweep: older than 1 hour
constexpr long long kSessionMinAgeSeconds = 120;      // close: own files older than 2 min

// Everything Locate needs from the machine.
struct Env {
	virtual ~Env() = default;
	// REG_SZ / REG_EXPAND_SZ value (expanded), empty when the key or value is missing.
	// hklm=false: HKCU; true: HKLM, 64-bit view.
	virtual std::wstring RegString(bool hklm, const std::wstring& subkey, const std::wstring& value) = 0;
	virtual bool FileExists(const std::wstring& path) = 0;   // a regular file
	virtual std::wstring LocalAppData() = 0;                 // empty when unknown
	// Full image paths of running processes named `imageName` (case-insensitive).
	virtual std::vector<std::wstring> RunningImages(const std::wstring& imageName) = 0;
};
Env& RealEnv();

enum class Source { None, Manual, HkcuInstall, HkcuUninstall, HklmInstall, HklmUninstall, LocalAppData, Process };

struct Located {
	std::wstring exe;          // empty = not found
	Source source = Source::None;
	int step = 0;              // the spec's step: 1 HKCU, 2 HKLM, 3 %LOCALAPPDATA%, 4 process, 5 manual
	bool manualMissing = false;   // a manual pick was set but its file is gone
};

// Steps above; `manual` = the stored pick (may be empty).
Located Locate(Env& env, const std::wstring& manual);
const char* SourceLabel(Source s);   // Chinese, for the panel

// "C:\x\ExileAppraiser.exe,0" -> "C:\x\ExileAppraiser.exe": drops surrounding
// quotes and a trailing ",<index>" (digits, optional '-').
std::wstring StripIconIndex(const std::wstring& displayIcon);
// dir + "\" + name, without doubling a trailing separator; surrounding quotes dropped.
std::wstring JoinPath(const std::wstring& dir, const std::wstring& name);
std::wstring DirOf(const std::wstring& path);

// One argument quoted for CommandLineToArgvW / the MSVC CRT: left bare when it
// has no space, tab, newline or quote; else wrapped in quotes, backslashes
// before a quote (or the closing quote) doubled, quotes escaped.
std::wstring QuoteArg(const std::wstring& arg);
// "<exe>" <arg>: the exe always quoted (it cannot contain a quote).
std::wstring BuildCommandLine(const std::wstring& exe, const std::wstring& arg);

// The code is non-empty and only A-Z a-z 0-9 - _.
bool IsCodeCharset(const std::string& code);

// What to put on the command line. Long codes are written to a temp file here.
struct Plan {
	std::wstring arg;          // the single argument (FlagCode(kind)... or FlagFile(kind)...)
	std::wstring tempFile;     // the file written (empty for the inline form)
};
// tempDir: where long codes go (created if missing). stamp/rnd make the name
// (callers pass the time and a random number; tests pass fixed ones).
// False + *err on a bad code or a failed write (nothing left behind then).
bool PlanSend(const std::string& code, const std::wstring& tempDir, const std::wstring& stamp, uint32_t rnd,
              Plan& out, std::string* err, size_t maxInline = kMaxInlineChars, Kind kind = Kind::Share);

// %TEMP%\PobTools (GetTempPathW), no trailing separator.
std::wstring DefaultTempDir();
// "20261007-153012" (local time).
std::wstring NowStamp();
uint32_t RandomU32();
// regex-share-<stamp>-<8 hex>.txt
std::wstring TempFileName(const std::wstring& stamp, uint32_t rnd);
bool IsTempFileName(const std::wstring& name);
// Deletes regex-share-*.txt plain files (not reparse points) directly in `dir`
// whose last write is more than maxAgeSeconds before `nowFileTime` (FILETIME as
// 100 ns ticks; 0 = now). Returns how many were deleted.
int SweepTempDir(const std::wstring& dir, long long maxAgeSeconds, unsigned long long nowFileTime = 0);
// Deletes those of `files` (this session's) that look like ours and are older
// than minAgeSeconds; the deleted / vanished ones are removed from the list.
int CleanupSession(std::vector<std::wstring>& files, long long minAgeSeconds, unsigned long long nowFileTime = 0);

// Starting the process.
struct Launcher {
	virtual ~Launcher() = default;
	// CreateProcessW(exe, cmdline, cwd), no shell, no wait. False + *err (Chinese) on failure.
	virtual bool Launch(const std::wstring& exe, const std::wstring& cmdline, const std::wstring& cwd,
	                    std::string* err) = 0;
};
Launcher& RealLauncher();

struct Result {
	bool ok = false;
	std::string message;       // Chinese, for the panel
	std::wstring tempFile;     // a file this send wrote (track it for CleanupSession)
};
// Plan + launch with cwd = the exe's folder. A temp file of a failed launch is deleted.
Result Send(const std::wstring& exe, const std::string& code, const std::wstring& tempDir, Launcher& launcher,
            const std::wstring& stamp, uint32_t rnd, size_t maxInline = kMaxInlineChars, Kind kind = Kind::Share);

std::string Narrow(const std::wstring& w);   // UTF-8
std::wstring Widen(const std::string& s);    // from UTF-8

// The "pick ExileAppraiser.exe" dialog (GetOpenFileNameW); empty = cancelled.
std::wstring PickExeDialog(void* owner, const std::wstring& initialDir);

} // namespace RegexSend
