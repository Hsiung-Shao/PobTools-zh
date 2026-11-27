#include "atlas_update.h"
#include "error_log.h"
#include "atlas_import.h"
#include "atlas_i18n.h"
#include "atlas_version_index.h" // versioned season layout + rolling retention
#include "http_client.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include "win_util.h"

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

// GitHub repo with GGG's official atlas tree export; updates ride on release tags (plain "3.28.0"; -ruthless/-atlas are other flavors).
static const wchar_t* kApiHost = L"api.github.com";
static const wchar_t* kRawHost = L"raw.githubusercontent.com";
static const wchar_t* kTagsPath = L"/repos/grindinggear/atlastree-export/tags?per_page=100";
static const wchar_t* kHeadPath = L"/repos/grindinggear/atlastree-export/commits?per_page=1";
static const wchar_t* kRawBase = L"/grindinggear/atlastree-export/";

// repoe-fork publishes the same tree with official Traditional-Chinese names and stat text, keyed by the same node hashes.
static const wchar_t* kRepoeHost = L"repoe-fork.github.io";
static const wchar_t* kRepoeVersionPath = L"/version.txt";
static const wchar_t* kRepoeTcAtlasPath = L"/Traditional%20Chinese/passive_skill_trees/Atlas.json";
// same rows in English: pairs statsZh to statsEn by text (best-effort)
static const wchar_t* kRepoeEnAtlasPath = L"/passive_skill_trees/Atlas.json";

// small helpers (same conventions as atlas_import.cpp)

// "3.28.0" -> {3,28,0}; rejects anything with a suffix ("3.25.0-ruthless").
static bool parse_semver(const std::string& s, std::tuple<int, int, int>* out)
{
	int a = 0, b = 0, c = 0, used = 0;
	if (sscanf_s(s.c_str(), "%d.%d.%d%n", &a, &b, &c, &used) != 3 || used != (int)s.size())
		return false;
	if (out) *out = { a, b, c };
	return true;
}

bool PickAtlasRemote(const std::string& tagsBody, const std::string& headBody,
                     std::string* tag, std::string* sha, bool* fromHead, std::string* err)
{
	std::string latestTag, latestSha;
	bool head = false;
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
	try {
		ordered_json h = headBody.empty() ? ordered_json() : ordered_json::parse(headBody);
		if (h.is_array() && !h.empty() && h[0].is_object()) {
			std::string hs = h[0].value("sha", std::string());
			std::string msg = h[0].contains("commit") && h[0]["commit"].is_object()
				? h[0]["commit"].value("message", std::string()) : std::string();
			int a = 0, b = 0, c = 0, used = 0;
			// same league (major.minor) and not older than the tag: "3.30.0c"/"3.30.3" yes,
			// "3.31.0 preview"/"3.29.9"/"3.30.0-ruthless"/"3.30.01" no
			const char nx = (sscanf_s(msg.c_str(), "%d.%d.%d%n", &a, &b, &c, &used) == 3) ? msg[used] : '.';
			bool same = std::make_tuple(a, b) == std::make_tuple(std::get<0>(best), std::get<1>(best)) &&
				std::make_tuple(a, b, c) >= best && !isdigit((unsigned char)nx) && nx != '.' && nx != '-';
			if (same && !hs.empty()) { latestSha = hs; head = true; }
		}
	} catch (const std::exception&) {
		// unparsable head: keep the tag
	}
	*tag = latestTag;
	*sha = latestSha;
	*fromHead = head;
	return true;
}

bool AtlasNeedsUpdate(const std::string& localTag, const std::string& localSha,
                      const std::string& remoteTag, const std::string& remoteSha, bool remoteFromHead)
{
	return remoteTag != localTag ||
	       (remoteFromHead && !localSha.empty() && !remoteSha.empty() && remoteSha != localSha);
}

std::string AtlasInstalledSha(const std::wstring& exeDir, const std::string& tag, const std::string& recordSha)
{
	// the season folder's own stamp travels with its files; the index can be restored to a stale copy
	std::string stamp;
	if (read_file_utf8(AtlasVersionIndex::VersionDir(exeDir, tag) + L"version.json", stamp)) {
		try {
			std::string s = ordered_json::parse(stamp).value("sha", std::string());
			if (!s.empty()) return s;
		} catch (...) {
		}
	}
	AtlasVersionIndex idx;
	idx.Load(exeDir);
	const AtlasVersionEntry* e = idx.Find(tag);
	return e && !e->sha.empty() ? e->sha : recordSha;
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

// zh mapping generation. KEEP IN SYNC with tools/gen_atlas_zh.py (both must emit the same atlas_tree_zh.json schema).

// repoe-fork keeps the "Grants N Atlas Passive Skill Points" line in skill_points, not stat_text, so statsEn is one
// line longer than statsZh and the node fails the equal-length guard in atlas_i18n.cpp (all English). Synthesise
// that trailing line. KEEP IN SYNC with _align_atlas_points_line() in tools/gen_atlas_zh.py.
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

// "[Id|Text]" -> "Text", "[Text]" -> "Text", whitespace runs -> one space.
static std::string StatLineKey(const std::string& s)
{
	std::string out;
	for (size_t i = 0; i < s.size();) {
		if (s[i] == '[') {
			const size_t close = s.find(']', i);
			if (close != std::string::npos) {
				const size_t bar = s.find('|', i);
				const size_t from = (bar != std::string::npos && bar < close) ? bar + 1 : i + 1;
				out.append(s, from, close - from);
				i = close + 1;
				continue;
			}
		}
		if (isspace((unsigned char)s[i])) {
			if (!out.empty() && out.back() != ' ') out.push_back(' ');
		} else {
			out.push_back(s[i]);
		}
		i++;
	}
	while (!out.empty() && out.back() == ' ') out.pop_back();
	return out;
}

// repoe-fork's stat_text order can differ from GGG's, pairing statsZh[i] with the wrong statsEn[i] (value backfill
// fails -> English). enText = repoe's English stat_text, same row order as the TC one: a line whose full text matches
// exactly one row takes that row's zh; the rest are paired by number signature, which cannot tell two lines with the
// same (or no) numbers apart. KEEP IN SYNC with _align_stats_zh() in tools/gen_atlas_zh.py.
static void AlignStatsZhOrder(ordered_json& n, const ordered_json* enText)
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
	if (enText && enText->is_array()) {
		std::vector<std::string> rowKey((std::min)(N, enText->size()));
		for (size_t j = 0; j < rowKey.size(); j++)
			if ((*enText)[j].is_string()) rowKey[j] = StatLineKey((*enText)[j].get<std::string>());
		for (size_t i = 0; i < N; i++) {
			const std::string k = StatLineKey(en[i].get<std::string>());
			int hit = -1, hits = 0;
			for (size_t j = 0; j < rowKey.size(); j++)
				if (!rowKey[j].empty() && rowKey[j] == k) { hit = (int)j; hits++; }
			if (hits == 1 && !used[hit]) { assign[i] = hit; used[hit] = true; }
		}
	}
	for (size_t oi = 0; oi < N; oi++) {
		const size_t i = order[oi];
		if (assign[i] >= 0 || enNums[i].empty()) continue;
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
                            const std::wstring& destDir, std::string* err, std::string* summary,
                            const std::string& enAtlasJson)
{
	auto fail = [&](const std::string& m) {
		if (err) *err = m;
		return false;
	};

	ordered_json ggg, tc, enRepoe;
	try {
		ggg = ordered_json::parse(gggDataJson);
		tc = ordered_json::parse(tcAtlasJson);
	} catch (const std::exception& e) {
		return fail(std::string(u8"對照資料解析失敗: ") + e.what());
	}
	// optional input: a broken English file only costs the text pairing
	if (!enAtlasJson.empty()) try {
		enRepoe = ordered_json::parse(enAtlasJson);
	} catch (const std::exception&) {
		enRepoe = nullptr;
	}
	auto enTextOf = [&](const std::string& k) -> const ordered_json* {
		if (!enRepoe.contains("passives") || !enRepoe["passives"].is_object()) return nullptr;
		auto eit = enRepoe["passives"].find(k);
		if (eit == enRepoe["passives"].end() || !eit->is_object() || !eit->contains("stat_text")) return nullptr;
		return &(*eit)["stat_text"];
	};

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
				AlignStatsZhOrder(n, enTextOf(std::to_string(id)));
			} else {
				// keep the English side so the file doubles as a full node list
				n["statsEn"] = statsEn;
			}
			outNodes[std::to_string(id)] = std::move(n);

			// Options of a multiple-choice node (3.30+): one entry per option, keyed by its effect id (a passive id
			// disjoint from node ids, so repoe / game files join it the same way). No name: an option is only its lines.
			if (v.contains("variantEffects") && v["variantEffects"].is_array())
				for (const auto& e : v["variantEffects"]) {
					if (!e.is_object() || !e.contains("effect") || !e["effect"].is_number_integer()) continue;
					std::string ek = std::to_string(e["effect"].get<int>());
					ordered_json o;
					o["en"] = "";
					o["statsEn"] = e.contains("stats") ? e["stats"] : ordered_json::array();
					auto eit = tcp.find(ek);
					if (eit != tcp.end() && eit->contains("stat_text")) {
						o["statsZh"] = (*eit)["stat_text"];
						AlignStatsZhOrder(o, enTextOf(ek));
					}
					outNodes[ek] = std::move(o);
				}
		}

		if (total == 0)
			return fail(u8"data.json 沒有任何節點，已中止");
		if (joined == 0)
			return fail(u8"沒有任何節點能對上繁中資料（hash 完全不合），保留舊對照");

		// Preserve season-official entries from the previous mapping ("fresh": zh patched from the season's game files
		// while repoe lagged); a repoe-only rebuild would wipe them. Where repoe now supplies zh, repoe wins and the marker is dropped.
		std::set<std::string> freshOut, freshNamesOut;
		bool prevProvisional = false;
		{
			// phase 1 (may throw): parse the previous mapping's fresh markers
			std::map<std::string, std::string> freshZh, freshName;
			std::string prevRaw;
			if (read_file_utf8(destDir + L"atlas_tree_zh.json", prevRaw)) try {
				ordered_json prev = ordered_json::parse(prevRaw);
				prevProvisional = prev.value("provisional", false);
				std::set<std::string> freshSet, freshNameSet;
				if (prev.contains("fresh") && prev["fresh"].is_array())
					for (const auto& f : prev["fresh"])
						if (f.is_string()) freshSet.insert(f.get<std::string>());
				if (prev.contains("freshNames") && prev["freshNames"].is_array())
					for (const auto& f : prev["freshNames"])
						if (f.is_string()) freshNameSet.insert(f.get<std::string>());
				if ((!freshSet.empty() || !freshNameSet.empty()) &&
					prev.contains("nodes") && prev["nodes"].is_object()) {
					for (const auto& [k, pv] : prev["nodes"].items()) {
						if (freshNameSet.count(k) && pv.contains("zh") && pv["zh"].is_string())
							freshName[k] = pv["zh"].get<std::string>();
						if (pv.contains("statsEn") && pv.contains("statsZh") &&
							pv["statsEn"].is_array() && pv["statsZh"].is_array() &&
							pv["statsEn"].size() == pv["statsZh"].size())
							for (size_t i = 0; i < pv["statsEn"].size(); i++)
								if (pv["statsEn"][i].is_string() && pv["statsZh"][i].is_string()) {
									std::string en = pv["statsEn"][i].get<std::string>();
									if (freshSet.count(en))
										freshZh[en] = pv["statsZh"][i].get<std::string>();
								}
					}
				}
			} catch (...) {
				// unreadable previous mapping: half-parsed markers must not
				// leak into the apply phase -> plain rebuild
				freshZh.clear();
				freshName.clear();
			}
			// phase 2 (no throw: outNodes was built above with known shapes):
			// re-apply markers onto the rebuilt nodes
			for (auto& [k, n] : outNodes.items()) {
				auto fn = freshName.find(k);
				if (fn != freshName.end() && !n.contains("zh")) {
					n["zh"] = fn->second;
					freshNamesOut.insert(k);
				}
				if (freshZh.empty() || !n.contains("statsEn") || !n["statsEn"].is_array())
					continue;
				const ordered_json se = n["statsEn"]; // copy: n may be re-shaped below
				bool aligned = n.contains("statsZh") && n["statsZh"].is_array() &&
				               n["statsZh"].size() == se.size();
				for (size_t i = 0; i < se.size(); i++) {
					if (!se[i].is_string()) continue;
					std::string en = se[i].get<std::string>();
					auto fz = freshZh.find(en);
					if (fz == freshZh.end()) continue;
					if (!aligned) { n["statsZh"] = se; aligned = true; }
					std::string cur = n["statsZh"][i].is_string()
						? n["statsZh"][i].get<std::string>() : std::string();
					if (cur != en && cur != fz->second)
						continue; // repoe supplied its own zh: repoe wins
					n["statsZh"][i] = fz->second;
					freshOut.insert(en);
				}
			}
		}

		ordered_json out;
		out["tag"] = tag;
		out["repoe"] = repoeVersion;
		out["joined"] = joined;
		out["total"] = total;
		if (!freshOut.empty())
			out["fresh"] = ordered_json(freshOut);
		if (!freshNamesOut.empty())
			out["freshNames"] = ordered_json(freshNamesOut);
		if (prevProvisional && (!freshOut.empty() || !freshNamesOut.empty()))
			out["provisional"] = true;
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

// AtlasUpdater

void AtlasUpdater::loadRecord()
{
	// version record (missing file = never updated -> any tag counts as new)
	std::string content;
	if (read_file_utf8(exeDir_ + L"Data\\atlas_version.json", content)) {
		try {
			ordered_json v = ordered_json::parse(content);
			tag_ = v.value("tag", std::string());
			sha_ = v.value("sha", std::string());
			repoe_ = v.value("repoe", std::string());
			lastCheckUtc_ = v.value("lastCheckUtc", 0ll);
			sha_ = AtlasInstalledSha(exeDir_, tag_, sha_); // an app update may have replaced the season's data
		} catch (...) {
			// corrupt record reads as "never updated": forces a fresh check
			tag_.clear();
			sha_.clear();
			repoe_.clear();
			lastCheckUtc_ = 0;
		}
	}
}

bool AtlasUpdater::doCheck(std::string* err)
{
	setPhase(AtlasUpdatePhase::Checking, u8"檢查輿圖資料更新中…");

	// newest release tag on GitHub
	std::string tagsBody;
	{
		HttpsClient api(kApiHost);
		if (!api.GetString(kTagsPath, tagsBody, err, &stop_)) return false;
	}
	std::string headBody;
	{
		HttpsClient api(kApiHost);
		std::string ignored; // best-effort: tag-only on failure
		if (!api.GetString(kHeadPath, headBody, &ignored, &stop_)) headBody.clear();
	}
	std::string latestTag, latestSha;
	bool fromHead = false;
	if (!PickAtlasRemote(tagsBody, headBody, &latestTag, &latestSha, &fromHead, err)) return false;

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

	AtlasUpdatePhase ph = AtlasUpdatePhase::UpToDate;
	std::string msg; // stays empty when the zh rebuild failed: keep the old mapping, retry on a future check
	bool zhRefreshed = false;
	if (AtlasNeedsUpdate(tag_, sha_, latestTag, latestSha, fromHead)) {
		ph = AtlasUpdatePhase::UpdateAvailable;
		msg = latestTag == tag_
			? u8"發現輿圖資料修訂 " + latestTag + u8"（" + latestSha.substr(0, 7) + u8"）"
			: u8"發現新賽季輿圖資料 " + latestTag + (tag_.empty() ? "" : u8"（目前 " + tag_ + u8"）");
	} else if (AtlasI18n cur; !repoeVer.empty() && repoeVer != repoe_ &&
	           // a mapping made from this season's game files beats one rebuilt from last season's repoe
	           !(AtlasI18n::SeasonLags(repoeVer, latestTag) && cur.LoadVersion(exeDir_, latestTag) && cur.Official())) {
		// same tree, newer translations: rebuild the zh mapping in place
		setPhase(AtlasUpdatePhase::Checking, u8"更新繁中對照資料中…");
		std::string zhErr;
		// tag_ == latestTag here: rebuild against the INSTALLED tree, not a tag-only fallback sha
		if (refreshZhMapping(latestTag, sha_.empty() ? latestTag : sha_, repoeVer, &zhErr)) {
			repoe_ = repoeVer;
			msg = u8"繁中對照已更新（repoe " + repoeVer + u8"）";
			zhRefreshed = true;
		}
	} else {
		msg = u8"輿圖資料已是最新（" + latestTag + u8"）";
	}

	// record first: Poll() readers treat the phase change as "check finished"
	lastCheckUtc_ = DataUpdaterNowFiletime();
	saveVersionRecord();
	{
		std::lock_guard<std::mutex> lk(stMx_);
		st_.phase = ph;
		st_.message = msg;
		if (zhRefreshed) st_.zhRefreshed = true;
	}
	return true;
}

// Re-fetches the English data.json plus the TC Atlas.json and rebuilds Data/atlas_tree_zh.json (catch-up when repoe-fork publishes translations after a season update).
bool AtlasUpdater::refreshZhMapping(const std::string& tag, const std::string& ref, const std::string& repoeVer, std::string* err)
{
	std::string dataJson;
	{
		HttpsClient raw(kRawHost);
		std::wstring path = std::wstring(kRawBase) + widen(ref) + L"/data.json";
		if (!raw.GetString(path, dataJson, err, &stop_)) return false;
	}
	std::string tcJson, enJson;
	{
		HttpsClient repoe(kRepoeHost);
		if (!repoe.GetString(kRepoeTcAtlasPath, tcJson, err, &stop_)) return false;
		std::string ignored;
		if (!repoe.GetString(kRepoeEnAtlasPath, enJson, &ignored, &stop_)) enJson.clear();
	}
	// write the refreshed mapping into the season's own folder (versioned
	// layout); ResolveDataDir falls back to flat Data/ for a legacy install
	AtlasVersionIndex idx;
	idx.Load(exeDir_);
	std::wstring dest = idx.ResolveDataDir(exeDir_, tag);
	std::string summary;
	return GenerateAtlasZhMapping(dataJson, tcJson, tag, repoeVer, dest, err, &summary, enJson);
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

	// download by commit sha: an untagged master revision shares the tag name
	const std::string ref = latestSha_.empty() ? tag : latestSha_;
	setPhase(AtlasUpdatePhase::Downloading, u8"下載 " + tag + u8" 資料中…");
	{
		std::lock_guard<std::mutex> lk(stMx_);
		st_.filesDone = 0;
		st_.filesTotal = 0;
	}

	std::wstring cacheDir = exeDir_ + L"PobTools\\cache\\atlas_update\\" + widen(ref) + L"\\";
	SHCreateDirectoryExW(nullptr, (cacheDir + L"assets").c_str(), nullptr);

	HttpsClient raw(kRawHost);

	// data.json first; the sprite list decides which sheets we actually need
	std::string dataJson;
	{
		std::wstring path = std::wstring(kRawBase) + widen(ref) + L"/data.json";
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
			const ordered_json* lv = AtlasSpriteLevel(cat, sec); // the level the import reads
			if (!lv) continue;
			std::string base = cdn_base_name(lv->value("filename", std::string()));
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
			std::wstring path = std::wstring(kRawBase) + widen(ref) + L"/assets/" + widen(base);
			if (!raw.Get(path, bytes, err, &stop_)) return false;
			if (!DataUpdaterWriteCacheFile(dst, bytes))
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
	if (!latestSha_.empty()) {
		ordered_json stamp;
		stamp["sha"] = latestSha_;
		// a stale stamp would outrank the index and re-offer this update forever
		if (!write_file_utf8(seasonDir + L"version.json", stamp.dump())) DeleteFileW((seasonDir + L"version.json").c_str());
	}

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
		std::string enJson;
		if (!repoe.GetString(kRepoeEnAtlasPath, enJson, &ignored, &stop_)) enJson.clear();
		if (repoe.GetString(kRepoeTcAtlasPath, tcJson, &zhErr, &stop_) &&
			GenerateAtlasZhMapping(dataJson, tcJson, tag, repoeVer, seasonDir, &zhErr, &zhSum, enJson)) {
			repoe_ = repoeVer;
			zhNote = u8"；" + zhSum;
		} else {
			zhNote = u8"；繁中對照暫時無法更新，沿用舊對照";
		}
	}

	// Register the season, make it active, prune to the newest two LEAGUES: all revisions of the current league stay,
	// the previous one keeps only its final revision, older folders are deleted.
	int keptSeasons = 0;
	{
		AtlasVersionIndex idx;
		idx.Load(exeDir_);
		AtlasVersionEntry e;
		e.tag = tag; e.sha = latestSha_; e.repoe = repoeVer;
		idx.UpsertActive(e);
		std::vector<std::string> dropped = idx.PruneSeasons(2);
		idx.SetLastCheckUtc(DataUpdaterNowFiletime());
		if (!idx.Save(exeDir_)) {
			// The files landed but the index did not: the new season is on disk and invisible; say so (looks like "the update did nothing").
			PobLog::Error("atlas", u8"新賽季資料已下載，但 atlas_index.json 存檔失敗");
		}
		keptSeasons = (int)idx.Versions().size();
		for (const std::string& d : dropped)
			remove_dir_with_sub(AtlasVersionIndex::VersionDir(exeDir_, d), L"atlas");
	}

	tag_ = tag;
	sha_ = latestSha_;
	lastCheckUtc_ = DataUpdaterNowFiletime();
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

// CLI wrappers

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
	u.loadRecord(); // same record + installed-sha rules as the GUI

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
                  const std::wstring& enJsonPath, const std::wstring& exeDir)
{
	attach_parent_console();

	std::string dataJson, tcJson, enJson;
	if (!enJsonPath.empty() && !read_file_utf8(enJsonPath, enJson)) {
		printf("FAIL: cannot read English Atlas.json\n");
		return 1;
	}
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
	if (!GenerateAtlasZhMapping(dataJson, tcJson, "local", "local", dest, &err, &summary, enJson)) {
		printf("FAIL: %s\n", err.c_str());
		return 1;
	}
	printf("%s\n", summary.c_str());
	return 0;
}
