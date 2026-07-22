#include "atlas_update.h"
#include "atlas_import.h"
#include "atlas_version_index.h" // versioned season layout + rolling retention
#include "http_client.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>

#include <json.hpp> // nlohmann::ordered_json (deps/nlohmann)

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#pragma comment(lib, "shell32.lib")

using nlohmann::ordered_json;

// GitHub repo with GGG's official atlas tree export; updates ride on release
// tags (plain "3.28.0" — the -ruthless/-atlas variants are other tree flavors).
static const wchar_t* kApiHost = L"api.github.com";
static const wchar_t* kRawHost = L"raw.githubusercontent.com";
static const wchar_t* kTagsPath = L"/repos/grindinggear/atlastree-export/tags?per_page=100";
static const wchar_t* kRawBase = L"/grindinggear/atlastree-export/";

// repoe-fork publishes the same tree with official Traditional-Chinese names
// and stat text, keyed by the same node hashes.
static const wchar_t* kRepoeHost = L"repoe-fork.github.io";
static const wchar_t* kRepoeVersionPath = L"/version.txt";
static const wchar_t* kRepoeTcAtlasPath = L"/Traditional%20Chinese/passive_skill_trees/Atlas.json";

// ---- small helpers (same conventions as atlas_import.cpp) ---------------------

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

// "3.28.0" -> {3,28,0}; rejects anything with a suffix ("3.25.0-ruthless").
static bool parse_semver(const std::string& s, std::tuple<int, int, int>* out)
{
	int a = 0, b = 0, c = 0, used = 0;
	if (sscanf_s(s.c_str(), "%d.%d.%d%n", &a, &b, &c, &used) != 3 || used != (int)s.size())
		return false;
	if (out) *out = { a, b, c };
	return true;
}

// cdn url "https://web.poecdn.com/.../atlas-skills-4.jpg?bcad" -> "atlas-skills-4.jpg"
static std::string cdn_base_name(const std::string& url)
{
	std::string s = url;
	size_t q = s.find('?');
	if (q != std::string::npos) s = s.substr(0, q);
	size_t sl = s.find_last_of('/');
	if (sl != std::string::npos) s = s.substr(sl + 1);
	return s;
}

// Deletes every plain file directly under d (trailing backslash), non-recursive.
static void clear_flat_dir(const std::wstring& d)
{
	WIN32_FIND_DATAW fd{};
	HANDLE h = FindFirstFileW((d + L"*").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return;
	do {
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
			DeleteFileW((d + fd.cFileName).c_str());
	} while (FindNextFileW(h, &fd));
	FindClose(h);
}

// Best-effort removal of a folder with one known subdirectory of files
// (download cache: assets/; season folder: atlas/).
static void remove_dir_with_sub(const std::wstring& dir, const wchar_t* sub)
{
	clear_flat_dir(dir + sub + L"\\");
	RemoveDirectoryW((dir + sub).c_str());
	clear_flat_dir(dir);
	RemoveDirectoryW(dir.c_str());
}

static void remove_cache_dir(const std::wstring& dir) { remove_dir_with_sub(dir, L"assets"); }

static long long now_filetime()
{
	FILETIME ft{};
	GetSystemTimeAsFileTime(&ft);
	ULARGE_INTEGER u{ ft.dwLowDateTime, ft.dwHighDateTime };
	return (long long)u.QuadPart;
}

// ---- zh mapping generation ----------------------------------------------------
// KEEP IN SYNC with tools/gen_atlas_zh.py (build-time snapshot generator);
// both must emit the same atlas_tree_zh.json schema.

// repoe-fork keeps the "Grants N Atlas Passive Skill Points" line in the
// passive's skill_points field rather than stat_text, so statsEn ends up one
// line longer than statsZh and the whole node then fails the equal-length
// guard in atlas_i18n.cpp (every line shows English). Synthesise that trailing
// line so the arrays align. KEEP IN SYNC with _align_atlas_points_line() in
// tools/gen_atlas_zh.py.
static void AlignAtlasPointsLine(ordered_json& n)
{
	if (!n.contains("statsEn") || !n.contains("statsZh")) return;
	if (!n["statsEn"].is_array() || !n["statsZh"].is_array()) return;
	ordered_json& en = n["statsEn"];
	ordered_json& zh = n["statsZh"];
	if (en.empty() || en.size() != zh.size() + 1) return;
	if (!en.back().is_string()) return;
	const std::string s = en.back().get<std::string>();
	const std::string pre = "Grants ";
	const std::string suf = " Atlas Passive Skill Points";
	if (s.size() <= pre.size() + suf.size()) return;
	if (s.compare(0, pre.size(), pre) != 0) return;
	if (s.compare(s.size() - suf.size(), suf.size(), suf) != 0) return;
	const std::string num = s.substr(pre.size(), s.size() - pre.size() - suf.size());
	if (num.empty()) return;
	for (char c : num)
		if (c < '0' || c > '9') return;
	zh.push_back(std::string(u8"授予 ") + num + u8" 點地圖天賦點數");
}

// Numbers in a stat line, matching gen_atlas_zh.py's -?\d+(?:\.\d+)? regex.
static std::vector<std::string> StatLineNums(const std::string& s)
{
	std::vector<std::string> out;
	size_t i = 0, n = s.size();
	while (i < n) {
		if (!isdigit((unsigned char)s[i])) { i++; continue; }
		size_t start = i;
		if (start > 0 && s[start - 1] == '-') start--;
		size_t j = i;
		while (j < n && isdigit((unsigned char)s[j])) j++;
		if (j + 1 < n && s[j] == '.' && isdigit((unsigned char)s[j + 1])) {
			j++;
			while (j < n && isdigit((unsigned char)s[j])) j++;
		}
		out.push_back(s.substr(start, j - start));
		i = j;
	}
	return out;
}

// repoe-fork's stat_text order can differ from GGG's stats, pairing statsZh[i]
// with the wrong statsEn[i] (the panel's value backfill then fails -> English).
// Reorder statsZh by number signature. KEEP IN SYNC with _align_stats_zh() in
// tools/gen_atlas_zh.py.
static void AlignStatsZhOrder(ordered_json& n)
{
	if (!n.contains("statsEn") || !n.contains("statsZh")) return;
	if (!n["statsEn"].is_array() || !n["statsZh"].is_array()) return;
	ordered_json& en = n["statsEn"];
	ordered_json& zh = n["statsZh"];
	const size_t N = en.size();
	if (N == 0 || N != zh.size()) return;

	std::vector<std::vector<std::string>> enNums(N), zhNums(N);
	std::vector<std::string> zhStr(N);
	for (size_t i = 0; i < N; i++) {
		if (!en[i].is_string() || !zh[i].is_string()) return;
		enNums[i] = StatLineNums(en[i].get<std::string>());
		zhStr[i] = zh[i].get<std::string>();
		zhNums[i] = StatLineNums(zhStr[i]);
	}

	std::vector<size_t> order(N);
	for (size_t i = 0; i < N; i++) order[i] = i;
	std::sort(order.begin(), order.end(),
	          [&](size_t a, size_t b) { return enNums[a].size() > enNums[b].size(); });

	std::vector<bool> used(N, false);
	std::vector<int> assign(N, -1);
	for (size_t oi = 0; oi < N; oi++) {
		const size_t i = order[oi];
		if (enNums[i].empty()) continue;
		std::map<std::string, int> ce;
		for (const auto& x : enNums[i]) ce[x]++;
		int bestJ = -1, bestExtra = 0;
		for (size_t j = 0; j < N; j++) {
			if (used[j]) continue;
			std::map<std::string, int> cz;
			for (const auto& x : zhNums[j]) cz[x]++;
			bool subset = true;
			for (const auto& kv : ce)
				if (cz[kv.first] < kv.second) { subset = false; break; }
			if (!subset) continue;
			const int extra = (int)zhNums[j].size() - (int)enNums[i].size();
			if (bestJ < 0 || extra < bestExtra) { bestJ = (int)j; bestExtra = extra; }
		}
		if (bestJ >= 0) { assign[i] = bestJ; used[bestJ] = true; }
	}
	std::vector<size_t> rem;
	for (size_t j = 0; j < N; j++) if (!used[j]) rem.push_back(j);
	size_t ri = 0;
	for (size_t i = 0; i < N; i++) if (assign[i] < 0) assign[i] = (int)rem[ri++];

	ordered_json newZh = ordered_json::array();
	for (size_t i = 0; i < N; i++) newZh.push_back(zhStr[assign[i]]);
	n["statsZh"] = std::move(newZh);
}

bool GenerateAtlasZhMapping(const std::string& gggDataJson, const std::string& tcAtlasJson,
                            const std::string& tag, const std::string& repoeVersion,
                            const std::wstring& destDir, std::string* err, std::string* summary)
{
	auto fail = [&](const std::string& m) {
		if (err) *err = m;
		return false;
	};

	ordered_json ggg, tc;
	try {
		ggg = ordered_json::parse(gggDataJson);
		tc = ordered_json::parse(tcAtlasJson);
	} catch (const std::exception& e) {
		return fail(std::string(u8"對照資料解析失敗: ") + e.what());
	}

	try {
		if (!ggg.contains("nodes") || !ggg["nodes"].is_object())
			return fail(u8"data.json 缺少 nodes（不是 atlastree-export 格式？）");
		if (!tc.contains("passives") || !tc["passives"].is_object())
			return fail(u8"繁中 Atlas.json 缺少 passives（repoe-fork schema 已變動？）");

		const ordered_json& tcp = tc["passives"];
		ordered_json outNodes;
		int total = 0, joined = 0;

		for (const auto& [key, v] : ggg["nodes"].items()) {
			if (key == "root" || v.value("isMastery", false)) continue;
			total++;
			int id = v.value("skill", 0);
			ordered_json n;
			n["en"] = v.value("name", std::string());
			ordered_json statsEn = v.contains("stats") ? v["stats"] : ordered_json::array();

			auto it = tcp.find(std::to_string(id));
			if (it != tcp.end()) {
				joined++;
				n["zh"] = it->value("name", std::string());
				n["statsEn"] = statsEn;
				n["statsZh"] = it->contains("stat_text") ? (*it)["stat_text"] : ordered_json::array();
				AlignAtlasPointsLine(n);
				AlignStatsZhOrder(n);
			} else {
				// keep the English side so the file doubles as a full node list
				n["statsEn"] = statsEn;
			}
			outNodes[std::to_string(id)] = std::move(n);
		}

		if (total == 0)
			return fail(u8"data.json 沒有任何節點，已中止");
		if (joined == 0)
			return fail(u8"沒有任何節點能對上繁中資料（hash 完全不合），保留舊對照");

		ordered_json out;
		out["tag"] = tag;
		out["repoe"] = repoeVersion;
		out["joined"] = joined;
		out["total"] = total;
		out["nodes"] = std::move(outNodes);

		SHCreateDirectoryExW(nullptr, destDir.c_str(), nullptr);
		if (!write_file_utf8(destDir + L"atlas_tree_zh.json", out.dump()))
			return fail(u8"寫入 atlas_tree_zh.json 失敗");

		int pct = (int)(100.0 * joined / total + 0.5);
		if (summary) {
			*summary = u8"中英對照：" + std::to_string(joined) + "/" + std::to_string(total) +
			           u8" 節點有繁中（" + std::to_string(pct) + "%）";
			if (pct < 50)
				*summary += u8"（覆蓋率偏低，repoe-fork 可能落後此賽季）";
		}
		return true;
	} catch (const std::exception& e) {
		return fail(std::string(u8"對照生成發生例外: ") + e.what());
	}
}

// ---- AtlasUpdater -------------------------------------------------------------

void AtlasUpdater::Init(const std::wstring& exeDir)
{
	exeDir_ = exeDir;

	// version record (missing file = never updated -> any tag counts as new)
	std::string content;
	if (read_file_utf8(exeDir_ + L"Data\\atlas_version.json", content)) {
		try {
			ordered_json v = ordered_json::parse(content);
			tag_ = v.value("tag", std::string());
			sha_ = v.value("sha", std::string());
			repoe_ = v.value("repoe", std::string());
			lastCheckUtc_ = v.value("lastCheckUtc", 0ll);
		} catch (...) {
			// corrupt record reads as "never updated": forces a fresh check
			tag_.clear();
			sha_.clear();
			repoe_.clear();
			lastCheckUtc_ = 0;
		}
	}

	worker_ = std::thread(&AtlasUpdater::workerLoop, this);
}

void AtlasUpdater::Shutdown()
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

void AtlasUpdater::RequestCheck(bool force)
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

void AtlasUpdater::StartUpdate()
{
	if (!worker_.joinable()) return;
	{
		std::lock_guard<std::mutex> lk(cmdMx_);
		cmdQ_.push_back(Cmd::Update);
	}
	cmdCv_.notify_one();
}

AtlasUpdater::Status AtlasUpdater::Poll()
{
	std::lock_guard<std::mutex> lk(stMx_);
	return st_;
}

void AtlasUpdater::AckReload()
{
	std::lock_guard<std::mutex> lk(stMx_);
	st_.reloadPending = false;
	st_.zhRefreshed = false;
	st_.phase = AtlasUpdatePhase::Idle;
	st_.message.clear();
}

void AtlasUpdater::setPhase(AtlasUpdatePhase p, const std::string& msg)
{
	std::lock_guard<std::mutex> lk(stMx_);
	st_.phase = p;
	st_.message = msg;
}

void AtlasUpdater::workerLoop()
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
				setPhase(AtlasUpdatePhase::Idle, "");
			}
		} else {
			if (!doUpdate(&err))
				setPhase(AtlasUpdatePhase::Error, err);
		}
	}
}

bool AtlasUpdater::doCheck(std::string* err)
{
	setPhase(AtlasUpdatePhase::Checking, u8"檢查圖譜資料更新中…");

	// newest release tag on GitHub
	std::string tagsBody;
	{
		HttpsClient api(kApiHost);
		if (!api.GetString(kTagsPath, tagsBody, err, &stop_)) return false;
	}
	std::string latestTag, latestSha;
	std::tuple<int, int, int> best{ -1, -1, -1 };
	try {
		ordered_json tags = ordered_json::parse(tagsBody);
		for (const auto& t : tags) {
			std::string name = t.value("name", std::string());
			std::tuple<int, int, int> v;
			if (!parse_semver(name, &v)) continue; // skip -ruthless/-atlas variants
			if (v > best) {
				best = v;
				latestTag = name;
				latestSha = t.contains("commit") ? t["commit"].value("sha", std::string()) : std::string();
			}
		}
	} catch (const std::exception& e) {
		if (err) *err = std::string(u8"tags 回應解析失敗: ") + e.what();
		return false;
	}
	if (latestTag.empty()) {
		if (err) *err = u8"GitHub 上找不到任何正式版 tag";
		return false;
	}

	// current repoe-fork dataset version (best-effort; zh refresh only)
	std::string repoeVer;
	{
		HttpsClient repoe(kRepoeHost);
		std::string ignored;
		if (repoe.GetString(kRepoeVersionPath, repoeVer, &ignored, &stop_)) {
			while (!repoeVer.empty() && (repoeVer.back() == '\n' || repoeVer.back() == '\r' || repoeVer.back() == ' '))
				repoeVer.pop_back();
		}
	}

	latestTag_ = latestTag;
	latestSha_ = latestSha;
	{
		std::lock_guard<std::mutex> lk(stMx_);
		st_.latestTag = latestTag;
	}

	if (latestTag != tag_) {
		setPhase(AtlasUpdatePhase::UpdateAvailable,
			u8"發現新賽季圖譜資料 " + latestTag + (tag_.empty() ? "" : u8"（目前 " + tag_ + u8"）"));
	} else if (!repoeVer.empty() && repoeVer != repoe_) {
		// same tree, newer translations: rebuild the zh mapping in place
		setPhase(AtlasUpdatePhase::Checking, u8"更新繁中對照資料中…");
		std::string zhErr;
		if (refreshZhMapping(latestTag, repoeVer, &zhErr)) {
			repoe_ = repoeVer;
			std::lock_guard<std::mutex> lk(stMx_);
			st_.phase = AtlasUpdatePhase::UpToDate;
			st_.message = u8"繁中對照已更新（repoe " + repoeVer + u8"）";
			st_.zhRefreshed = true;
		} else {
			// keep the old mapping; retry on a future check
			setPhase(AtlasUpdatePhase::UpToDate, "");
		}
	} else {
		setPhase(AtlasUpdatePhase::UpToDate, u8"圖譜資料已是最新（" + latestTag + u8"）");
	}

	lastCheckUtc_ = now_filetime();
	saveVersionRecord();
	return true;
}

// Re-fetches the English data.json (for names/stats) plus the TC Atlas.json and
// rebuilds Data/atlas_tree_zh.json. Used by the catch-up path when repoe-fork
// publishes translations after a season update already happened.
bool AtlasUpdater::refreshZhMapping(const std::string& tag, const std::string& repoeVer, std::string* err)
{
	std::string dataJson;
	{
		HttpsClient raw(kRawHost);
		std::wstring path = std::wstring(kRawBase) + widen(tag) + L"/data.json";
		if (!raw.GetString(path, dataJson, err, &stop_)) return false;
	}
	std::string tcJson;
	{
		HttpsClient repoe(kRepoeHost);
		if (!repoe.GetString(kRepoeTcAtlasPath, tcJson, err, &stop_)) return false;
	}
	// write the refreshed mapping into the season's own folder (versioned
	// layout); ResolveDataDir falls back to flat Data/ for a legacy install
	AtlasVersionIndex idx;
	idx.Load(exeDir_);
	std::wstring dest = idx.ResolveDataDir(exeDir_, tag);
	std::string summary;
	return GenerateAtlasZhMapping(dataJson, tcJson, tag, repoeVer, dest, err, &summary);
}

bool AtlasUpdater::doUpdate(std::string* err)
{
	// normally preceded by a successful check; run one defensively otherwise
	if (latestTag_.empty() && !doCheck(err)) return false;
	const std::string tag = latestTag_;
	if (tag.empty()) {
		if (err) *err = u8"沒有可更新的版本";
		return false;
	}

	setPhase(AtlasUpdatePhase::Downloading, u8"下載 " + tag + u8" 資料中…");
	{
		std::lock_guard<std::mutex> lk(stMx_);
		st_.filesDone = 0;
		st_.filesTotal = 0;
	}

	std::wstring cacheDir = exeDir_ + L"PobTools\\cache\\atlas_update\\" + widen(tag) + L"\\";
	SHCreateDirectoryExW(nullptr, (cacheDir + L"assets").c_str(), nullptr);

	HttpsClient raw(kRawHost);

	// data.json first; the sprite list decides which sheets we actually need
	std::string dataJson;
	{
		std::wstring path = std::wstring(kRawBase) + widen(tag) + L"/data.json";
		if (!raw.GetString(path, dataJson, err, &stop_)) return false;
		if (!write_file_utf8(cacheDir + L"data.json", dataJson))
			{ if (err) *err = u8"寫入下載快取失敗"; return false; }
	}

	std::set<std::string> assets;
	try {
		ordered_json d = ordered_json::parse(dataJson);
		if (!d.contains("sprites") || !d["sprites"].is_object())
			{ if (err) *err = u8"data.json 缺少 sprites 段"; return false; }
		for (const auto& [cat, sec] : d["sprites"].items()) {
			(void)cat;
			if (!sec.is_object() || !sec.contains("0.5")) continue; // import uses zoom 0.5 only
			std::string base = cdn_base_name(sec["0.5"].value("filename", std::string()));
			if (!base.empty()) assets.insert(base);
		}
	} catch (const std::exception& e) {
		if (err) *err = std::string(u8"data.json 解析失敗: ") + e.what();
		return false;
	}

	{
		std::lock_guard<std::mutex> lk(stMx_);
		st_.filesTotal = (int)assets.size();
	}
	int done = 0;
	for (const std::string& base : assets) {
		if (stop_.load()) { if (err) *err = u8"已取消"; return false; }
		std::wstring dst = cacheDir + L"assets\\" + widen(base);
		DWORD attr = GetFileAttributesW(dst.c_str());
		bool cached = attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
		if (!cached) {
			std::vector<unsigned char> bytes;
			std::wstring path = std::wstring(kRawBase) + widen(tag) + L"/assets/" + widen(base);
			if (!raw.Get(path, bytes, err, &stop_)) return false;
			std::string body((const char*)bytes.data(), bytes.size());
			if (!write_file_utf8(dst, body))
				{ if (err) *err = u8"寫入圖集快取失敗: " + base; return false; }
		}
		done++;
		std::lock_guard<std::mutex> lk(stMx_);
		st_.filesDone = done;
		st_.message = u8"下載 " + tag + u8" 圖集中… " + std::to_string(done) + "/" + std::to_string((int)assets.size());
	}

	// install the new season into its own folder (Data/atlas_versions/<tag>/) so
	// the previous season survives side by side for the compare view.
	std::wstring seasonDir = AtlasVersionIndex::VersionDir(exeDir_, tag);
	setPhase(AtlasUpdatePhase::Importing, u8"匯入 " + tag + u8" 中…");
	std::string sum;
	if (!ImportAtlasTreeData(cacheDir + L"data.json", seasonDir, err, &sum)) return false;

	// zh mapping into the same season folder; best-effort (English falls back)
	std::string zhNote, repoeVer;
	{
		std::string tcJson, zhErr, zhSum;
		HttpsClient repoe(kRepoeHost);
		std::string ignored;
		if (repoe.GetString(kRepoeVersionPath, repoeVer, &ignored, &stop_)) {
			while (!repoeVer.empty() && (repoeVer.back() == '\n' || repoeVer.back() == '\r' || repoeVer.back() == ' '))
				repoeVer.pop_back();
		}
		if (repoe.GetString(kRepoeTcAtlasPath, tcJson, &zhErr, &stop_) &&
			GenerateAtlasZhMapping(dataJson, tcJson, tag, repoeVer, seasonDir, &zhErr, &zhSum)) {
			repoe_ = repoeVer;
			zhNote = u8"；" + zhSum;
		} else {
			zhNote = u8"；繁中對照暫時無法更新，沿用舊對照";
		}
	}

	// register the season, make it active, and roll back to the newest two: the
	// third-oldest folder is deleted so the disk holds exactly two leagues.
	int keptSeasons = 0;
	{
		AtlasVersionIndex idx;
		idx.Load(exeDir_);
		AtlasVersionEntry e;
		e.tag = tag; e.sha = latestSha_; e.repoe = repoeVer;
		idx.UpsertActive(e);
		std::vector<std::string> dropped = idx.PruneToNewest(2);
		idx.SetLastCheckUtc(now_filetime());
		idx.Save(exeDir_);
		keptSeasons = (int)idx.Versions().size();
		for (const std::string& d : dropped)
			remove_dir_with_sub(AtlasVersionIndex::VersionDir(exeDir_, d), L"atlas");
	}

	tag_ = tag;
	sha_ = latestSha_;
	lastCheckUtc_ = now_filetime();
	saveVersionRecord();          // legacy record: keeps the daily-throttle state
	remove_cache_dir(cacheDir);   // best-effort

	{
		std::lock_guard<std::mutex> lk(stMx_);
		st_.phase = AtlasUpdatePhase::Done;
		st_.message = sum + zhNote +
			(keptSeasons >= 2 ? u8"；已保留最近兩季，可用「版本比較」查看本次改版變更" : "");
		st_.reloadPending = true;
	}
	return true;
}

void AtlasUpdater::saveVersionRecord()
{
	ordered_json v;
	v["tag"] = tag_;
	v["sha"] = sha_;
	v["repoe"] = repoe_;
	v["lastCheckUtc"] = (long long)lastCheckUtc_;
	CreateDirectoryW((exeDir_ + L"Data").c_str(), nullptr);
	write_file_utf8(exeDir_ + L"Data\\atlas_version.json", v.dump());
}

// ---- CLI wrappers -------------------------------------------------------------

static void attach_parent_console()
{
	if (AttachConsole(ATTACH_PARENT_PROCESS)) {
		FILE* f = nullptr;
		freopen_s(&f, "CONOUT$", "w", stdout);
	}
}

int RunAtlasUpdateCli(const std::wstring& exeDir)
{
	attach_parent_console();

	AtlasUpdater u;
	u.exeDir_ = exeDir;
	{
		std::string content;
		if (read_file_utf8(exeDir + L"Data\\atlas_version.json", content)) {
			try {
				ordered_json v = ordered_json::parse(content);
				u.tag_ = v.value("tag", std::string());
				u.sha_ = v.value("sha", std::string());
				u.repoe_ = v.value("repoe", std::string());
			} catch (...) {
				// corrupt record reads as "never updated": forces a full update
				u.tag_.clear();
				u.sha_.clear();
				u.repoe_.clear();
			}
		}
	}

	std::string err;
	if (!u.doCheck(&err)) {
		printf("FAIL: %s\n", err.c_str());
		return 1;
	}
	AtlasUpdater::Status st = u.Poll();
	printf("%s\n", st.message.c_str());
	if (st.phase != AtlasUpdatePhase::UpdateAvailable)
		return 0; // up to date (possibly after a zh-mapping refresh)

	if (!u.doUpdate(&err)) {
		printf("FAIL: %s\n", err.c_str());
		return 1;
	}
	st = u.Poll();
	printf("%s\n", st.message.c_str());
	return 0;
}

int RunAtlasZhCli(const std::wstring& dataJsonPath, const std::wstring& tcJsonPath,
                  const std::wstring& exeDir)
{
	attach_parent_console();

	std::string dataJson, tcJson;
	if (!read_file_utf8(dataJsonPath, dataJson)) {
		printf("FAIL: cannot read data.json\n");
		return 1;
	}
	if (!read_file_utf8(tcJsonPath, tcJson)) {
		printf("FAIL: cannot read TC Atlas.json\n");
		return 1;
	}
	AtlasVersionIndex idx;
	idx.Load(exeDir);
	std::wstring dest = idx.ResolveDataDir(exeDir, idx.Active());
	std::string err, summary;
	if (!GenerateAtlasZhMapping(dataJson, tcJson, "local", "local", dest, &err, &summary)) {
		printf("FAIL: %s\n", err.c_str());
		return 1;
	}
	printf("%s\n", summary.c_str());
	return 0;
}
