#include "passive_tree_update.h"
#include "passive_import.h"
#include "launcher_config.h" // FindPoe1Dir
#include "http_client.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>

#include <json.hpp> // nlohmann::ordered_json (deps/nlohmann)

#pragma comment(lib, "shell32.lib")

#include <algorithm>
#include <cstdio>
#include <set>
#include <tuple>
#include <vector>

using nlohmann::ordered_json;

// GitHub repo with GGG's official character-tree export; releases ride on
// plain "3.29.0" tags (variants like -ruthless are other tree flavors).
static const wchar_t* kApiHost = L"api.github.com";
static const wchar_t* kRawHost = L"raw.githubusercontent.com";
static const wchar_t* kTagsPath = L"/repos/grindinggear/skilltree-export/tags?per_page=100";
static const wchar_t* kRawBase = L"/grindinggear/skilltree-export/";

// the sprite categories the condensed tree actually references (KEEP IN SYNC
// with passive_import.cpp / gen_passive_tree.py)
static const char* kUsedCats[] = {
	"normalActive", "notableActive", "keystoneActive",
	"normalInactive", "notableInactive", "keystoneInactive",
	"frame", "mastery", "groupBackground",
};
static const char* kZoom = "0.3835";

// ---- small helpers (same conventions as atlas_update.cpp) -------------------

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

static bool write_file_utf8(const std::wstring& path, const std::string& content)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	DWORD written = 0;
	bool ok = content.empty() ||
		(WriteFile(h, content.data(), (DWORD)content.size(), &written, nullptr) && written == content.size());
	CloseHandle(h);
	return ok;
}

static std::wstring widen(const std::string& s)
{
	if (s.empty()) return std::wstring();
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
	std::wstring w(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
	return w;
}

static bool file_exists(const std::wstring& p)
{
	DWORD a = GetFileAttributesW(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// "3.28.0" -> {3,28,0}; rejects suffixed variants ("3.25.0-ruthless")
static bool parse_semver(const std::string& s, std::tuple<int, int, int>* out)
{
	int a = 0, b = 0, c = 0, used = 0;
	if (sscanf_s(s.c_str(), "%d.%d.%d%n", &a, &b, &c, &used) != 3 || used != (int)s.size())
		return false;
	if (out) *out = { a, b, c };
	return true;
}

// folder-style tree version "3_28" -> {3,28}; false on anything else
static bool parse_folder_ver(const std::string& s, std::pair<int, int>* out)
{
	int a = 0, b = 0, used = 0;
	if (sscanf_s(s.c_str(), "%d_%d%n", &a, &b, &used) != 2 || used != (int)s.size())
		return false;
	if (out) *out = { a, b };
	return true;
}

static std::string folder_ver(int major, int minor)
{
	return std::to_string(major) + "_" + std::to_string(minor);
}

// cdn url -> basename
static std::string cdn_base_name(const std::string& url)
{
	std::string s = url;
	size_t q = s.find('?');
	if (q != std::string::npos) s = s.substr(0, q);
	size_t sl = s.find_last_of('/');
	if (sl != std::string::npos) s = s.substr(sl + 1);
	return s;
}

static void remove_cache_dir(const std::wstring& dir)
{
	auto clearFlat = [](const std::wstring& d) {
		WIN32_FIND_DATAW fd{};
		HANDLE h = FindFirstFileW((d + L"*").c_str(), &fd);
		if (h == INVALID_HANDLE_VALUE) return;
		do {
			if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
				DeleteFileW((d + fd.cFileName).c_str());
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	};
	clearFlat(dir + L"assets\\");
	RemoveDirectoryW((dir + L"assets").c_str());
	clearFlat(dir);
	RemoveDirectoryW(dir.c_str());
}

static long long now_filetime()
{
	FILETIME ft{};
	GetSystemTimeAsFileTime(&ft);
	ULARGE_INTEGER u{ ft.dwLowDateTime, ft.dwHighDateTime };
	return (long long)u.QuadPart;
}

// Bundled tree's "treeVersion" without a full 1MB JSON parse: both writers
// (gen_passive_tree.py and passive_import.cpp) emit it as the second field, so
// a bounded search over the head of the file is reliable.
static std::string read_bundled_tree_version(const std::wstring& exeDir)
{
	std::string content;
	if (!read_file_utf8(exeDir + L"Data\\passive_tree_poe1.json", content)) return std::string();
	const std::string key = "\"treeVersion\":\"";
	size_t pos = content.compare(0, 1, "{") == 0 ? content.find(key, 0) : std::string::npos;
	if (pos == std::string::npos || pos > 4096) return std::string();
	pos += key.size();
	size_t end = content.find('"', pos);
	if (end == std::string::npos || end - pos > 16) return std::string();
	return content.substr(pos, end - pos);
}

// newest bare TreeData\3_NN folder in the local PoB install ({0,0} when none)
static std::pair<int, int> newest_pob_tree_version(const std::wstring& exeDir)
{
	std::pair<int, int> best{ 0, 0 };
	std::wstring base = FindPoe1Dir(exeDir);
	if (base.empty()) return best;
	std::wstring pattern = base + L"TreeData\\*";
	WIN32_FIND_DATAW fd{};
	HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return best;
	do {
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
		char name[64] = {};
		WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof(name) - 1, nullptr, nullptr);
		std::pair<int, int> v;
		if (parse_folder_ver(name, &v) && v > best) best = v;
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	return best;
}

// ---- PassiveTreeUpdater -----------------------------------------------------

void PassiveTreeUpdater::Init(const std::wstring& exeDir)
{
	exeDir_ = exeDir;
	currentVer_ = read_bundled_tree_version(exeDir_);

	std::string content;
	if (read_file_utf8(exeDir_ + L"Data\\passive_update.json", content)) {
		try {
			ordered_json v = ordered_json::parse(content);
			lastCheckUtc_ = v.value("lastCheckUtc", 0ll);
		} catch (...) {
			lastCheckUtc_ = 0;
		}
	}
	worker_ = std::thread(&PassiveTreeUpdater::workerLoop, this);
}

void PassiveTreeUpdater::Shutdown()
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

void PassiveTreeUpdater::RequestCheck(bool force)
{
	if (!worker_.joinable()) return;
	static const long long kDay = 24ll * 3600 * 10'000'000; // FILETIME is 100ns units
	if (!force && lastCheckUtc_ > 0 && now_filetime() - lastCheckUtc_ < kDay)
		return;
	{
		std::lock_guard<std::mutex> lk(cmdMx_);
		cmdQ_.push_back(Cmd::Check);
	}
	cmdCv_.notify_one();
}

void PassiveTreeUpdater::StartUpdate()
{
	if (!worker_.joinable()) return;
	{
		std::lock_guard<std::mutex> lk(cmdMx_);
		cmdQ_.push_back(Cmd::Update);
	}
	cmdCv_.notify_one();
}

PassiveTreeUpdater::Status PassiveTreeUpdater::Poll()
{
	std::lock_guard<std::mutex> lk(stMx_);
	return st_;
}

void PassiveTreeUpdater::AckReload()
{
	std::lock_guard<std::mutex> lk(stMx_);
	st_.reloadPending = false;
	st_.phase = PassiveUpdatePhase::Idle;
	st_.message.clear();
}

void PassiveTreeUpdater::setPhase(PassiveUpdatePhase p, const std::string& msg)
{
	std::lock_guard<std::mutex> lk(stMx_);
	st_.phase = p;
	st_.message = msg;
}

void PassiveTreeUpdater::workerLoop()
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
				setPhase(PassiveUpdatePhase::Idle, "");
			}
		} else {
			if (!doUpdate(&err))
				setPhase(PassiveUpdatePhase::Error, err);
		}
	}
}

bool PassiveTreeUpdater::doCheck(std::string* err)
{
	setPhase(PassiveUpdatePhase::Checking, u8"檢查天賦樹資料更新中…");

	std::pair<int, int> current{ 0, 0 };
	parse_folder_ver(currentVer_, &current); // "" parses false -> {0,0}: anything is newer

	// a. local PoB TreeData (PoB self-updated before us: sheets are local)
	std::pair<int, int> localBest = newest_pob_tree_version(exeDir_);

	// b. GitHub skilltree-export tags (best-effort; local result stands alone)
	std::pair<int, int> remoteBest{ 0, 0 };
	std::string remoteFullTag;
	{
		std::string tagsBody, tagsErr;
		HttpsClient api(kApiHost);
		if (api.GetString(kTagsPath, tagsBody, &tagsErr, &stop_)) {
			try {
				ordered_json tags = ordered_json::parse(tagsBody);
				std::tuple<int, int, int> best{ -1, -1, -1 };
				for (const auto& t : tags) {
					std::string name = t.value("name", std::string());
					std::tuple<int, int, int> v;
					if (!parse_semver(name, &v)) continue;
					if (v > best) { best = v; remoteFullTag = name; }
				}
				if (!remoteFullTag.empty())
					remoteBest = { std::get<0>(best), std::get<1>(best) };
			} catch (...) {
			}
		}
	}

	std::pair<int, int> newest = std::max(localBest, remoteBest);
	if (newest <= current || newest == std::make_pair(0, 0)) {
		setPhase(PassiveUpdatePhase::UpToDate,
			currentVer_.empty() ? u8"天賦樹資料未知版本" : u8"天賦樹資料已是最新（" + currentVer_ + u8"）");
		lastCheckUtc_ = now_filetime();
		saveRecord();
		return true;
	}

	latestVer_ = folder_ver(newest.first, newest.second);
	// the data.json download always rides the GitHub tag; when only the local
	// PoB was newer, derive the plain tag ("3.29.0") and let doUpdate verify it
	latestTag_ = (remoteBest == newest && !remoteFullTag.empty())
		? remoteFullTag
		: std::to_string(newest.first) + "." + std::to_string(newest.second) + ".0";
	{
		std::lock_guard<std::mutex> lk(stMx_);
		st_.latestVer = latestVer_;
	}
	setPhase(PassiveUpdatePhase::UpdateAvailable,
		u8"發現新賽季天賦樹 " + latestVer_ +
		(currentVer_.empty() ? "" : u8"（目前 " + currentVer_ + u8"）") +
		(localBest == newest ? u8"｜POB 已更新，圖集可直接取用" : ""));

	lastCheckUtc_ = now_filetime();
	saveRecord();
	return true;
}

bool PassiveTreeUpdater::doUpdate(std::string* err)
{
	if (latestVer_.empty() && !doCheck(err)) return false;
	const std::string ver = latestVer_;
	const std::string tag = latestTag_;
	if (ver.empty() || tag.empty()) {
		if (err) *err = u8"沒有可更新的版本";
		return false;
	}

	setPhase(PassiveUpdatePhase::Downloading, u8"下載 " + tag + u8" 天賦樹資料中…");

	std::wstring cacheDir = exeDir_ + L"PobTools\\cache\\pt_update\\" + widen(tag) + L"\\";
	SHCreateDirectoryExW(nullptr, (cacheDir + L"assets").c_str(), nullptr);

	HttpsClient raw(kRawHost);

	std::string dataJson;
	{
		std::wstring path = std::wstring(kRawBase) + widen(tag) + L"/data.json";
		if (!raw.GetString(path, dataJson, err, &stop_)) return false;
		if (!write_file_utf8(cacheDir + L"data.json", dataJson))
			{ if (err) *err = u8"寫入下載快取失敗"; return false; }
	}

	// the sheets the condensed tree references; skip those PoB already has
	std::set<std::string> needed;
	try {
		ordered_json d = ordered_json::parse(dataJson);
		if (!d.contains("sprites") || !d["sprites"].is_object())
			{ if (err) *err = u8"data.json 缺少 sprites 段"; return false; }
		for (const char* cat : kUsedCats) {
			if (!d["sprites"].contains(cat) || !d["sprites"][cat].contains(kZoom)) continue;
			std::string base = cdn_base_name(d["sprites"][cat][kZoom].value("filename", std::string()));
			if (!base.empty()) needed.insert(base);
		}
	} catch (const std::exception& e) {
		if (err) *err = std::string(u8"data.json 解析失敗: ") + e.what();
		return false;
	}

	std::wstring poe1Base = FindPoe1Dir(exeDir_); // L"" when no PoB install detected
	std::wstring pobDir = poe1Base.empty() ? L"" : poe1Base + L"TreeData\\" + widen(ver) + L"\\";
	int done = 0;
	for (const std::string& base : needed) {
		if (stop_.load()) { if (err) *err = u8"已取消"; return false; }
		if (!pobDir.empty() && file_exists(pobDir + widen(base))) continue; // importer copies from PoB directly
		std::wstring dst = cacheDir + L"assets\\" + widen(base);
		if (!file_exists(dst)) {
			std::vector<unsigned char> bytes;
			std::wstring path = std::wstring(kRawBase) + widen(tag) + L"/assets/" + widen(base);
			if (!raw.Get(path, bytes, err, &stop_)) return false;
			std::string body((const char*)bytes.data(), bytes.size());
			if (!write_file_utf8(dst, body))
				{ if (err) *err = u8"寫入圖集快取失敗: " + base; return false; }
		}
		done++;
		setPhase(PassiveUpdatePhase::Downloading,
			u8"下載 " + tag + u8" 圖集中… " + std::to_string(done) + "/" + std::to_string((int)needed.size()));
	}

	setPhase(PassiveUpdatePhase::Importing, u8"匯入 " + ver + u8" 中…");
	std::string sum;
	if (!ImportPassiveTreeData(cacheDir + L"data.json", ver, exeDir_, err, &sum)) return false;

	currentVer_ = ver;
	lastCheckUtc_ = now_filetime();
	saveRecord();
	remove_cache_dir(cacheDir); // best-effort

	{
		std::lock_guard<std::mutex> lk(stMx_);
		st_.phase = PassiveUpdatePhase::Done;
		st_.message = sum;
		st_.reloadPending = true;
	}
	return true;
}

void PassiveTreeUpdater::saveRecord()
{
	ordered_json v;
	v["lastCheckUtc"] = (long long)lastCheckUtc_;
	CreateDirectoryW((exeDir_ + L"Data").c_str(), nullptr);
	write_file_utf8(exeDir_ + L"Data\\passive_update.json", v.dump());
}

// ---- CLI --------------------------------------------------------------------

int RunPassiveTreeUpdateCli(const std::wstring& exeDir)
{
	if (AttachConsole(ATTACH_PARENT_PROCESS)) {
		FILE* f = nullptr;
		freopen_s(&f, "CONOUT$", "w", stdout);
	}

	PassiveTreeUpdater u;
	u.exeDir_ = exeDir;
	u.currentVer_ = read_bundled_tree_version(exeDir);

	std::string err;
	if (!u.doCheck(&err)) {
		printf("FAIL: %s\n", err.c_str());
		return 1;
	}
	PassiveTreeUpdater::Status st = u.Poll();
	printf("%s\n", st.message.c_str());
	if (st.phase != PassiveUpdatePhase::UpdateAvailable)
		return 0;

	if (!u.doUpdate(&err)) {
		printf("FAIL: %s\n", err.c_str());
		return 1;
	}
	st = u.Poll();
	printf("%s\n", st.message.c_str());
	return 0;
}
