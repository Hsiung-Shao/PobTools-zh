#include "atlas_i18n.h"
#include "atlas_stat_agg.h"      // ToLowerAscii / StripStatMarkup for the search keys
#include "atlas_tree_data.h"
#include "atlas_version_index.h" // season resolution (versioned data layout)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <json.hpp> // nlohmann::ordered_json (deps/nlohmann)

using nlohmann::ordered_json;

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

bool AtlasI18n::Load(const std::wstring& exeDir)
{
	return LoadVersion(exeDir, std::string()); // active season
}

bool AtlasI18n::LoadVersion(const std::wstring& exeDir, const std::string& tag)
{
	nameById_.clear();
	statByEn_.clear();
	freshStats_.clear();
	freshNames_.clear();
	provisional_ = false;
	note_.clear();
	repoe_.clear();
	loaded_ = false;

	AtlasVersionIndex idx;
	idx.Load(exeDir);
	std::wstring dir = idx.ResolveDataDir(exeDir, tag);

	std::string content;
	if (!read_file_utf8(dir + L"atlas_tree_zh.json", content)) return false;

	try {
		ordered_json doc = ordered_json::parse(content);
		if (!doc.contains("nodes") || !doc["nodes"].is_object()) return false;

		for (const auto& [key, v] : doc["nodes"].items()) {
			// English-only entry (repoe behind). An option entry has lines but no name.
			if (!v.contains("zh") && !v.contains("statsZh")) continue;
			int id = 0;
			try {
				id = std::stoi(key);
			} catch (...) {
				continue;
			}
			std::string zh = v.value("zh", std::string());
			if (!zh.empty()) nameById_.emplace(id, std::move(zh));

			// stat lines join by full English text; only aligned lists are safe
			if (v.contains("statsEn") && v.contains("statsZh") &&
				v["statsEn"].is_array() && v["statsZh"].is_array() &&
				v["statsEn"].size() == v["statsZh"].size()) {
				for (size_t i = 0; i < v["statsEn"].size(); i++) {
					if (v["statsEn"][i].is_string() && v["statsZh"][i].is_string())
						statByEn_.emplace(v["statsEn"][i].get<std::string>(),
						                  v["statsZh"][i].get<std::string>());
				}
			}
		}

		// entries whose zh came from the season's own game files (ggpk patch)
		// while repoe lagged; PruneStaleTranslations must not drop them
		if (doc.contains("fresh") && doc["fresh"].is_array())
			for (const auto& f : doc["fresh"])
				if (f.is_string()) freshStats_.insert(f.get<std::string>());
		if (doc.contains("freshNames") && doc["freshNames"].is_array())
			for (const auto& f : doc["freshNames"])
				if (f.is_string()) freshNames_.insert(atoi(f.get<std::string>().c_str()));
		provisional_ = doc.value("provisional", false);

		repoe_ = doc.value("repoe", std::string());
		note_ = doc.value("tag", std::string("?")) + " / repoe " + (repoe_.empty() ? "?" : repoe_);
		loaded_ = !nameById_.empty();
		return loaded_;
	} catch (...) {
		nameById_.clear();
		statByEn_.clear();
		freshStats_.clear();
		return false;
	}
}

bool AtlasI18n::LagsSeason(const std::string& tag) const
{
	int ra = 0, rb = 0, ta = 0, tb = 0;
	if (sscanf_s(repoe_.c_str(), "%d.%d", &ra, &rb) != 2 || sscanf_s(tag.c_str(), "%d.%d", &ta, &tb) != 2)
		return false;   // unknown on either side: no claim
	return ra < ta || (ra == ta && rb < tb);
}

const std::string& AtlasI18n::NodeName(int id, const std::string& en) const
{
	auto it = nameById_.find(id);
	return it != nameById_.end() ? it->second : en;
}

const std::string& AtlasI18n::StatLine(const std::string& en) const
{
	auto it = statByEn_.find(en);
	return it != statByEn_.end() ? it->second : en;
}

// ---- whole-tree node search ---------------------------------------------------

std::vector<std::string> AtlasNodeSearchKeys(const AtlasTreeData& d, const AtlasI18n* zh)
{
	std::vector<std::string> keys(d.nodes.size());
	for (size_t i = 0; i < d.nodes.size(); i++) {
		const AtlasNode& n = d.nodes[i];
		if (n.kind == kAtlasStart) continue;
		std::string k = n.name;
		if (zh) k += "\n" + zh->NodeName(n.id, n.name);
		for (const std::string& s : n.AllStats()) {   // every option of a multiple-choice node
			k += "\n" + StripStatMarkup(s);
			if (zh) k += "\n" + StripStatMarkup(zh->StatLine(s));
		}
		keys[i] = ToLowerAscii(k);
	}
	return keys;
}

std::vector<int> AtlasSearchNodes(const std::vector<std::string>& keys, const std::string& query)
{
	std::vector<int> hits;
	const size_t a = query.find_first_not_of(" \t"), b = query.find_last_not_of(" \t");
	if (a == std::string::npos) return hits;
	const std::string needle = ToLowerAscii(query.substr(a, b - a + 1));
	for (size_t i = 0; i < keys.size(); i++)
		if (keys[i].find(needle) != std::string::npos) hits.push_back((int)i);
	return hits;
}

// ---- self-test (folded into --atlas-selftest) --------------------------------

namespace {

struct SearchReport {
	std::string& text;
	int failures = 0;

	void check(bool ok, const char* what, const std::string& detail = std::string())
	{
		text += ok ? "PASS  " : "FAIL  ";
		text += what;
		if (!detail.empty()) { text += "  ("; text += detail; text += ")"; }
		text += "\n";
		if (!ok) failures++;
	}
	void note(const std::string& s) { text += "      " + s + "\n"; }
};

// First line of a stat with markup removed (stats may span several lines).
std::string FirstStatLine(const std::string& stat)
{
	std::string s = StripStatMarkup(stat);
	return s.substr(0, s.find('\n'));
}

bool HasIdx(const std::vector<int>& v, int idx)
{
	for (int i : v) if (i == idx) return true;
	return false;
}

} // namespace

int RunAtlasSearchSelfTest(const std::wstring& exeDir, std::string& out)
{
	SearchReport rep{ out };
	out += "\n-- atlas node search --\n";

	AtlasTreeData d;
	std::string err;
	if (!d.Load(exeDir, &err)) {
		rep.check(false, u8"載入輿圖天賦樹資料失敗,搜尋檢查無法進行", err);
		return rep.failures;
	}
	AtlasI18n zh;
	if (!zh.Load(exeDir)) {
		rep.check(false, u8"載入輿圖中文對照表失敗,搜尋檢查無法進行");
		return rep.failures;
	}

	// Pick a non-start, unallocated node whose zh name and at least one stat differ from English.
	int pick = -1;
	std::string enName, zhName, enFrag, zhFrag;
	for (int i = 0; i < (int)d.nodes.size() && pick < 0; i++) {
		const AtlasNode& n = d.nodes[i];
		if (n.kind == kAtlasStart || n.alloc || n.name.empty()) continue;
		const std::string& zn = zh.NodeName(n.id, n.name);
		if (zn == n.name) continue;
		for (const std::string& st : n.stats) {
			const std::string& zs = zh.StatLine(st);
			if (zs == st) continue;
			std::string ef = FirstStatLine(st), zf = FirstStatLine(zs);
			if (ef.empty() || zf.empty() || ef == zf) continue;
			pick = i; enName = n.name; zhName = zn; enFrag = ef; zhFrag = zf;
			break;
		}
	}
	if (pick < 0) {
		rep.check(false, u8"資料中找不到中英文名稱與詞條都不同的未配置節點,無法測試搜尋");
		return rep.failures;
	}
	rep.note("picked node id " + std::to_string(d.nodes[pick].id) + " \"" + enName + "\" / \"" + zhName + "\"");
	rep.note("en fragment \"" + enFrag + "\" / zh fragment \"" + zhFrag + "\"");

	const std::vector<std::string> keys = AtlasNodeSearchKeys(d, &zh);
	rep.check(keys.size() == d.nodes.size(), u8"搜尋鍵數量與節點數一致",
	          std::to_string(keys.size()) + " vs " + std::to_string(d.nodes.size()));

	rep.check(HasIdx(AtlasSearchNodes(keys, enName), pick), u8"S1 英文名稱找到未配置節點");
	rep.check(HasIdx(AtlasSearchNodes(keys, zhName), pick), u8"S2 中文名稱找到同一節點");
	rep.check(HasIdx(AtlasSearchNodes(keys, enFrag), pick), u8"S3 英文詞條內文可搜");
	rep.check(HasIdx(AtlasSearchNodes(keys, zhFrag), pick), u8"S4 中文詞條內文可搜");

	{
		std::string up = enName, lo = enName;
		for (char& c : up) if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
		for (char& c : lo) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
		std::vector<int> ru = AtlasSearchNodes(keys, up), rl = AtlasSearchNodes(keys, lo);
		rep.check(!ru.empty() && ru == rl && HasIdx(ru, pick), u8"S5 大小寫不影響結果",
		          std::to_string(ru.size()) + " vs " + std::to_string(rl.size()));
	}

	rep.check(AtlasSearchNodes(keys, "").empty() && AtlasSearchNodes(keys, "   ").empty(),
	          u8"S6 空字串不標示任何節點");
	rep.check(AtlasSearchNodes(keys, "zzqqxx_no_such_node").empty(), u8"S7 查無時回傳 0 筆");

	return rep.failures;
}
