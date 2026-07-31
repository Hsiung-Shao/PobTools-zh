#include "atlas_maps.h"

#include "atlas_stat_agg.h" // ToLowerAscii

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <json.hpp> // nlohmann::ordered_json (deps/nlohmann)

#include <algorithm>

using nlohmann::ordered_json;

static bool map_read_file(const std::wstring& path, std::string& out)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	LARGE_INTEGER size{};
	bool ok = false;
	if (GetFileSizeEx(h, &size) && size.QuadPart >= 0 && size.QuadPart < (1ll << 26)) {
		out.resize((size_t)size.QuadPart);
		DWORD read = 0;
		ok = out.empty() || (ReadFile(h, &out[0], (DWORD)out.size(), &read, nullptr) && read == out.size());
		if (!ok) out.clear();
	}
	CloseHandle(h);
	return ok;
}

bool AtlasMapDb::Load(const std::wstring& exeDir, std::string* err)
{
	defs_.clear();
	byId_.clear();
	source_.clear();
	series_.clear();

	std::string body;
	if (!map_read_file(exeDir + L"Data\\atlas_maps_poe1.json", body)) {
		if (err) *err = u8"找不到 Data\\atlas_maps_poe1.json";
		return false;
	}
	try {
		ordered_json doc = ordered_json::parse(body);
		if (doc.value("format", std::string()) != "pobtools-atlas-maps") {
			if (err) *err = u8"atlas_maps_poe1.json 的 format 欄位不符";
			return false;
		}
		source_ = doc.value("source", std::string());
		series_ = doc.value("series", std::string());
		const auto& arr = doc["maps"];
		if (!arr.is_array()) {
			if (err) *err = u8"atlas_maps_poe1.json 缺少 maps 陣列";
			return false;
		}
		defs_.reserve(arr.size());
		for (const auto& m : arr) {
			if (!m.is_object()) continue;
			AtlasMapDef d;
			d.id = m.value("id", std::string());
			d.enArea = m.value("enArea", std::string());
			d.zhArea = m.value("zhArea", std::string());
			if (d.id.empty() || d.enArea.empty()) continue;
			d.enItem = m.value("enItem", std::string());
			d.zhItem = m.value("zhItem", std::string());
			d.region = m.value("region", std::string());
			d.art = m.value("art", std::string());
			d.tier = m.value("tier", 0);
			std::string kind = m.value("kind", std::string("normal"));
			d.kind = kind == "unique" ? AtlasMapDef::kUnique
			       : kind == "offatlas" ? AtlasMapDef::kOffAtlas
			                            : AtlasMapDef::kNormal;
			if (d.zhArea.empty()) d.zhArea = d.enArea; // untranslated: show English
			if (d.zhItem.empty()) d.zhItem = d.enItem;

			// Both names go into one key per locale so a query matches whichever
			// name the user knows; the tier is appended as "t14" so a tier query
			// narrows the list too.
			std::string tierTag = d.tier > 0 ? " t" + std::to_string(d.tier) : std::string();
			d.keyEn = ToLowerAscii(d.enArea + "\n" + d.enItem + tierTag);
			d.keyZh = ToLowerAscii(d.zhArea + "\n" + d.zhItem + tierTag);
			d.keyEnCompact = FuzzyCompactKey(d.keyEn);
			d.keyZhCompact = FuzzyCompactKey(d.keyZh);
			defs_.push_back(std::move(d));
		}
	} catch (const std::exception& e) {
		defs_.clear();
		if (err) *err = std::string(u8"atlas_maps_poe1.json 解析失敗: ") + e.what();
		return false;
	}
	if (defs_.empty()) {
		if (err) *err = u8"atlas_maps_poe1.json 沒有任何地圖";
		return false;
	}
	for (int i = 0; i < (int)defs_.size(); i++) byId_.emplace(defs_[i].id, i);
	for (const AtlasMapDef& d : defs_)
		if (d.kind != AtlasMapDef::kUnique && d.tier > 0 &&
		    std::find(tiers_.begin(), tiers_.end(), d.tier) == tiers_.end())
			tiers_.push_back(d.tier);
	std::sort(tiers_.begin(), tiers_.end());
	return true;
}

const AtlasMapDef* AtlasMapDb::ById(const std::string& id) const
{
	auto it = byId_.find(id);
	return it == byId_.end() ? nullptr : &defs_[it->second];
}

std::string AtlasMapDb::SanitizeOne(const std::string& id) const
{
	if (id.empty() || !available()) return id;
	return ById(id) ? id : std::string();
}

int AtlasMapDb::MatchScore(const AtlasMapDef& d, const FuzzyQuery& q) const
{
	if (q.empty()) return 1; // everything matches; caller keeps the natural order
	int best = FuzzyNameScore(d.keyZh, d.keyZhCompact, q);
	int en = FuzzyNameScore(d.keyEn, d.keyEnCompact, q);
	return en > best ? en : best;
}

// ---- self-test (folded into --atlas-selftest) --------------------------------

namespace {

struct MapReport {
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

} // namespace

int RunAtlasMapSelfTest(const std::wstring& exeDir, std::string& out)
{
	MapReport rep{ out };
	out += "\n-- atlas maps --\n";

	AtlasMapDb db;
	std::string derr;
	if (!db.Load(exeDir, &derr)) {
		rep.note("map catalogue absent - checks skipped (" + derr + ")");
		// Without a catalogue a saved pick must survive untouched, exactly as
		// the scarab and astrolabe lists do.
		rep.check(db.SanitizeOne("MapWorldsShrine") == "MapWorldsShrine",
		          "without a catalogue SanitizeOne leaves the saved map untouched");
		return rep.failures;
	}

	rep.note("catalogue: " + std::to_string(db.All().size()) + " maps, series " +
	         db.Series() + ", source " + db.Source());
	rep.check(db.All().size() == 136, "catalogue holds 136 maps",
	          std::to_string(db.All().size()));

	{
		int normal = 0, off = 0, uniq = 0, noRegion = 0, noArt = 0, untranslated = 0;
		for (const AtlasMapDef& d : db.All()) {
			switch (d.kind) {
			case AtlasMapDef::kNormal:   normal++; break;
			case AtlasMapDef::kOffAtlas: off++;    break;
			case AtlasMapDef::kUnique:   uniq++;   break;
			}
			if (d.region.empty()) noRegion++;
			if (d.art.empty()) noArt++;
			if (d.zhArea == d.enArea) untranslated++;
		}
		rep.check(normal == 110 && off == 3 && uniq == 23, "kind split is 110/3/23",
		          std::to_string(normal) + "/" + std::to_string(off) + "/" + std::to_string(uniq));
		rep.check(noRegion == 0, "every map names a quadrant", std::to_string(noRegion));
		rep.check(noArt == 0, "every map has an art path", std::to_string(noArt));
		rep.check(untranslated == 0, "every area name is translated", std::to_string(untranslated));
	}
	{
		// Regular maps must carry a usable tier; unique maps legitimately do not.
		int badTier = 0;
		for (const AtlasMapDef& d : db.All())
			if (d.kind != AtlasMapDef::kUnique && (d.tier < 1 || d.tier > 17)) badTier++;
		rep.check(badTier == 0, "every regular map has a tier in [1,17]", std::to_string(badTier));

		// The picker's tier filter is built from this list.
		const std::vector<int>& tiers = db.TiersPresent();
		std::string got;
		for (int t : tiers) got += (got.empty() ? "" : ",") + std::to_string(t);
		bool ascending = true, inRange = true;
		for (size_t i = 0; i < tiers.size(); i++) {
			if (i && tiers[i] <= tiers[i - 1]) ascending = false;
			if (tiers[i] < 1 || tiers[i] > 17) inRange = false;
		}
		rep.check(!tiers.empty() && ascending && inRange,
		          "TiersPresent is a sorted, de-duplicated, in-range list", got);
		// Every regular map must fall in one of the offered tiers, or the filter
		// would hide maps that have no way of being reached.
		int unreachable = 0;
		for (const AtlasMapDef& d : db.All())
			if (d.kind != AtlasMapDef::kUnique &&
			    std::find(tiers.begin(), tiers.end(), d.tier) == tiers.end())
				unreachable++;
		rep.check(unreachable == 0, "every regular map is reachable through a tier filter",
		          std::to_string(unreachable));
	}
	{
		rep.check(db.SanitizeOne("") == "", "an empty pick stays empty");
		rep.check(db.SanitizeOne("NotAMapAtAll").empty(), "sanitize drops an unknown map id");
		const std::string& first = db.All()[0].id;
		rep.check(db.SanitizeOne(first) == first, "sanitize keeps a known map id");
	}
	{
		auto hitCount = [&](const char* qs) {
			FuzzyQuery q = MakeFuzzyQuery(qs);
			int n = 0;
			for (const AtlasMapDef& d : db.All()) if (db.MatchScore(d, q)) n++;
			return n;
		};
		// Fixture by id so a re-wording in a future season cannot silently void
		// the check.
		const AtlasMapDef* shrine = db.ById("MapWorldsShrine");
		rep.check(shrine != nullptr, "search fixture present");
		if (shrine) {
			auto score = [&](const char* qs) { return db.MatchScore(*shrine, MakeFuzzyQuery(qs)); };
			rep.check(score(u8"奇術秘殿") > 0, "zh area name matches");
			rep.check(score("shrine") > 0, "en name matches");
			// The item name differs from the area name on 8 maps; both must hit.
			const AtlasMapDef* sanct = db.ById("MapWorldsSanctuary");
			rep.check(sanct && db.MatchScore(*sanct, MakeFuzzyQuery(u8"庇護聖所")) > 0 &&
			              db.MatchScore(*sanct, MakeFuzzyQuery(u8"聖所地圖")) > 0,
			          "a map whose area and item names differ is findable by both");
		}
		rep.check(hitCount("") == (int)db.All().size(), "empty query matches everything");
		rep.check(hitCount("zzzzqqqq") == 0, "nonsense query matches nothing");
		int t14 = hitCount("t14");
		rep.check(t14 > 0 && t14 < (int)db.All().size(), "tier query narrows the list",
		          std::to_string(t14));
	}

	return rep.failures;
}
