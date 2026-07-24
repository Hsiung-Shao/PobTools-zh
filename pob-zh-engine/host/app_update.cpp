#include "app_update.h"
#include "app_version.h"
#include "http_client.h"
#include "zip_extract.h"
#include "hash_sha256.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>

#include <json.hpp> // nlohmann::ordered_json (deps/nlohmann)
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <vector>

#include <miniz.h> // self test builds in-memory archives

using nlohmann::ordered_json;

#pragma comment(lib, "shell32.lib")

// ---- release source -------------------------------------------------------

static const wchar_t* kApiHost = L"api.github.com";
static const wchar_t* kLatestPath = L"/repos/Hsiung-Shao/PobTools-zh/releases/latest";

// Every CJK literal the updater can surface in Status.message (including the
// ones bubbled up from HttpsClient / ExtractZipToDir), concatenated so the
// launcher can seed its glyph atlas. Keep in sync when adding messages.
const char* kAppUpdateGlyphSeed =
	u8"檢查更新中…已是最新版發現新翻譯資料已更新至（引擎下次啟動生效）目前版本下載主體更新中"
	u8"解壓與驗證更新檔就緒，即將重新啟動雜湊值不符（下載損毀？）磁碟空間不足（需 300MB）"
	u8"找不到對應的發佈資產暫存檔驗證失敗（可能被防毒隔離）另一個實例正在套用內容檔複製替換失敗"
	u8"備份放置回滾已還原舊版無法建立解壓目錄格式無效條目資訊讀取路徑非法檔案寫入"
	u8"已取消回應為空連線失敗（網路無法使用？）建立 HTTP 請求 HTTPS 初始化版本記錄解析"
	u8"發佈資產網址無效發佈版號無法解析翻譯資料包內容驗證失敗無法建立更新鎖";

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
			if (err) *err = u8"內容檔複製失敗: " + narrow(rel);
			for (const std::wstring& s : staged) DeleteFileW((exeDir + s + L".new").c_str());
			return false;
		}
		staged.push_back(rel);
	}
	for (size_t i = 0; i < rels.size(); i++) {
		if (!MoveFileExW((exeDir + rels[i] + L".new").c_str(), (exeDir + rels[i]).c_str(),
		                 MOVEFILE_REPLACE_EXISTING)) {
			if (err) *err = u8"內容檔替換失敗: " + narrow(rels[i]);
			for (size_t j = i; j < rels.size(); j++)
				DeleteFileW((exeDir + rels[j] + L".new").c_str());
			return false;
		}
	}
	return true;
}

// ---- policy -----------------------------------------------------------------

AppUpdateDecision ClassifyAppUpdate(std::tuple<int, int, int> remote,
                                    std::tuple<int, int, int> localApp,
                                    std::tuple<int, int, int> localTrans)
{
	AppUpdateDecision d;
	if (!(remote > localApp)) return d; // equal or older: never downgrade
	if (std::get<0>(remote) == std::get<0>(localApp) &&
	    std::get<1>(remote) == std::get<1>(localApp)) {
		d.applyTransNow = remote > localTrans; // patch bump: data-only release
	} else {
		d.promptApp = true;                    // minor/major bump: app release
	}
	return d;
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
		if (err) *err = u8"發佈資產網址無效: " + url;
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
			if (err) *err = u8"下載檔案雜湊值不符（下載損毀？）";
			return false;
		}
	} else {
		log_line(exeDir_, "asset without digest, hash check skipped: " + url);
	}
	return true;
}

bool AppUpdater::doCheck(std::string* err)
{
	setPhase(AppUpdatePhase::Checking, u8"檢查更新中…");

	std::string body;
	{
		HttpsClient api(kApiHost);
		if (!api.GetString(kLatestPath, body, err, &stop_)) return false;
	}

	RemoteRelease rel;
	try {
		ordered_json j = ordered_json::parse(body);
		std::string tag = j.value("tag_name", std::string());
		if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) tag.erase(0, 1);
		rel.ver = tag;
		std::string appName = "PobTools-" + tag + ".zip";
		std::string transName = "PobTools-Translations-" + tag + ".zip";
		if (j.contains("assets")) {
			for (const auto& a : j["assets"]) {
				std::string name = a.value("name", std::string());
				std::string url = a.value("browser_download_url", std::string());
				std::string digest = a.value("digest", std::string());
				if (digest.compare(0, 7, "sha256:") == 0) digest.erase(0, 7);
				else digest.clear(); // unknown scheme: skip hash check
				if (name == appName) { rel.hasApp = true; rel.appUrl = url; rel.appSha = digest; }
				else if (name == transName) { rel.hasTrans = true; rel.transUrl = url; rel.transSha = digest; }
			}
		}
	} catch (...) {
		if (err) *err = u8"版本資訊解析失敗";
		return false;
	}

	std::tuple<int, int, int> remoteV;
	if (!parse_semver(rel.ver, &remoteV)) {
		if (err) *err = u8"發佈版號無法解析: " + rel.ver;
		return false;
	}
	std::tuple<int, int, int> localApp;
	parse_semver(POBTOOLS_VERSION_STRING, &localApp);
	std::tuple<int, int, int> localTrans = localApp;
	{
		std::tuple<int, int, int> t;
		if (parse_semver(appliedTrans_, &t) && t > localTrans) localTrans = t;
	}

	latest_ = rel;
	latestSeen_ = rel.ver;
	{
		std::lock_guard<std::mutex> lk(stMx_);
		st_.latestVer = rel.ver;
	}

	AppUpdateDecision d = ClassifyAppUpdate(remoteV, localApp, localTrans);
	if (d.applyTransNow && rel.hasTrans) {
		std::string terr;
		if (!doUpdateTranslations(&terr)) {
			// silent: old dictionaries stay intact, retried next launch
			// (lastCheckUtc is not persisted on this path)
			log_line(exeDir_, "translation update failed: " + terr);
			setPhase(AppUpdatePhase::Idle, "");
			return true;
		}
	} else if (d.promptApp && rel.hasApp) {
		setPhase(AppUpdatePhase::AppAvailable,
		         u8"發現新版 v" + rel.ver + u8"（目前 v" POBTOOLS_VERSION_STRING u8"）");
	} else {
		setPhase(AppUpdatePhase::UpToDate, u8"已是最新版 v" POBTOOLS_VERSION_STRING);
	}

	lastCheckUtc_ = now_filetime();
	saveState();
	return true;
}

bool AppUpdater::doUpdateTranslations(std::string* err)
{
	setPhase(AppUpdatePhase::TransUpdating, u8"下載新翻譯資料 v" + latest_.ver + u8"…");

	std::vector<unsigned char> buf;
	if (!downloadAsset(latest_.transUrl, latest_.transSha, &buf, err, false)) return false;

	std::wstring cacheDir = exeDir_ + L"PobTools\\cache\\app_update\\" + widen(latest_.ver);
	std::wstring stage = cacheDir + L"\\trans_stage\\";
	remove_dir_rec(cacheDir);
	if (!ExtractZipToDir(buf.data(), buf.size(), stage, err)) return false;

	// pack sanity: dictionaries only — a mispackaged asset must not slip through
	if (!dir_exists(stage + L"Data") || file_exists(stage + L"pob-zh.exe") ||
	    dir_exists(stage + L"engine")) {
		if (err) *err = u8"翻譯資料包內容驗證失敗";
		remove_dir_rec(cacheDir);
		return false;
	}

	std::vector<std::wstring> rels;
	list_files_rec(stage, L"", &rels);
	if (!apply_content_two_pass(exeDir_, stage, rels, err)) {
		remove_dir_rec(cacheDir);
		return false;
	}

	appliedTrans_ = latest_.ver;
	remove_dir_rec(cacheDir);
	setPhase(AppUpdatePhase::TransDone,
	         u8"翻譯資料已更新至 v" + latest_.ver + u8"（引擎下次啟動生效）");
	log_line(exeDir_, "translations updated to v" + latest_.ver + " (" +
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
		if (err) *err = u8"更新暫存檔驗證失敗（可能被防毒隔離）";
		return false;
	}
	return true;
}

bool AppUpdater::doUpdateApp(std::string* err)
{
	if (!latest_.hasApp) {
		if (err) *err = u8"找不到對應的發佈資產";
		return false;
	}

	ULARGE_INTEGER freeBytes{};
	if (GetDiskFreeSpaceExW(exeDir_.c_str(), &freeBytes, nullptr, nullptr) &&
	    freeBytes.QuadPart < 300ull * 1024 * 1024) {
		if (err) *err = u8"磁碟空間不足（需 300MB）";
		return false;
	}

	{
		std::lock_guard<std::mutex> lk(stMx_);
		st_.phase = AppUpdatePhase::AppDownloading;
		st_.message = u8"下載主體更新 v" + latest_.ver + u8"…";
		st_.bytesDone = st_.bytesTotal = 0;
	}
	std::vector<unsigned char> buf;
	if (!downloadAsset(latest_.appUrl, latest_.appSha, &buf, err, true)) return false;

	setPhase(AppUpdatePhase::AppStaging, u8"解壓與驗證更新檔…");
	std::wstring cacheDir = exeDir_ + L"PobTools\\cache\\app_update\\" + widen(latest_.ver);
	std::wstring stage = cacheDir + L"\\app_stage\\";
	remove_dir_rec(cacheDir);
	if (!ExtractZipToDir(buf.data(), buf.size(), stage, err)) return false;
	if (!validate_app_stage(stage, err)) return false;

	{
		std::lock_guard<std::mutex> lk(stMx_);
		st_.phase = AppUpdatePhase::AppReadyToApply;
		st_.message = u8"更新檔就緒，即將重新啟動…";
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
                                    const std::string& tag, bool relaunch, std::string* errOut)
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
	if (!mtx) return fail(u8"無法建立更新鎖");
	if (GetLastError() == ERROR_ALREADY_EXISTS) {
		CloseHandle(mtx);
		return fail(u8"另一個 PobTools 實例正在套用更新");
	}

	int rc = 1;
	std::string msg;
	do {
		std::string verr;
		if (!validate_app_stage(stage, &verr)) { msg = verr; break; }

		std::vector<std::wstring> rels;
		list_files_rec(stage, L"", &rels);
		std::vector<std::wstring> content, boot;
		for (const std::wstring& rel : rels) {
			if (rel == L"pob-zh.exe" || rel.compare(0, 7, L"engine\\") == 0) boot.push_back(rel);
			else content.push_back(rel);
		}

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
					msg = u8"備份失敗: " + narrow(rel);
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
					msg = u8"放置失敗: " + narrow(boot[i]);
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
			msg += u8"（已還原舊版）";
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
			v["appliedTranslations"] = tag; // the full zip carries Data\ too
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
	if (rc != 0) return fail(msg.empty() ? u8"更新套用失敗" : msg);
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
	printf("local v%s, remote v%s\n%s\n", POBTOOLS_VERSION_STRING, st.latestVer.c_str(),
	       st.message.c_str());
	log_line(exeDir, "cli check: local v" POBTOOLS_VERSION_STRING ", remote v" + st.latestVer);
	if (checkOnly || st.phase != AppUpdatePhase::AppAvailable) return 0;

	if (!u.doUpdateApp(&err)) {
		printf("FAIL: %s\n", err.c_str());
		return 1;
	}
	st = u.Poll();
	std::string aerr;
	if (ApplyStagedAppUpdateAndRelaunch(exeDir, st.stageDir, st.latestVer, false, &aerr) != 0) {
		printf("FAIL: %s\n", aerr.c_str());
		return 1;
	}
	printf("updated to v%s - restart PobTools to finish\n", st.latestVer.c_str());
	return 0;
}

// Hidden helper for the one-time redirect verification: downloads the latest
// Translations asset (github.com -> objects.githubusercontent.com 302) and
// reports whether the sha256 digest matched. Applies nothing.
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
	if (!u.latest_.hasTrans) {
		printf("FAIL: latest release has no translations asset\n");
		return 1;
	}
	std::vector<unsigned char> buf;
	if (!u.downloadAsset(u.latest_.transUrl, u.latest_.transSha, &buf, &err, false)) {
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

	// T4: update policy matrix (patch = data-only, minor/major = app, never downgrade)
	{
		auto v = [](int a, int b, int c) { return std::tuple<int, int, int>{a, b, c}; };
		AppUpdateDecision d;
		bool ok = true;
		d = ClassifyAppUpdate(v(0, 1, 1), v(0, 1, 0), v(0, 1, 0));
		ok = ok && d.applyTransNow && !d.promptApp;              // patch bump
		d = ClassifyAppUpdate(v(0, 2, 0), v(0, 1, 5), v(0, 1, 5));
		ok = ok && !d.applyTransNow && d.promptApp;              // minor bump
		d = ClassifyAppUpdate(v(1, 0, 0), v(0, 9, 9), v(0, 9, 9));
		ok = ok && !d.applyTransNow && d.promptApp;              // major bump
		d = ClassifyAppUpdate(v(0, 1, 0), v(0, 1, 0), v(0, 1, 0));
		ok = ok && !d.applyTransNow && !d.promptApp;             // equal
		d = ClassifyAppUpdate(v(0, 0, 9), v(0, 1, 0), v(0, 1, 0));
		ok = ok && !d.applyTransNow && !d.promptApp;             // never downgrade
		d = ClassifyAppUpdate(v(0, 1, 2), v(0, 1, 0), v(0, 1, 2));
		ok = ok && !d.applyTransNow && !d.promptApp;             // patch already applied
		check(ok, "T4 update policy matrix");
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

	remove_dir_rec(root);

	report += fails == 0 ? "ALL PASS\r\n" : "FAILURES PRESENT\r\n";
	CreateDirectoryW((exeDir + L"PobTools").c_str(), nullptr);
	write_file_bytes(exeDir + L"PobTools\\app_update_selftest.txt", report.data(), report.size());
	printf("%s (report: PobTools\\app_update_selftest.txt)\n",
	       fails == 0 ? "ALL PASS" : "FAILURES PRESENT");
	return fails == 0 ? 0 : 1;
}
