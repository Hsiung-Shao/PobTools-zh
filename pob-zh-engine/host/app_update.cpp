#include "app_update.h"
#include "app_version.h"
#include "http_client.h"
#include "launcher_config.h" // LoadLauncherConfig (the CLI honours the same opt-out)
#include "zip_extract.h"
#include "hash_sha256.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>

#include <json.hpp> // nlohmann::ordered_json (deps/nlohmann)
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>

#include <miniz.h> // self test builds in-memory archives

using nlohmann::ordered_json;

#pragma comment(lib, "shell32.lib")

// ---- release source -------------------------------------------------------

static const wchar_t* kApiHost = L"api.github.com";
// App 線。releases/latest 只回最新的 non-prerelease、non-draft release,而
// data-<n> 一律標 prerelease —— 所以這個端點的語意天生就是「最新程式版」,
// 契約與 v0.18.0 以前的客戶端完全一致。
static const wchar_t* kLatestPath = L"/repos/Hsiung-Shao/PobTools-zh/releases/latest";
// Data 線。⚠ 這份清單依 created_at(tag 指向的 commit 時間)排序,不是依 tag,
// 從舊 commit 打的 data tag 會排到後面 —— 所以一律解出序號自己比大小,
// 絕不取第一個命中的。
static const wchar_t* kReleasesPath = L"/repos/Hsiung-Shao/PobTools-zh/releases?per_page=100";
// 資產名的前綴。改成前綴比對(而非 "PobTools-" + tag + ".zip" 字串拼接)是拆
// 兩線的前提:資料包的檔名裡根本沒有程式版號,拼不出來。
static const char* kAppAssetPrefix = "PobTools-update-";   // 主檔包(不含字典)
static const char* kDataAssetPrefix = "PobTools-Data-";    // 翻譯資料包
// 安裝目錄裡的翻譯資料版本戳記。內容 {"dataVersion":"data-3"}。
static const wchar_t* kDataStampRel = L"Data\\translations_version.json";

// ---- Status.message vocabulary --------------------------------------------
//
// 為什麼是一份 X-macro 而不是一段手抄的字串:
//
// 啟動器的字型圖集是**一次性**建好的,而 Status.message 是執行期才組出來的,所以
// 訊息用到的字必須事先餵給圖集。以前這裡是一段人工維護的「把所有訊息串起來」的
// 常數,而 ImGui 對沒有 glyph 的字直接畫 '?',不 assert 不 log —— 於是
// 「訊息裡有、種子裡沒有」這個狀態完全是靜默的。
//
// ⚠ 它真的發生過:「（目前設定為不自動更新）」的 設/自/動 與「請先關閉所有 POB
// 視窗」的 請/先/關/閉/所/視/窗 在 v0.18.0 就已經在送出,卻從來不在種子裡。
// --font-coverage-selftest **當時就已經在吃這個種子了**,而且是綠的 —— 因為它問的
// 是「種子裡的字畫不畫得出來」,不是「訊息用到的字在不在種子裡」。守門守的是錯的
// 那一半,所以漏了一整版都沒人知道。
//
// 現在:訊息片語列在下面這份清單裡,種子由清單**生成**(不可能不同步),而
// --app-update-selftest 的 T13 反過來檢查「每一則實際組出來的訊息,每個字都在種子
// 裡」,並用一個故意沒列進來的探針字證明這道檢查真的會失敗。
//
// 加新訊息的規矩:片語加進這份清單、用生成的常數,不要在 setPhase 現場打字面值。
#define PT_UPD_MSGS(X)                                                          \
	/* 檢查與結果 */                                                            \
	X(kMsgChecking,        u8"檢查更新中…")                                     \
	X(kMsgUpToDate,        u8"已是最新版 v")                                     \
	X(kMsgAppFound,        u8"發現新版 v")                                       \
	X(kMsgAppCurrent,      u8"（目前 v")                                         \
	X(kMsgCloseParen,      u8"）")                                               \
	/* 翻譯資料線 */                                                            \
	X(kMsgDataFound,       u8"有新的翻譯資料 ")                                  \
	X(kMsgDataOptedOut,    u8"（目前設定為不自動更新）")                         \
	X(kMsgDataDownloading, u8"下載新翻譯資料 ")                                  \
	X(kMsgDataDone,        u8"翻譯資料已更新至 ")                                \
	X(kMsgDataEffective,   u8"（引擎下次啟動生效）")                             \
	X(kMsgNoDataAsset,     u8"找不到對應的翻譯資料")                             \
	X(kMsgNoDataRelease,   u8"尚無翻譯資料發佈")                                 \
	X(kMsgDataPackBad,     u8"翻譯資料包內容驗證失敗")                           \
	/* 程式主體線 */                                                            \
	X(kMsgAppDownloading,  u8"下載主體更新 v")                                   \
	X(kMsgEllipsis,        u8"…")                                               \
	X(kMsgStaging,         u8"解壓與驗證更新檔…")                                \
	X(kMsgReadyRestart,    u8"更新檔就緒，即將重新啟動…")                        \
	X(kMsgNoAppAsset,      u8"找不到對應的發佈資產")                             \
	X(kMsgNoDiskSpace,     u8"磁碟空間不足（需 300MB）")                         \
	/* 失敗與阻擋 */                                                            \
	X(kMsgPobRunning,      u8"POB 執行中，請先關閉所有 POB 視窗")                \
	X(kMsgHashMismatch,    u8"下載檔案雜湊值不符（下載損毀？）")                 \
	X(kMsgBadAssetUrl,     u8"發佈資產網址無效: ")                               \
	X(kMsgBadVersion,      u8"發佈版號無法解析: ")                               \
	X(kMsgAssetNotUnique,  u8"發佈資產名稱不唯一: ")                             \
	X(kMsgBadReleaseInfo,  u8"版本資訊解析失敗")                                 \
	X(kMsgBadReleaseList,  u8"發佈清單解析失敗")                                 \
	X(kMsgStageBad,        u8"更新暫存檔驗證失敗（可能被防毒隔離）")             \
	X(kMsgLockFailed,      u8"無法建立更新鎖")                                   \
	X(kMsgOtherInstance,   u8"另一個 PobTools 實例正在套用更新")                 \
	X(kMsgApplyFailed,     u8"更新套用失敗")                                     \
	X(kMsgRolledBack,      u8"（已還原舊版）")                                   \
	X(kMsgCopyFailed,      u8"內容檔複製失敗: ")                                 \
	X(kMsgReplaceFailed,   u8"內容檔替換失敗: ")                                 \
	X(kMsgBackupFailed,    u8"備份失敗: ")                                       \
	X(kMsgPlaceFailed,     u8"放置失敗: ")

#define PT_UPD_DEFINE(name, lit) static const char* const name = lit;
PT_UPD_MSGS(PT_UPD_DEFINE)
#undef PT_UPD_DEFINE

// Index-aligned with nothing -- just an enumerable copy, so T13 can walk every
// phrase instead of trusting that someone remembered to seed it.
static const char* const kUpdMsgFragments[] = {
#define PT_UPD_LIST(name, lit) lit,
	PT_UPD_MSGS(PT_UPD_LIST)
#undef PT_UPD_LIST
};

// ⚠ 這一段是**別的 TU** 丟進 Status.message 的訊息(http_client.cpp、
// zip_extract.cpp)。它們不在上面那份清單裡,所以這裡仍然是手抄的 —— 改那兩個檔
// 的訊息時要回來補。單獨標出來,是為了讓「還沒被結構性保護的部分」有明確邊界,
// 而不是混在一起假裝全部都安全。
#define PT_UPD_EXTERNAL_SEED                                                   \
	u8"已取消回應為空連線失敗（網路無法使用？）建立 HTTP 請求 HTTPS 初始化"     \
	u8"無法建立解壓目錄格式無效條目資訊讀取路徑非法檔案寫入失敗回滾備份"
static const char* const kExternalMsgSeed = PT_UPD_EXTERNAL_SEED;

// Generated: the concatenation of every phrase above. Cannot drift from the
// list, because it IS the list.
#define PT_UPD_SEED(name, lit) lit
const char* kAppUpdateGlyphSeed = PT_UPD_MSGS(PT_UPD_SEED) PT_UPD_EXTERNAL_SEED;
#undef PT_UPD_SEED

// 組合型訊息全部集中在這裡,而不是在 setPhase 現場拼字串。兩個好處:
//   1. T13 有東西可以驅動 —— 它能真的產生一則完整訊息再逐字檢查,而不是只檢查
//      片語(片語由定義就在種子裡,那種檢查是恆真的);
//   2. 版號/標籤這些插進去的東西是 ASCII,所以「訊息用到的字」嚴格等於「片語用
//      到的字」,這條等式是 T13 能成立的前提。
static std::string MsgUpToDate()
{
	return std::string(kMsgUpToDate) + POBTOOLS_VERSION_STRING;
}
static std::string MsgAppAvailable(const std::string& ver)
{
	return std::string(kMsgAppFound) + ver + kMsgAppCurrent + POBTOOLS_VERSION_STRING +
	       kMsgCloseParen;
}
static std::string MsgAppDownloading(const std::string& ver)
{
	return std::string(kMsgAppDownloading) + ver + kMsgEllipsis;
}
static std::string MsgDataAvailable(const std::string& tag)
{
	return std::string(kMsgDataFound) + tag + kMsgDataOptedOut;
}
static std::string MsgDataDownloading(const std::string& tag)
{
	return std::string(kMsgDataDownloading) + tag + kMsgEllipsis;
}
static std::string MsgDataApplied(const std::string& tag)
{
	return std::string(kMsgDataDone) + tag + kMsgDataEffective;
}

// ---- small helpers (same conventions as atlas_update.cpp) ------------------

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

static bool write_file_bytes(const std::wstring& path, const void* data, size_t size)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	DWORD written = 0;
	bool ok = size == 0 || (WriteFile(h, data, (DWORD)size, &written, nullptr) && written == (DWORD)size);
	CloseHandle(h);
	return ok;
}

// atomic-ish: write .tmp beside the target, then rename over it
static bool write_file_atomic(const std::wstring& path, const std::string& content)
{
	std::wstring tmp = path + L".tmp";
	if (!write_file_bytes(tmp, content.data(), content.size())) return false;
	if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
		DeleteFileW(tmp.c_str());
		return false;
	}
	return true;
}

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

// "0.2.0" -> {0,2,0}; rejects anything with a suffix ("0.2.0-rc1").
static bool parse_semver(const std::string& s, std::tuple<int, int, int>* out)
{
	int a = 0, b = 0, c = 0, used = 0;
	if (sscanf_s(s.c_str(), "%d.%d.%d%n", &a, &b, &c, &used) != 3 || used != (int)s.size())
		return false;
	if (out) *out = { a, b, c };
	return true;
}

static long long now_filetime()
{
	FILETIME ft{};
	GetSystemTimeAsFileTime(&ft);
	ULARGE_INTEGER u{ ft.dwLowDateTime, ft.dwHighDateTime };
	return (long long)u.QuadPart;
}

static bool file_exists(const std::wstring& p)
{
	DWORD a = GetFileAttributesW(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool dir_exists(const std::wstring& p)
{
	DWORD a = GetFileAttributesW(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static long long file_size(const std::wstring& p)
{
	WIN32_FILE_ATTRIBUTE_DATA fad{};
	if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fad)) return -1;
	return ((long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
}

static void ensure_parent_dir(const std::wstring& filePath)
{
	size_t bs = filePath.find_last_of(L'\\');
	if (bs != std::wstring::npos)
		SHCreateDirectoryExW(nullptr, filePath.substr(0, bs).c_str(), nullptr);
}

// Relative paths ("Data\\x.json") of every plain file under dir (trailing
// backslash), depth-first.
static void list_files_rec(const std::wstring& dir, const std::wstring& rel,
                           std::vector<std::wstring>* out)
{
	WIN32_FIND_DATAW fd{};
	HANDLE h = FindFirstFileW((dir + rel + L"*").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return;
	do {
		std::wstring name = fd.cFileName;
		if (name == L"." || name == L"..") continue;
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			list_files_rec(dir, rel + name + L"\\", out);
		else
			out->push_back(rel + name);
	} while (FindNextFileW(h, &fd));
	FindClose(h);
}

// Recursive best-effort delete. Only ever pointed at PobTools-owned scratch
// paths (cache/stage/selftest); never at the install root or POB folders.
static void remove_dir_rec(const std::wstring& dir)
{
	WIN32_FIND_DATAW fd{};
	HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			std::wstring name = fd.cFileName;
			if (name == L"." || name == L"..") continue;
			std::wstring p = dir + L"\\" + name;
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
				remove_dir_rec(p);
			} else {
				SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
				DeleteFileW(p.c_str());
			}
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}
	RemoveDirectoryW(dir.c_str());
}

// "https://github.com/a/b" -> host "github.com", path "/a/b" (kept encoded)
static bool split_https_url(const std::string& url, std::wstring* host, std::wstring* path)
{
	const char* pfx = "https://";
	if (url.compare(0, 8, pfx) != 0) return false;
	size_t slash = url.find('/', 8);
	if (slash == std::string::npos || slash == 8) return false;
	*host = widen(url.substr(8, slash - 8));
	*path = widen(url.substr(slash));
	return true;
}

static std::string to_lower_ascii(std::string s)
{
	for (char& c : s) if (c >= 'A' && c <= 'Z') c += 32;
	return s;
}

// Append one line to PobTools\app_update_log.txt (worker failures, CLI runs).
static void log_line(const std::wstring& exeDir, const std::string& msg)
{
	std::wstring dir = exeDir + L"PobTools";
	CreateDirectoryW(dir.c_str(), nullptr);
	HANDLE h = CreateFileW((dir + L"\\app_update_log.txt").c_str(), FILE_APPEND_DATA,
	                       FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return;
	SYSTEMTIME st{};
	GetLocalTime(&st);
	char head[64];
	int n = sprintf_s(head, "[%04d-%02d-%02d %02d:%02d:%02d] ",
	                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
	DWORD w = 0;
	WriteFile(h, head, (DWORD)n, &w, nullptr);
	WriteFile(h, msg.c_str(), (DWORD)msg.size(), &w, nullptr);
	WriteFile(h, "\r\n", 2, &w, nullptr);
	CloseHandle(h);
}

// Two-pass content apply shared by the translation updater and the app swap:
// copy every rel from stage as <dst>.new first, then per-file atomic rename.
// On failure removes the pending .new files; files already renamed keep the
// new version (independent dictionaries — same as a partial manual update).
static bool apply_content_two_pass(const std::wstring& exeDir, const std::wstring& stage,
                                   const std::vector<std::wstring>& rels, std::string* err)
{
	std::vector<std::wstring> staged;
	for (const std::wstring& rel : rels) {
		std::wstring dst = exeDir + rel + L".new";
		ensure_parent_dir(dst);
		if (!CopyFileW((stage + rel).c_str(), dst.c_str(), FALSE)) {
			if (err) *err = kMsgCopyFailed + narrow(rel);
			for (const std::wstring& s : staged) DeleteFileW((exeDir + s + L".new").c_str());
			return false;
		}
		staged.push_back(rel);
	}
	for (size_t i = 0; i < rels.size(); i++) {
		if (!MoveFileExW((exeDir + rels[i] + L".new").c_str(), (exeDir + rels[i]).c_str(),
		                 MOVEFILE_REPLACE_EXISTING)) {
			if (err) *err = kMsgReplaceFailed + narrow(rels[i]);
			for (size_t j = i; j < rels.size(); j++)
				DeleteFileW((exeDir + rels[j] + L".new").c_str());
			return false;
		}
	}
	return true;
}

// ---- translation-data classification ---------------------------------------

// THE boundary between the two release lines (see the header for why the safety
// direction flipped in v0.19.0). package_release.ps1 no longer keeps its own
// copy of this rule -- it calls --translation-data-list, which walks the staged
// tree through this exact function -- so there is one definition again.
bool IsTranslationDataRel(const std::wstring& rel)
{
	std::wstring p = rel;
	for (wchar_t& c : p) {
		if (c == L'/') c = L'\\';
		else c = (wchar_t)towlower(c);
	}
	if (p.compare(0, 5, L"data\\") != 0) return false;
	const std::wstring tail = p.substr(5);

	auto ends_with = [](const std::wstring& s, const wchar_t* suffix) {
		const size_t n = wcslen(suffix);
		return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
	};
	// Data\<game>\<locale>\*.json -- the POB dictionaries and the launcher's own
	// labels, which live under Data\launcher\<locale>\ for exactly this reason.
	for (const wchar_t* game : { L"poe1\\", L"poe2\\", L"launcher\\" }) {
		const size_t n = wcslen(game);
		if (tail.compare(0, n, game) != 0) continue;
		const std::wstring rest = tail.substr(n);
		// one more directory level, then a .json file: <locale>\<name>.json
		const size_t slash = rest.find(L'\\');
		if (slash == std::wstring::npos) return false;
		const std::wstring leaf = rest.substr(slash + 1);
		return !leaf.empty() && leaf.find(L'\\') == std::wstring::npos && ends_with(leaf, L".json");
	}
	// ⚠ Data\atlas_versions\<tag>\atlas_tree_zh.json used to be here and is
	// deliberately NOT any more. A season folder is only adopted when
	// atlas_tree_poe1.json + atlas_tree_zh.json + atlas\ are ALL present
	// (atlas_version_index.cpp, adoptFromDisk); putting one of the three in the
	// other zip splits the season in half, and anyone who has the app pack but
	// not yet the matching data pack loses the whole new league -- on the day the
	// league starts, which is exactly when it hurts most. The atlas text is
	// machine-fetched and rebuilt by the in-app updater anyway; translating it is
	// not a PR-shaped job (the same argument .gitignore already makes).
	if (tail.compare(0, 15, L"atlas_versions\\") == 0) return false;

	// The stamp itself is translation data: it must be swapped by the same
	// two-pass apply as the dictionaries it describes, or "content moved, version
	// did not" becomes reachable.
	return tail == L"translations_version.json" || tail == L"filter_items_zh.json" ||
	       tail == L"item_meta.json" || tail == L"item_classes_zh.json";
}

std::string ReadLocalDataVersion(const std::wstring& exeDir)
{
	std::string content;
	if (!read_file_utf8(exeDir + kDataStampRel, content)) return std::string();
	try {
		ordered_json v = ordered_json::parse(content);
		return v.value("dataVersion", std::string());
	} catch (...) {
		// A corrupt stamp reads as "unstamped", which costs one redundant
		// download and then repairs itself -- the alternative (treating it as
		// current) would strand the install on stale dictionaries forever.
		return std::string();
	}
}

// ---- policy -----------------------------------------------------------------

bool ParseDataTagSeq(const std::string& tag, long long* seq)
{
	if (tag.compare(0, 5, "data-") != 0 || tag.size() <= 5) return false;
	long long n = 0;
	for (size_t i = 5; i < tag.size(); i++) {
		if (tag[i] < '0' || tag[i] > '9') return false;
		n = n * 10 + (tag[i] - '0');
		if (n > 1'000'000'000ll) return false; // absurd: treat as malformed
	}
	if (seq) *seq = n;
	return true;
}

UpdatePlan PlanUpdates(bool hasAppAsset, std::tuple<int, int, int> remoteApp,
                       std::tuple<int, int, int> localApp,
                       bool hasDataAsset, long long remoteDataSeq, long long localDataSeq,
                       bool autoData)
{
	UpdatePlan p;
	// EVERY app bump prompts now, patch included. The old rule ("patch = data
	// only, apply it silently") rested on patch releases touching nothing but
	// Data\; with the dictionaries on their own line that premise is gone, and
	// applying an app pack always means renaming pob-zh.exe + engine\* and
	// restarting the process. Doing that without a click would close the program
	// in front of the user.
	p.promptApp = hasAppAsset && remoteApp > localApp;

	// -1 (unstamped install) is smaller than every real sequence, so a v0.18.0
	// install picks up the newest pack exactly once and is stamped from then on.
	const bool dataNewer = hasDataAsset && remoteDataSeq > localDataSeq;
	p.applyDataNow = dataNewer && autoData;
	p.offerData = dataNewer && !autoData;
	return p;
}

// ---- AppUpdater --------------------------------------------------------------

void AppUpdater::loadState()
{
	appliedTrans_.clear();
	appliedApp_.clear();
	latestSeen_.clear();
	lastCheckUtc_ = 0;
	std::string content;
	if (read_file_utf8(exeDir_ + L"PobTools\\update_state.json", content)) {
		try {
			ordered_json v = ordered_json::parse(content);
			appliedTrans_ = v.value("appliedTranslations", std::string());
			appliedApp_ = v.value("appliedApp", std::string());
			latestSeen_ = v.value("latestSeen", std::string());
			lastCheckUtc_ = v.value("lastCheckUtc", 0ll);
		} catch (...) {
			// corrupt record reads as "never checked": forces a fresh check
			appliedTrans_.clear();
			appliedApp_.clear();
			latestSeen_.clear();
			lastCheckUtc_ = 0;
		}
	}
}

void AppUpdater::saveState()
{
	CreateDirectoryW((exeDir_ + L"PobTools").c_str(), nullptr);
	ordered_json v;
	v["lastCheckUtc"] = (long long)lastCheckUtc_;
	v["latestSeen"] = latestSeen_;
	v["appliedTranslations"] = appliedTrans_;
	v["appliedApp"] = appliedApp_;
	write_file_atomic(exeDir_ + L"PobTools\\update_state.json", v.dump(2));
}

void AppUpdater::Init(const std::wstring& exeDir)
{
	exeDir_ = exeDir;
	stop_.store(false); // support re-Init after a failed apply (Shutdown set it)
	loadState();
	{
		std::lock_guard<std::mutex> lk(stMx_);
		st_.localVer = POBTOOLS_VERSION_STRING;
		st_.localDataVer = ReadLocalDataVersion(exeDir_);
	}
	worker_ = std::thread(&AppUpdater::workerLoop, this);
}

void AppUpdater::Shutdown()
{
	if (worker_.joinable()) {
		{
			std::lock_guard<std::mutex> lk(cmdMx_);
			stop_.store(true);
		}
		cmdCv_.notify_all();
		worker_.join();
	}
}

void AppUpdater::RequestCheck(bool force)
{
	if (!worker_.joinable()) return;
	static const long long kDay = 24ll * 3600 * 10'000'000; // FILETIME is 100ns units
	if (!force && lastCheckUtc_ > 0 && now_filetime() - lastCheckUtc_ < kDay)
		return; // throttled: checked within the last day
	{
		std::lock_guard<std::mutex> lk(cmdMx_);
		cmdQ_.push_back(Cmd::Check);
	}
	cmdCv_.notify_one();
}

void AppUpdater::StartAppUpdate()
{
	if (!worker_.joinable()) return;
	{
		std::lock_guard<std::mutex> lk(cmdMx_);
		cmdQ_.push_back(Cmd::UpdateApp);
	}
	cmdCv_.notify_one();
}

void AppUpdater::StartTranslationUpdate()
{
	if (!worker_.joinable()) return;
	{
		std::lock_guard<std::mutex> lk(cmdMx_);
		cmdQ_.push_back(Cmd::UpdateTranslations);
	}
	cmdCv_.notify_one();
}

AppUpdater::Status AppUpdater::Poll()
{
	std::lock_guard<std::mutex> lk(stMx_);
	return st_;
}

void AppUpdater::AckNotice()
{
	std::lock_guard<std::mutex> lk(stMx_);
	if (st_.phase == AppUpdatePhase::TransDone || st_.phase == AppUpdatePhase::UpToDate ||
	    st_.phase == AppUpdatePhase::Error) {
		st_.phase = AppUpdatePhase::Idle;
		st_.message.clear();
	}
}

void AppUpdater::setPhase(AppUpdatePhase p, const std::string& msg)
{
	std::lock_guard<std::mutex> lk(stMx_);
	st_.phase = p;
	st_.message = msg;
}

void AppUpdater::workerLoop()
{
	for (;;) {
		Cmd cmd;
		{
			std::unique_lock<std::mutex> lk(cmdMx_);
			cmdCv_.wait(lk, [&] { return stop_.load() || !cmdQ_.empty(); });
			if (stop_.load()) break;
			cmd = cmdQ_.front();
			cmdQ_.pop_front();
		}
		std::string err;
		if (cmd == Cmd::Check) {
			if (!doCheck(&err)) {
				// a failed background check stays quiet; retried next launch
				// because lastCheckUtc is only persisted on success
				log_line(exeDir_, "check failed: " + err);
				setPhase(AppUpdatePhase::Idle, "");
			}
		} else if (cmd == Cmd::UpdateTranslations) {
			// The one-off apply behind the TransAvailable notice and behind the
			// "no dictionaries at all" banner. Deliberately not gated on
			// transUpdates_: the user pressed this.
			if (hold_.load()) {
				setPhase(AppUpdatePhase::Error, kMsgPobRunning);
			} else {
				// 按鈕可能在任何檢查跑完之前就被按下(全新安裝的橫幅、或這一輪
				// 檢查因為 POB 開著被跳過)。少了這一步,按鈕會回「找不到對應的
				// 翻譯資料」—— 而事實只是還沒去問過,使用者無從分辨。
				if (!latest_.hasData) {
					std::string derr;
					if (!fetchDataRelease(&latest_, &derr))
						log_line(exeDir_, "manual translation update: data line failed: " + derr);
				}
				if (!latest_.hasData) {
					setPhase(AppUpdatePhase::Error, kMsgNoDataAsset);
				} else if (!doUpdateTranslations(&err)) {
					log_line(exeDir_, "manual translation update failed: " + err);
					setPhase(AppUpdatePhase::Error, err);
				}
			}
		} else {
			if (!doUpdateApp(&err)) {
				log_line(exeDir_, "app update failed: " + err);
				setPhase(AppUpdatePhase::Error, err);
			}
		}
	}
}

bool AppUpdater::downloadAsset(const std::string& url, const std::string& sha256hex,
                               std::vector<unsigned char>* out, std::string* err, bool reportBytes)
{
	std::wstring host, path;
	if (!split_https_url(url, &host, &path)) {
		if (err) *err = kMsgBadAssetUrl + url;
		return false;
	}
	HttpsClient c(host);
	HttpsClient::ProgressFn progress = nullptr;
	if (reportBytes) {
		progress = [this](unsigned long long got, unsigned long long total) {
			std::lock_guard<std::mutex> lk(stMx_);
			st_.bytesDone = got;
			st_.bytesTotal = total;
		};
	}
	// browser_download_url 302s to objects.githubusercontent.com; WinHTTP's
	// default redirect policy follows HTTPS->HTTPS across hosts.
	if (!c.Get(path, *out, err, &stop_, progress)) return false;
	if (!sha256hex.empty()) {
		std::string got;
		if (!Sha256Hex(out->data(), out->size(), &got) ||
		    got != to_lower_ascii(sha256hex)) {
			if (err) *err = kMsgHashMismatch;
			return false;
		}
	} else {
		log_line(exeDir_, "asset without digest, hash check skipped: " + url);
	}
	return true;
}

// One release's asset list -> the single entry whose name starts with `prefix`
// and ends in ".zip". "Exactly one" is asserted rather than "the first one": two
// matches means the release carries an asset nobody planned for, and picking one
// at random would install it.
static bool pick_asset(const ordered_json& release, const char* prefix,
                       std::string* url, std::string* sha, std::string* why)
{
	int hits = 0;
	if (release.contains("assets")) {
		for (const auto& a : release["assets"]) {
			std::string name = a.value("name", std::string());
			if (name.compare(0, strlen(prefix), prefix) != 0) continue;
			if (name.size() < 4 || name.compare(name.size() - 4, 4, ".zip") != 0) continue;
			hits++;
			*url = a.value("browser_download_url", std::string());
			std::string digest = a.value("digest", std::string());
			if (digest.compare(0, 7, "sha256:") == 0) digest.erase(0, 7);
			else digest.clear(); // unknown scheme: skip hash check
			*sha = digest;
		}
	}
	if (hits == 1) return true;
	if (hits > 1 && why) *why = kMsgAssetNotUnique + std::string(prefix);
	url->clear();
	sha->clear();
	return false;
}

bool AppUpdater::fetchAppRelease(RemoteRelease* rel, std::string* err)
{
	std::string body;
	{
		HttpsClient api(kApiHost);
		if (!api.GetString(kLatestPath, body, err, &stop_)) return false;
	}
	try {
		ordered_json j = ordered_json::parse(body);
		std::string tag = j.value("tag_name", std::string());
		if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) tag.erase(0, 1);
		rel->appVer = tag;
		std::string why;
		rel->hasApp = pick_asset(j, kAppAssetPrefix, &rel->appUrl, &rel->appSha, &why);
		if (!why.empty()) log_line(exeDir_, "app asset rejected: " + why);
	} catch (...) {
		if (err) *err = kMsgBadReleaseInfo;
		return false;
	}
	// ⚠ 這裡解析失敗就整個檢查失敗,是刻意的:releases/latest 回了一個不是
	// semver 的 tag,代表有人把 data-<n> 發成了正式 release,那正是本次改版
	// 最高風險的那一格 —— 沉默地繼續會讓使用者以為自己是最新版。
	std::tuple<int, int, int> v;
	if (!parse_semver(rel->appVer, &v)) {
		if (err) *err = kMsgBadVersion + rel->appVer;
		return false;
	}
	return true;
}

bool AppUpdater::fetchDataRelease(RemoteRelease* rel, std::string* err)
{
	std::string body;
	{
		HttpsClient api(kApiHost);
		if (!api.GetString(kReleasesPath, body, err, &stop_)) return false;
	}
	try {
		ordered_json j = ordered_json::parse(body);
		if (!j.is_array()) {
			if (err) *err = kMsgBadReleaseList;
			return false;
		}
		for (const auto& r : j) {
			if (r.value("draft", false)) continue;
			long long seq = 0;
			const std::string tag = r.value("tag_name", std::string());
			if (!ParseDataTagSeq(tag, &seq)) continue;
			if (seq <= rel->dataSeq) continue; // 自己比大小,不信清單順序
			std::string url, sha, why;
			if (!pick_asset(r, kDataAssetPrefix, &url, &sha, &why)) {
				// 一個沒有資產的 data release 不該讓比它舊的那一個也失效
				if (!why.empty()) log_line(exeDir_, "data asset rejected: " + why);
				continue;
			}
			rel->dataSeq = seq;
			rel->dataTag = tag;
			rel->dataUrl = url;
			rel->dataSha = sha;
			rel->hasData = true;
		}
	} catch (...) {
		if (err) *err = kMsgBadReleaseList;
		return false;
	}
	// 「一個 data release 都還沒發」在過渡期是正常狀態,不是壞掉 —— 但它與
	// 「清單抓不到」在畫面上長得一模一樣(兩者都是什麼都沒有),所以回 false
	// 讓 doCheck 把原因寫進 log。呼叫端本來就把這條線當非致命處理。
	if (!rel->hasData) {
		if (err) *err = kMsgNoDataRelease;
		return false;
	}
	return true;
}

bool AppUpdater::doCheck(std::string* err)
{
	// A POB is running: it holds engine\*.dll open, and this function goes on to
	// overwrite Data\*.json with a fresh translation pack. Return without
	// recording the check time, so the next tick after the hold lifts retries.
	if (hold_.load()) return true;

	setPhase(AppUpdatePhase::Checking, kMsgChecking);

	// --- App 線(致命):失敗就整個檢查失敗,沿用舊行為 ---------------------
	RemoteRelease rel;
	if (!fetchAppRelease(&rel, err)) return false;

	// --- Data 線(非致命):翻譯拉不到,程式更新照常 -----------------------
	// 兩條線分開請求就是為了這一格。共用一次請求的話,翻譯線的任何毛病
	//(還沒發過 data release、清單暫時打不開)都會連坐擋掉程式更新。
	{
		std::string derr;
		if (!fetchDataRelease(&rel, &derr))
			log_line(exeDir_, "data line unavailable (non-fatal): " + derr);
	}

	std::tuple<int, int, int> remoteApp, localApp;
	parse_semver(rel.appVer, &remoteApp); // fetchAppRelease 已驗過
	parse_semver(POBTOOLS_VERSION_STRING, &localApp);

	const std::string localData = ReadLocalDataVersion(exeDir_);
	long long localDataSeq = -1;
	ParseDataTagSeq(localData, &localDataSeq); // 解不出來就維持 -1

	latest_ = rel;
	latestSeen_ = rel.appVer;
	{
		std::lock_guard<std::mutex> lk(stMx_);
		st_.latestAppVer = rel.appVer;
		st_.latestDataVer = rel.dataTag;
		st_.localDataVer = localData;
	}

	const UpdatePlan plan = PlanUpdates(rel.hasApp, remoteApp, localApp,
	                                    rel.hasData, rel.dataSeq, localDataSeq,
	                                    transUpdates_.load());

	// 翻譯線先跑(它會自己套用完),程式線接著**無條件**判斷。
	// ⚠ 這兩段以前是 if / else-if。當時 patch 與 minor 互斥所以安全,拆線後
	// 兩者可以同時成立(v0.19.0 與 data-N 同日發),而 else-if 會讓翻譯套用完
	// 之後**吞掉程式更新提示**,同時 lastCheckUtc 已落盤 → 沉默 24 小時。
	bool dataOk = true;
	if (plan.applyDataNow) {
		std::string terr;
		if (!doUpdateTranslations(&terr)) {
			// silent: old dictionaries stay intact, retried next launch
			// (lastCheckUtc is not persisted on this path)
			log_line(exeDir_, "translation update failed: " + terr);
			setPhase(AppUpdatePhase::Idle, "");
			dataOk = false;
		}
	} else if (plan.offerData) {
		// Opted out. Saying "up to date" here would be false, and leaving no way to
		// take the pack would mean toggling the setting back and forth -- so name
		// the version and let the UI offer a one-off apply.
		setPhase(AppUpdatePhase::TransAvailable,
		         MsgDataAvailable(rel.dataTag));
	}

	if (plan.promptApp) {
		setPhase(AppUpdatePhase::AppAvailable,
		         MsgAppAvailable(rel.appVer));
	} else {
		// 沒有程式更新時,不要把翻譯線剛設好的通知蓋掉;兩線都沒事才報最新版。
		std::lock_guard<std::mutex> lk(stMx_);
		if (st_.phase == AppUpdatePhase::Checking) {
			st_.phase = AppUpdatePhase::UpToDate;
			st_.message = MsgUpToDate();
		}
	}

	if (dataOk) lastCheckUtc_ = now_filetime();
	saveState();
	return true;
}

bool AppUpdater::doUpdateTranslations(std::string* err)
{
	setPhase(AppUpdatePhase::TransUpdating, MsgDataDownloading(latest_.dataTag));

	std::vector<unsigned char> buf;
	if (!downloadAsset(latest_.dataUrl, latest_.dataSha, &buf, err, false)) return false;

	// ⚠ 快取目錄名用各自的版號。兩線共用一個 "<ver>" 目錄時,一線的
	// remove_dir_rec 會把另一線正在用的 stage 一起刪掉。
	std::wstring cacheDir = exeDir_ + L"PobTools\\cache\\app_update\\" + widen(latest_.dataTag);
	std::wstring stage = cacheDir + L"\\trans_stage\\";
	remove_dir_rec(cacheDir);
	if (!ExtractZipToDir(buf.data(), buf.size(), stage, err)) return false;

	// pack sanity: dictionaries only — a mispackaged asset must not slip through
	if (!dir_exists(stage + L"Data") || file_exists(stage + L"pob-zh.exe") ||
	    dir_exists(stage + L"engine")) {
		if (err) *err = kMsgDataPackBad;
		remove_dir_rec(cacheDir);
		return false;
	}

	std::vector<std::wstring> rels;
	list_files_rec(stage, L"", &rels);
	const bool packHasStamp = file_exists(stage + kDataStampRel);
	if (!apply_content_two_pass(exeDir_, stage, rels, err)) {
		remove_dir_rec(cacheDir);
		return false;
	}
	// The stamp normally rides inside the pack and lands atomically with the
	// content it describes. A pack without one (hand-built by a translator) would
	// otherwise leave the install unstamped and redownload the same 4.4MB every
	// single day, so write it here as a fallback -- late, but bounded.
	if (!packHasStamp) {
		ordered_json v;
		v["dataVersion"] = latest_.dataTag;
		write_file_atomic(exeDir_ + kDataStampRel, v.dump(2));
		log_line(exeDir_, "pack carried no translations_version.json; stamped locally");
	}

	appliedTrans_ = latest_.dataTag; // informational only; the stamp is the truth
	remove_dir_rec(cacheDir);
	{
		std::lock_guard<std::mutex> lk(stMx_);
		st_.localDataVer = ReadLocalDataVersion(exeDir_);
		// 手動路徑可能沒經過 doCheck,latestDataVer 還是空的 —— UI 的
		// 「翻譯資料已更新至 」後面就會什麼都沒有。
		st_.latestDataVer = latest_.dataTag;
		st_.phase = AppUpdatePhase::TransDone;
		st_.message = MsgDataApplied(latest_.dataTag);
	}
	log_line(exeDir_, "translations updated to " + latest_.dataTag + " (" +
	                  std::to_string(rels.size()) + " files)");
	return true;
}

// stage layout sanity shared by the worker and the (re)validation in apply
static bool validate_app_stage(const std::wstring& stage, std::string* err)
{
	if (file_size(stage + L"pob-zh.exe") < (1ll << 20) ||
	    !file_exists(stage + L"engine\\SimpleGraphic.dll") ||
	    !file_exists(stage + L"engine\\glfw3.dll") ||
	    !file_exists(stage + L"engine\\libGLESv2.dll") ||
	    !dir_exists(stage + L"Data")) {
		if (err) *err = kMsgStageBad;
		return false;
	}
	return true;
}

bool AppUpdater::doUpdateApp(std::string* err)
{
	if (!latest_.hasApp) {
		if (err) *err = kMsgNoAppAsset;
		return false;
	}

	ULARGE_INTEGER freeBytes{};
	if (GetDiskFreeSpaceExW(exeDir_.c_str(), &freeBytes, nullptr, nullptr) &&
	    freeBytes.QuadPart < 300ull * 1024 * 1024) {
		if (err) *err = kMsgNoDiskSpace;
		return false;
	}

	{
		std::lock_guard<std::mutex> lk(stMx_);
		st_.phase = AppUpdatePhase::AppDownloading;
		st_.message = MsgAppDownloading(latest_.appVer);
		st_.bytesDone = st_.bytesTotal = 0;
	}
	std::vector<unsigned char> buf;
	if (!downloadAsset(latest_.appUrl, latest_.appSha, &buf, err, true)) return false;

	setPhase(AppUpdatePhase::AppStaging, kMsgStaging);
	std::wstring cacheDir = exeDir_ + L"PobTools\\cache\\app_update\\v" + widen(latest_.appVer);
	std::wstring stage = cacheDir + L"\\app_stage\\";
	remove_dir_rec(cacheDir);
	if (!ExtractZipToDir(buf.data(), buf.size(), stage, err)) return false;
	if (!validate_app_stage(stage, err)) return false;

	{
		std::lock_guard<std::mutex> lk(stMx_);
		st_.phase = AppUpdatePhase::AppReadyToApply;
		st_.message = kMsgReadyRestart;
		st_.applyPending = true;
		st_.stageDir = stage;
	}
	return true;
}

// ---- swap / cleanup -----------------------------------------------------------

static void delete_old_backups(const std::wstring& exeDir, int retries)
{
	auto tryDelete = [&](const std::wstring& p) {
		for (int i = 0; i < retries; i++) {
			SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
			if (DeleteFileW(p.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND) return;
			Sleep(200);
		}
	};
	if (file_exists(exeDir + L"pob-zh.exe.old")) tryDelete(exeDir + L"pob-zh.exe.old");
	WIN32_FIND_DATAW fd{};
	HANDLE h = FindFirstFileW((exeDir + L"engine\\*.old").c_str(), &fd);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
				tryDelete(exeDir + L"engine\\" + fd.cFileName);
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}
}

void CleanupAppUpdateLeftovers(const std::wstring& exeDir)
{
	delete_old_backups(exeDir, 5);
	if (dir_exists(exeDir + L"PobTools\\cache\\app_update"))
		remove_dir_rec(exeDir + L"PobTools\\cache\\app_update");
}

int ApplyStagedAppUpdateAndRelaunch(const std::wstring& exeDir, const std::wstring& stageDir,
                                    const std::string& tag, bool relaunch, std::string* errOut,
                                    bool includeTranslations)
{
	auto fail = [&](const std::string& m) {
		log_line(exeDir, "apply failed: " + m);
		if (errOut) *errOut = m;
		return 1;
	};

	std::wstring stage = stageDir;
	if (!stage.empty() && stage.back() != L'\\') stage += L'\\';

	// one apply at a time per install dir (two launcher instances)
	unsigned long long hash = 1469598103934665603ull; // FNV-1a over the lowered dir
	for (wchar_t c : exeDir) {
		wchar_t l = (c >= L'A' && c <= L'Z') ? c + 32 : c;
		hash = (hash ^ (unsigned long long)l) * 1099511628211ull;
	}
	wchar_t mname[64];
	swprintf_s(mname, L"Local\\PobTools-appswap-%016llx", hash);
	HANDLE mtx = CreateMutexW(nullptr, TRUE, mname);
	if (!mtx) return fail(kMsgLockFailed);
	if (GetLastError() == ERROR_ALREADY_EXISTS) {
		CloseHandle(mtx);
		return fail(kMsgOtherInstance);
	}

	int rc = 1;
	std::string msg;
	do {
		std::string verr;
		if (!validate_app_stage(stage, &verr)) { msg = verr; break; }

		std::vector<std::wstring> rels;
		list_files_rec(stage, L"", &rels);
		std::vector<std::wstring> content, boot;
		int skippedTrans = 0;
		for (const std::wstring& rel : rels) {
			if (rel == L"pob-zh.exe" || rel.compare(0, 7, L"engine\\") == 0) { boot.push_back(rel); continue; }
			// Normally a no-op since v0.19.0: PobTools-update-<ver>.zip carries no
			// dictionaries at all. Kept as the last line of defence for the two
			// cases where the stage DOES hold them -- someone pointed at the full
			// zip, or packaging leaked a dictionary into the app pack.
			if (!includeTranslations && IsTranslationDataRel(rel)) { skippedTrans++; continue; }
			content.push_back(rel);
		}
		if (skippedTrans)
			log_line(exeDir, "translation updates off: kept " + std::to_string(skippedTrans) +
			                 " existing dictionary file(s)");

		delete_old_backups(exeDir, 1);

		// content set (Data\, Fonts\, docs): a failure here leaves exe +
		// engine untouched (never-downgrade)
		if (!apply_content_two_pass(exeDir, stage, content, &msg)) break;

		// bootable set (exe + engine DLLs): back up as *.old, then move the
		// staged replacement in (same volume = atomic rename). Renaming works
		// on the running exe and on loaded DLL images.
		struct Rb { std::wstring dst; bool backedUp = false; bool placed = false; };
		std::vector<Rb> rb;
		bool ok = true;
		for (const std::wstring& rel : boot) {
			Rb r;
			r.dst = exeDir + rel;
			if (file_exists(r.dst)) {
				if (!MoveFileExW(r.dst.c_str(), (r.dst + L".old").c_str(), MOVEFILE_REPLACE_EXISTING)) {
					msg = kMsgBackupFailed + narrow(rel);
					ok = false;
					break;
				}
				r.backedUp = true;
			}
			rb.push_back(r);
		}
		if (ok) {
			for (size_t i = 0; i < boot.size(); i++) {
				ensure_parent_dir(exeDir + boot[i]);
				bool placed = false;
				for (int t = 0; t < 3 && !placed; t++) { // Defender can pin fresh files briefly
					placed = MoveFileExW((stage + boot[i]).c_str(), (exeDir + boot[i]).c_str(),
					                     MOVEFILE_REPLACE_EXISTING) != 0;
					if (!placed) Sleep(300);
				}
				if (!placed) {
					msg = kMsgPlaceFailed + narrow(boot[i]);
					ok = false;
					break;
				}
				rb[i].placed = true;
			}
		}
		if (!ok) {
			for (auto it = rb.rbegin(); it != rb.rend(); ++it) {
				if (it->placed) DeleteFileW(it->dst.c_str());
				if (it->backedUp)
					MoveFileExW((it->dst + L".old").c_str(), it->dst.c_str(), MOVEFILE_REPLACE_EXISTING);
			}
			msg += kMsgRolledBack;
			break;
		}

		// informational record; the new exe's compile-time constant is the truth
		{
			std::string content2;
			ordered_json v;
			if (read_file_utf8(exeDir + L"PobTools\\update_state.json", content2)) {
				try { v = ordered_json::parse(content2); } catch (...) { v = ordered_json(); }
			}
			v["appliedApp"] = tag;
			// ⚠ 這裡以前會在 includeTranslations 時寫 appliedTranslations = <程式版號>。
			// 拆兩線之後那是錯的:程式版號與 data-<n> 不同號,寫進去只會污染一個
			// 現在純資訊性的欄位。翻譯資料的真值是 Data\translations_version.json,
			// 它跟著內容一起被搬,不需要也不該由這裡代寫。
			CreateDirectoryW((exeDir + L"PobTools").c_str(), nullptr);
			write_file_atomic(exeDir + L"PobTools\\update_state.json", v.dump(2));
		}
		log_line(exeDir, "app updated to v" + tag);

		if (relaunch) {
			std::wstring exe = exeDir + L"pob-zh.exe";
			std::vector<wchar_t> cmd(exe.size() + 3);
			swprintf_s(cmd.data(), cmd.size(), L"\"%s\"", exe.c_str());
			STARTUPINFOW si{};
			si.cb = sizeof(si);
			PROCESS_INFORMATION pi{};
			if (CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
			                   exeDir.c_str(), &si, &pi)) {
				CloseHandle(pi.hThread);
				CloseHandle(pi.hProcess);
			}
		}
		rc = 0;
	} while (false);

	ReleaseMutex(mtx);
	CloseHandle(mtx);
	if (rc != 0) return fail(msg.empty() ? kMsgApplyFailed : msg);
	return 0;
}

// ---- CLI wrappers ---------------------------------------------------------------

static void attach_parent_console()
{
	if (AttachConsole(ATTACH_PARENT_PROCESS)) {
		FILE* f = nullptr;
		freopen_s(&f, "CONOUT$", "w", stdout);
	}
}

int RunAppUpdateCli(const std::wstring& exeDir, bool checkOnly)
{
	attach_parent_console();

	AppUpdater u;
	u.exeDir_ = exeDir;
	u.loadState();
	// The CLI has to honour the same opt-out as the UI, or "update from a script"
	// becomes the one path that still overwrites edited dictionaries.
	const bool wantTrans = LoadLauncherConfig(exeDir + L"pob-zh.ini").updateTranslations;
	u.SetTranslationUpdates(wantTrans);
	{
		std::lock_guard<std::mutex> lk(u.stMx_);
		u.st_.localVer = POBTOOLS_VERSION_STRING;
	}

	std::string err;
	if (!u.doCheck(&err)) {
		printf("FAIL: %s\n", err.c_str());
		log_line(exeDir, std::string("cli check failed: ") + err);
		return 1;
	}
	AppUpdater::Status st = u.Poll();
	// 兩個版號都印。這是舊客戶端相容性驗證(拿 v0.18.0 的 exe 跑 --app-update-check)
	// 唯一的肉眼證據來源,所以程式版與資料版必須分行看得見。
	printf("app:  local v%s, remote v%s\n", POBTOOLS_VERSION_STRING, st.latestAppVer.c_str());
	printf("data: local %s, remote %s\n",
	       st.localDataVer.empty() ? "(unstamped)" : st.localDataVer.c_str(),
	       st.latestDataVer.empty() ? "(none)" : st.latestDataVer.c_str());
	printf("%s\n", st.message.c_str());
	log_line(exeDir, "cli check: app local v" POBTOOLS_VERSION_STRING ", remote v" +
	                 st.latestAppVer + "; data local " + st.localDataVer + ", remote " +
	                 st.latestDataVer);
	if (checkOnly || st.phase != AppUpdatePhase::AppAvailable) return 0;

	if (!u.doUpdateApp(&err)) {
		printf("FAIL: %s\n", err.c_str());
		return 1;
	}
	st = u.Poll();
	std::string aerr;
	if (ApplyStagedAppUpdateAndRelaunch(exeDir, st.stageDir, st.latestAppVer, false, &aerr,
	                                    wantTrans) != 0) {
		printf("FAIL: %s\n", aerr.c_str());
		return 1;
	}
	printf("updated to v%s - restart PobTools to finish\n", st.latestAppVer.c_str());
	return 0;
}

int RunTranslationDataList(const std::wstring& dir, const std::wstring& outFile)
{
	attach_parent_console();

	std::wstring root = dir;
	if (!root.empty() && root.back() != L'\\') root += L'\\';
	if (!dir_exists(root)) {
		printf("FAIL: not a directory: %s\n", narrow(root).c_str());
		return 1;
	}

	std::vector<std::wstring> all;
	list_files_rec(root, L"", &all);

	std::vector<std::wstring> hits;
	for (const std::wstring& rel : all)
		if (IsTranslationDataRel(rel)) hits.push_back(rel);
	// Deterministic order: packaging diffs this output against the zip entries,
	// and FindFirstFileW's order is not something to build an assertion on.
	std::sort(hits.begin(), hits.end());

	std::string text;
	for (const std::wstring& rel : hits) text += narrow(rel) + "\n";

	fwrite(text.data(), 1, text.size(), stdout);
	if (!outFile.empty() && !write_file_bytes(outFile, text.data(), text.size())) {
		printf("FAIL: cannot write %s\n", narrow(outFile).c_str());
		return 1;
	}
	// 一個檔都沒有 = 規則沒接上或指錯目錄。舊打包腳本的 glob 打錯字會靜默產出
	// 空 zip 並一路成功,那個洞在這裡就堵掉。
	if (hits.empty()) {
		printf("FAIL: no translation data under %s\n", narrow(root).c_str());
		return 1;
	}
	return 0;
}

// Hidden helper for the one-time redirect verification: downloads the newest
// data pack (github.com -> objects.githubusercontent.com 302) and reports
// whether the sha256 digest matched. Applies nothing.
int RunAppFetchTest(const std::wstring& exeDir)
{
	attach_parent_console();
	AppUpdater u;
	u.exeDir_ = exeDir;
	u.loadState();
	std::string err;
	if (!u.doCheck(&err)) {
		printf("FAIL check: %s\n", err.c_str());
		return 1;
	}
	if (!u.latest_.hasData) {
		printf("FAIL: no data-<n> release with a %s* asset\n", kDataAssetPrefix);
		return 1;
	}
	std::vector<unsigned char> buf;
	if (!u.downloadAsset(u.latest_.dataUrl, u.latest_.dataSha, &buf, &err, false)) {
		printf("FAIL fetch: %s\n", err.c_str());
		return 1;
	}
	printf("OK: fetched %zu bytes, sha256 verified (redirect followed)\n", buf.size());
	return 0;
}

// ---- self test ------------------------------------------------------------------

// Builds a one-entry zip whose stored name is patched to arbitrary bytes
// (miniz's writer would reject hostile names, which is exactly what we want
// to smuggle past the extractor under test).
static std::vector<unsigned char> make_zip_single(const std::string& entryName,
                                                  const std::string& content)
{
	std::string placeholder(entryName.size(), 'q');
	mz_zip_archive zw{};
	std::vector<unsigned char> out;
	if (!mz_zip_writer_init_heap(&zw, 0, 0)) return out;
	if (mz_zip_writer_add_mem(&zw, placeholder.c_str(), content.data(), content.size(),
	                          MZ_NO_COMPRESSION)) {
		void* p = nullptr;
		size_t n = 0;
		if (mz_zip_writer_finalize_heap_archive(&zw, &p, &n)) {
			out.assign((unsigned char*)p, (unsigned char*)p + n);
			mz_free(p);
		}
	}
	mz_zip_writer_end(&zw);
	// patch every occurrence (local header + central directory)
	if (!out.empty() && !placeholder.empty()) {
		for (size_t i = 0; i + placeholder.size() <= out.size(); i++) {
			if (memcmp(out.data() + i, placeholder.data(), placeholder.size()) == 0)
				memcpy(out.data() + i, entryName.data(), entryName.size());
		}
	}
	return out;
}

int RunAppUpdateSelfTest(const std::wstring& exeDir)
{
	attach_parent_console();

	std::string report;
	int fails = 0;
	auto check = [&](bool ok, const char* name) {
		report += ok ? "PASS " : "FAIL ";
		report += name;
		report += "\r\n";
		printf("%s %s\n", ok ? "PASS" : "FAIL", name);
		if (!ok) fails++;
	};

	std::wstring root = exeDir + L"PobTools\\selftest_appupd";
	remove_dir_rec(root);
	SHCreateDirectoryExW(nullptr, root.c_str(), nullptr);

	// T1: extraction normalizes backslash entry names (Compress-Archive zips)
	{
		std::vector<unsigned char> z = make_zip_single("Data\\a\\b.json", "x");
		std::wstring dest = root + L"\\t1\\";
		std::string err, got;
		int files = 0;
		bool ok = !z.empty() && ExtractZipToDir(z.data(), z.size(), dest, &err, &files) &&
		          files == 1 && read_file_utf8(dest + L"Data\\a\\b.json", got) && got == "x";
		check(ok, "T1 zip extract normalizes backslash entries");
	}

	// T2: zip-slip attempts must all be rejected
	{
		const char* bad[] = { "../evil.txt", "/abs.txt", "C:\\abs.txt", "a/../../evil.txt" };
		bool allRejected = true;
		for (const char* name : bad) {
			std::vector<unsigned char> z = make_zip_single(name, "x");
			std::string err;
			if (z.empty() || ExtractZipToDir(z.data(), z.size(), root + L"\\t2\\", &err))
				allRejected = false;
		}
		check(allRejected, "T2 zip-slip entry names rejected");
	}

	// T3: SHA-256 known vector
	{
		std::string hex;
		bool ok = Sha256Hex("abc", 3, &hex) &&
		          hex == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
		check(ok, "T3 sha256 test vector");
	}

	// T4: the two-line update plan.
	//
	// ⚠ This test used to assert the OPPOSITE rule -- "patch bump = silent
	// data-only release". That rule died with the split (a patch bump now means a
	// program pack that renames pob-zh.exe and restarts the process), and a test
	// left asserting it would have gone on giving a green light to the wrong
	// behaviour. The cases that matter most are the last two: both lines updating
	// at once, which the old if/else-if silently collapsed into one.
	{
		auto v = [](int a, int b, int c) { return std::tuple<int, int, int>{a, b, c}; };
		const auto v0190 = v(0, 19, 0);
		UpdatePlan p;
		bool ok = true;
		std::string bad;
		auto want = [&](const char* what, const UpdatePlan& got, bool app, bool applyD, bool offerD) {
			if (got.promptApp == app && got.applyDataNow == applyD && got.offerData == offerD) return;
			ok = false;
			bad += std::string(" ") + what;
		};

		// --- App 線 -------------------------------------------------------
		// every bump prompts now, patch included
		want("patch-prompts", PlanUpdates(true, v(0, 19, 1), v0190, false, -1, -1, true),
		     true, false, false);
		want("minor-prompts", PlanUpdates(true, v(0, 20, 0), v0190, false, -1, -1, true),
		     true, false, false);
		want("major-prompts", PlanUpdates(true, v(1, 0, 0), v0190, false, -1, -1, true),
		     true, false, false);
		want("equal-quiet", PlanUpdates(true, v0190, v0190, false, -1, -1, true),
		     false, false, false);
		want("never-downgrade", PlanUpdates(true, v(0, 18, 0), v0190, false, -1, -1, true),
		     false, false, false);
		// a release without the update asset must not offer an update it cannot do
		want("no-asset-no-prompt", PlanUpdates(false, v(0, 20, 0), v0190, false, -1, -1, true),
		     false, false, false);

		// --- Data 線 ------------------------------------------------------
		want("data-newer-auto", PlanUpdates(true, v0190, v0190, true, 3, 2, true),
		     false, true, false);
		want("data-newer-manual", PlanUpdates(true, v0190, v0190, true, 3, 2, false),
		     false, false, true);
		want("data-same", PlanUpdates(true, v0190, v0190, true, 3, 3, true),
		     false, false, false);
		// 遠端比本機舊(有人把 data-2 重發成 latest)絕不倒退
		want("data-older", PlanUpdates(true, v0190, v0190, true, 2, 3, true),
		     false, false, false);
		// 全新安裝/v0.18.0 升上來:沒有戳記 = -1,拿一次就好
		want("data-unstamped", PlanUpdates(true, v0190, v0190, true, 1, -1, true),
		     false, true, false);
		want("data-no-asset", PlanUpdates(true, v0190, v0190, false, -1, -1, true),
		     false, false, false);

		// --- 兩線同時 ------------------------------------------------------
		// 這才是 doCheck 的 if/else-if 會吃掉東西的那一格
		want("both-auto", PlanUpdates(true, v(0, 20, 0), v0190, true, 4, 3, true),
		     true, true, false);
		want("both-manual", PlanUpdates(true, v(0, 20, 0), v0190, true, 4, 3, false),
		     true, false, true);
		check(ok, ("T4 two-line update plan" + (ok ? std::string() : (" -- wrong:" + bad))).c_str());
	}

	// T5: state record round-trip + corrupt file reads as defaults
	{
		std::wstring stRoot = root + L"\\t5\\";
		SHCreateDirectoryExW(nullptr, stRoot.c_str(), nullptr);
		AppUpdater a;
		a.exeDir_ = stRoot;
		a.appliedTrans_ = "1.2.3";
		a.appliedApp_ = "1.2.0";
		a.latestSeen_ = "1.2.3";
		a.lastCheckUtc_ = 42;
		a.saveState();
		AppUpdater b;
		b.exeDir_ = stRoot;
		b.loadState();
		bool ok = b.appliedTrans_ == "1.2.3" && b.appliedApp_ == "1.2.0" &&
		          b.latestSeen_ == "1.2.3" && b.lastCheckUtc_ == 42;
		write_file_bytes(stRoot + L"PobTools\\update_state.json", "{corrupt", 8);
		b.loadState();
		ok = ok && b.appliedTrans_.empty() && b.lastCheckUtc_ == 0;
		check(ok, "T5 update_state.json round-trip and corrupt fallback");
	}

	// helpers for T6/T7: fake install root + staged replacement
	auto writeBig = [&](const std::wstring& p, const char* tagStr) {
		std::string big(1200 * 1024, 'B');
		memcpy(&big[0], tagStr, strlen(tagStr));
		ensure_parent_dir(p);
		return write_file_bytes(p, big.data(), big.size());
	};
	auto writeSmall = [&](const std::wstring& p, const char* s) {
		ensure_parent_dir(p);
		return write_file_bytes(p, s, strlen(s));
	};
	auto readPrefix = [&](const std::wstring& p) {
		std::string c;
		if (!read_file_utf8(p, c)) return std::string();
		return c.substr(0, 3);
	};
	auto setupInstall = [&](const std::wstring& inst, const std::wstring& stage) {
		bool ok = writeBig(inst + L"pob-zh.exe", "OLD");
		ok = ok && writeSmall(inst + L"engine\\SimpleGraphic.dll", "OLD");
		ok = ok && writeSmall(inst + L"engine\\glfw3.dll", "OLD");
		ok = ok && writeSmall(inst + L"engine\\libGLESv2.dll", "OLD");
		ok = ok && writeSmall(inst + L"Data\\dict.json", "OLD");
		ok = ok && writeBig(stage + L"pob-zh.exe", "NEW");
		ok = ok && writeSmall(stage + L"engine\\SimpleGraphic.dll", "NEW");
		ok = ok && writeSmall(stage + L"engine\\glfw3.dll", "NEW");
		ok = ok && writeSmall(stage + L"engine\\libGLESv2.dll", "NEW");
		ok = ok && writeSmall(stage + L"Data\\dict.json", "NEW");
		return ok;
	};

	// T6: full swap succeeds; install carries NEW, backups carry OLD
	{
		std::wstring inst = root + L"\\t6\\inst\\";
		std::wstring stage = root + L"\\t6\\stage\\";
		std::string aerr;
		bool ok = setupInstall(inst, stage) &&
		          ApplyStagedAppUpdateAndRelaunch(inst, stage, "9.9.9", false, &aerr) == 0;
		ok = ok && readPrefix(inst + L"pob-zh.exe") == "NEW" &&
		     readPrefix(inst + L"engine\\SimpleGraphic.dll") == "NEW" &&
		     readPrefix(inst + L"Data\\dict.json") == "NEW" &&
		     readPrefix(inst + L"pob-zh.exe.old") == "OLD";
		std::string stateRaw;
		ok = ok && read_file_utf8(inst + L"PobTools\\update_state.json", stateRaw) &&
		     stateRaw.find("9.9.9") != std::string::npos;
		check(ok, "T6 staged swap applies and backs up boot files");
	}

	// T7: a mid-swap failure (locked staged DLL) rolls the boot set back
	{
		std::wstring inst = root + L"\\t7\\inst\\";
		std::wstring stage = root + L"\\t7\\stage\\";
		bool ok = setupInstall(inst, stage);
		// exclusive handle: MoveFileExW on the staged source fails (sharing violation)
		HANDLE lock = CreateFileW((stage + L"engine\\libGLESv2.dll").c_str(), GENERIC_READ, 0,
		                          nullptr, OPEN_EXISTING, 0, nullptr);
		std::string aerr;
		ok = ok && lock != INVALID_HANDLE_VALUE &&
		     ApplyStagedAppUpdateAndRelaunch(inst, stage, "9.9.9", false, &aerr) != 0;
		if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
		ok = ok && readPrefix(inst + L"pob-zh.exe") == "OLD" &&
		     readPrefix(inst + L"engine\\SimpleGraphic.dll") == "OLD" &&
		     readPrefix(inst + L"engine\\glfw3.dll") == "OLD" &&
		     readPrefix(inst + L"engine\\libGLESv2.dll") == "OLD";
		check(ok, "T7 mid-swap failure rolls boot files back");
	}

	// T8: what counts as translation data -- now the line between the two release
	// zips, so BOTH directions are damaging. A file wrongly called translation
	// data disappears from the app pack (nobody who only takes program updates
	// ever gets it again); a file wrongly called app data is packed into the app
	// zip and every program update overwrites the translator's copy.
	{
		struct Case { const wchar_t* rel; bool want; };
		const Case cases[] = {
			// the dictionaries, including the launcher's own labels
			{ L"Data\\poe1\\zh-rTW\\ui.json",                  true  },
			{ L"Data\\poe2\\zh-rTW\\meta.json",                true  },
			{ L"Data\\launcher\\zh-rTW\\launcher.json",        true  },
			{ L"data\\POE1\\zh-rTW\\Stats.JSON",               true  }, // case-insensitive
			{ L"Data/poe1/zh-rTW/items.json",                  true  }, // forward slashes
			// a locale nobody has shipped yet: the rule is "one more directory
			// level", never a list of known locales, so ko is covered for free.
			// Hardcoding zh-rTW anywhere is how a new language silently fails to
			// be packaged at all.
			{ L"Data\\poe1\\ko\\ui.json",                      true  },
			{ L"Data\\launcher\\ko\\launcher.json",            true  },
			{ L"Data\\filter_items_zh.json",                   true  },
			{ L"Data\\item_meta.json",                         true  },
			{ L"Data\\item_classes_zh.json",                   true  },
			{ L"Data\\translations_version.json",              true  }, // the stamp rides with them
			// near misses that must stay in the app pack
			{ L"Data\\poe1x\\zh-rTW\\ui.json",                 false }, // prefix trap
			{ L"Data\\poe1\\zh-rTW\\notes.txt",                false }, // not a dictionary
			{ L"Data\\poe1\\ui.json",                          false }, // missing locale level
			{ L"Data\\poe1\\zh-rTW\\sub\\ui.json",             false }, // one level too deep
			// ⚠ atlas_tree_zh.json moved OUT of the translation set in v0.19.0: a
			// season folder needs all three of its files at once, so splitting it
			// across the two zips makes a new league invisible to anyone who only
			// took the app pack. This case is the one guarding that decision.
			{ L"Data\\atlas_versions\\3.29\\atlas_tree_zh.json",   false },
			{ L"Data\\atlas_versions\\3.29\\atlas_tree_poe1.json", false },
			{ L"Data\\atlas_index.json",                       false },
			{ L"Fonts\\NotoSansTC-Regular.ttf",                false },
			{ L"pob-zh.exe",                                   false },
			{ L"engine\\SimpleGraphic.dll",                    false },
			{ L"Datafile.json",                                false }, // "Data" without a separator
		};
		bool ok = true;
		std::string bad;
		for (const Case& c : cases) {
			if (IsTranslationDataRel(c.rel) == c.want) continue;
			ok = false;
			bad += " " + narrow(c.rel);
		}
		check(ok, ("T8 IsTranslationDataRel classifies the pack contents" +
		           (ok ? std::string() : (" -- wrong:" + bad))).c_str());
	}

	// T9: with translation updates off, an app update still swaps the exe and
	// engine but leaves the dictionaries exactly as they were. This is the whole
	// point of the split, so it is asserted on real files, not on a flag.
	{
		std::wstring inst = root + L"\\t9\\inst\\";
		std::wstring stage = root + L"\\t9\\stage\\";
		bool ok = setupInstall(inst, stage);
		// dict.json sits directly under Data\ and is NOT translation data; add one
		// that is, plus a non-dictionary Data file that must still be updated.
		ok = ok && writeSmall(inst + L"Data\\poe1\\zh-rTW\\ui.json", "OLD");
		ok = ok && writeSmall(stage + L"Data\\poe1\\zh-rTW\\ui.json", "NEW");
		ok = ok && writeSmall(inst + L"Data\\atlas_index.json", "OLD");
		ok = ok && writeSmall(stage + L"Data\\atlas_index.json", "NEW");
		std::string aerr;
		ok = ok && ApplyStagedAppUpdateAndRelaunch(inst, stage, "9.9.9", false, &aerr,
		                                           /*includeTranslations=*/false) == 0;
		ok = ok && readPrefix(inst + L"pob-zh.exe") == "NEW" &&
		     readPrefix(inst + L"engine\\SimpleGraphic.dll") == "NEW" &&
		     readPrefix(inst + L"Data\\atlas_index.json") == "NEW" &&
		     readPrefix(inst + L"Data\\poe1\\zh-rTW\\ui.json") == "OLD";
		// The app swap must not touch the data version at all -- in either
		// direction. It used to write appliedTranslations = <app tag>, which after
		// the split is a value from the wrong numbering scheme entirely.
		std::string stateRaw;
		ok = ok && read_file_utf8(inst + L"PobTools\\update_state.json", stateRaw) &&
		     stateRaw.find("appliedTranslations") == std::string::npos;
		check(ok, "T9 app update with translations off keeps the dictionaries");
	}

	// T10: the same run with the setting ON must replace them -- otherwise T9
	// would pass even if the dictionaries were never updatable at all.
	{
		std::wstring inst = root + L"\\t10\\inst\\";
		std::wstring stage = root + L"\\t10\\stage\\";
		bool ok = setupInstall(inst, stage);
		ok = ok && writeSmall(inst + L"Data\\poe1\\zh-rTW\\ui.json", "OLD");
		ok = ok && writeSmall(stage + L"Data\\poe1\\zh-rTW\\ui.json", "NEW");
		std::string aerr;
		ok = ok && ApplyStagedAppUpdateAndRelaunch(inst, stage, "9.9.9", false, &aerr,
		                                           /*includeTranslations=*/true) == 0;
		ok = ok && readPrefix(inst + L"Data\\poe1\\zh-rTW\\ui.json") == "NEW";
		// Even here -- dictionaries genuinely replaced -- the app tag must not be
		// recorded as a data version. The stamp file is the only truth.
		std::string stateRaw;
		ok = ok && read_file_utf8(inst + L"PobTools\\update_state.json", stateRaw) &&
		     stateRaw.find("appliedTranslations") == std::string::npos;
		check(ok, "T10 app update with translations on replaces the dictionaries");
	}

	// T11: the data version stamp. The whole Data 線 rests on this one number, and
	// every way of failing to read it has to land on "unstamped" (-1) rather than
	// on "current" -- guessing current strands an install on stale dictionaries
	// with nothing to show for it.
	{
		std::wstring inst = root + L"\\t11\\inst\\";
		SHCreateDirectoryExW(nullptr, (inst + L"Data").c_str(), nullptr);
		bool ok = true;
		std::string bad;
		auto seqOf = [](const char* tag) {
			long long n = -1;
			return ParseDataTagSeq(tag, &n) ? n : -1;
		};
		// tag parsing: an app tag must never be mistaken for a data tag
		if (seqOf("data-3") != 3) { ok = false; bad += " data-3"; }
		if (seqOf("data-0") != 0) { ok = false; bad += " data-0"; }
		if (seqOf("data-12") != 12) { ok = false; bad += " data-12"; }
		if (seqOf("v0.19.0") != -1) { ok = false; bad += " v0.19.0"; }
		if (seqOf("data-") != -1) { ok = false; bad += " data-"; }
		if (seqOf("data-1a") != -1) { ok = false; bad += " data-1a"; }
		if (seqOf("Data-1") != -1) { ok = false; bad += " Data-1"; }
		// missing file
		if (!ReadLocalDataVersion(inst).empty()) { ok = false; bad += " missing"; }
		// present
		writeSmall(inst + kDataStampRel, "{\"dataVersion\":\"data-7\"}");
		if (ReadLocalDataVersion(inst) != "data-7") { ok = false; bad += " present"; }
		// corrupt reads as unstamped, not as current
		writeSmall(inst + kDataStampRel, "{not json");
		if (!ReadLocalDataVersion(inst).empty()) { ok = false; bad += " corrupt"; }
		check(ok, ("T11 data version stamp round-trip" +
		           (ok ? std::string() : (" -- wrong:" + bad))).c_str());
	}

	// T12: --translation-data-list is the contract packaging splits the zips on,
	// so it is exercised against a tree rather than trusted. Asserting the exact
	// text (not just a count) is deliberate: packaging compares this output to the
	// zip entry names, so separator and ordering are part of the contract.
	{
		std::wstring src = root + L"\\t12\\dist\\";
		bool ok = writeSmall(src + L"Data\\poe1\\zh-rTW\\ui.json", "x");
		ok = ok && writeSmall(src + L"Data\\poe1\\ko\\ui.json", "x");
		ok = ok && writeSmall(src + L"Data\\launcher\\zh-rTW\\launcher.json", "x");
		ok = ok && writeSmall(src + L"Data\\translations_version.json", "x");
		ok = ok && writeSmall(src + L"Data\\item_meta.json", "x");
		// must NOT be listed
		ok = ok && writeSmall(src + L"Data\\atlas_versions\\3.29.0\\atlas_tree_zh.json", "x");
		ok = ok && writeSmall(src + L"Data\\atlas_index.json", "x");
		ok = ok && writeSmall(src + L"Data\\poe1\\zh-rTW\\glossary.md", "x");
		ok = ok && writeBig(src + L"pob-zh.exe", "x");

		std::wstring outFile = root + L"\\t12\\list.txt";
		ok = ok && RunTranslationDataList(src, outFile) == 0;
		std::string got;
		ok = ok && read_file_utf8(outFile, got);
		const std::string wantText =
			"Data\\item_meta.json\n"
			"Data\\launcher\\zh-rTW\\launcher.json\n"
			"Data\\poe1\\ko\\ui.json\n"
			"Data\\poe1\\zh-rTW\\ui.json\n"
			"Data\\translations_version.json\n";
		ok = ok && got == wantText;
		// an empty result is a packaging bug, not a valid answer
		std::wstring empty = root + L"\\t12\\empty\\";
		SHCreateDirectoryExW(nullptr, empty.c_str(), nullptr);
		ok = ok && RunTranslationDataList(empty, L"") != 0;
		ok = ok && RunTranslationDataList(root + L"\\t12\\nope\\", L"") != 0;
		check(ok, ("T12 --translation-data-list walks the tree through the same rule" +
		           (ok ? std::string() : (" -- got:\n" + got))).c_str());
	}

	// T13: every character the updater can put on screen must be in the glyph
	// seed. This is the half that was missing for a whole release.
	//
	// --font-coverage-selftest已經在吃 kAppUpdateGlyphSeed,而且一直是綠的 -- but it
	// asks "can the fonts draw what is IN the seed", never "is what the updater
	// SAYS in the seed". So a phrase that was never seeded (「請先關閉所有 POB
	// 視窗」 shipped that way in v0.18.0) is invisible to it by construction: the
	// characters it would have complained about were never handed to it.
	//
	// This test closes the loop from the other end -- it drives the real message
	// formatters with fake data and checks the produced text, so the failure it
	// catches is "someone wrote a message with a character nobody seeded".
	{
		// UTF-8 -> codepoints. Not a validating decoder: this only has to answer
		// "which characters appear", and the input is our own literals.
		auto codepoints = [](const std::string& s, std::vector<unsigned>* out) {
			for (size_t i = 0; i < s.size();) {
				const unsigned char c = (unsigned char)s[i];
				unsigned cp = c;
				int extra = 0;
				if (c >= 0xF0) { cp = c & 0x07u; extra = 3; }
				else if (c >= 0xE0) { cp = c & 0x0Fu; extra = 2; }
				else if (c >= 0xC0) { cp = c & 0x1Fu; extra = 1; }
				i++;
				for (int k = 0; k < extra && i < s.size(); k++, i++)
					cp = (cp << 6) | ((unsigned char)s[i] & 0x3Fu);
				out->push_back(cp);
			}
		};
		std::vector<unsigned> seedCps;
		codepoints(kAppUpdateGlyphSeed, &seedCps);
		std::sort(seedCps.begin(), seedCps.end());

		auto firstMissing = [&](const std::string& msg, unsigned* missing) {
			std::vector<unsigned> cps;
			codepoints(msg, &cps);
			for (unsigned cp : cps) {
				if (cp < 0x80) continue; // ASCII: every font has it
				if (!std::binary_search(seedCps.begin(), seedCps.end(), cp)) {
					if (missing) *missing = cp;
					return true;
				}
			}
			return false;
		};
		auto hex = [](unsigned cp) {
			char buf[16];
			sprintf_s(buf, "U+%04X", cp);
			return std::string(buf);
		};

		int checked = 0;
		bool ok = true;
		std::string bad;
		auto expectCovered = [&](const std::string& msg) {
			checked++;
			unsigned miss = 0;
			if (firstMissing(msg, &miss)) {
				ok = false;
				bad += " " + hex(miss) + "(" + msg + ")";
			}
		};

		for (const char* frag : kUpdMsgFragments) expectCovered(frag);
		expectCovered(kExternalMsgSeed);
		// The composed messages, driven with stand-in版號/標籤 -- these are what a
		// user actually sees, and the only place a stray literal could hide.
		expectCovered(MsgUpToDate());
		expectCovered(MsgAppAvailable("9.9.9"));
		expectCovered(MsgAppDownloading("9.9.9"));
		expectCovered(MsgDataAvailable("data-42"));
		expectCovered(MsgDataDownloading("data-42"));
		expectCovered(MsgDataApplied("data-42"));

		// ⚠ Does this check have teeth? Everything above is seeded by
		// construction (the seed is generated from the same list), so all of it
		// would also pass if firstMissing() were broken and always returned false.
		// A character deliberately left out of the list must be detected.
		unsigned probeMiss = 0;
		const bool probeCaught = firstMissing(u8"Ω", &probeMiss) && probeMiss == 0x03A9;
		if (!probeCaught) { ok = false; bad += " probe-not-detected"; }
		if (checked == 0) { ok = false; bad += " nothing-checked"; }

		check(ok, ("T13 every updater message character is in the glyph seed (" +
		           std::to_string(checked) + " messages)" +
		           (ok ? std::string() : (" -- missing:" + bad))).c_str());
	}

	remove_dir_rec(root);

	report += fails == 0 ? "ALL PASS\r\n" : "FAILURES PRESENT\r\n";
	CreateDirectoryW((exeDir + L"PobTools").c_str(), nullptr);
	write_file_bytes(exeDir + L"PobTools\\app_update_selftest.txt", report.data(), report.size());
	printf("%s (report: PobTools\\app_update_selftest.txt)\n",
	       fails == 0 ? "ALL PASS" : "FAILURES PRESENT");
	return fails == 0 ? 0 : 1;
}
