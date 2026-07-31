#include "atlas_astrolabes.h" // pulls in atlas_persist.h for AstrolabePlacement

#include "atlas_stat_agg.h"  // ToLowerAscii / StripStatMarkup

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <json.hpp> // nlohmann::ordered_json (deps/nlohmann)

#include <algorithm>

using nlohmann::ordered_json;

// Same local file helper as the sibling atlas modules; kept local so this stays
// a leaf that does not depend on the persistence layer.
static bool astro_read_file(const std::wstring& path, std::string& out)
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

static std::vector<std::string> parse_lines(const ordered_json& arr)
{
	std::vector<std::string> out;
	if (!arr.is_array()) return out;
	for (const auto& s : arr)
		if (s.is_string()) out.push_back(s.get<std::string>());
	return out;
}

bool AstrolabeDb::Load(const std::wstring& exeDir, std::string* err)
{
	defs_.clear();
	regions_.clear();
	byId_.clear();
	regionById_.clear();
	source_.clear();

	std::string body;
	if (!astro_read_file(exeDir + L"Data\\astrolabes_poe1.json", body)) {
		if (err) *err = u8"找不到 Data\\astrolabes_poe1.json";
		return false;
	}
	try {
		ordered_json doc = ordered_json::parse(body);
		if (doc.value("format", std::string()) != "pobtools-astrolabes") {
			if (err) *err = u8"astrolabes_poe1.json 的 format 欄位不符";
			return false;
		}
		source_ = doc.value("source", std::string());

		if (doc.contains("regions") && doc["regions"].is_array()) {
			for (const auto& r : doc["regions"]) {
				if (!r.is_object()) continue;
				AtlasQuadrant q;
				q.id = r.value("id", std::string());
				if (q.id.empty()) continue;
				q.vaultEn = r.value("vaultEn", std::string());
				q.vaultZh = r.value("vaultZh", std::string());
				if (q.vaultZh.empty()) q.vaultZh = q.vaultEn;
				regions_.push_back(std::move(q));
			}
		}

		const auto& arr = doc["astrolabes"];
		if (!arr.is_array()) {
			if (err) *err = u8"astrolabes_poe1.json 缺少 astrolabes 陣列";
			return false;
		}
		defs_.reserve(arr.size());
		for (const auto& s : arr) {
			if (!s.is_object()) continue;
			AstrolabeDef d;
			d.id = s.value("id", std::string());
			d.en = s.value("en", std::string());
			d.zh = s.value("zh", std::string());
			if (d.id.empty() || d.en.empty()) continue;
			d.mechanic = s.value("mechanic", std::string());
			d.art = s.value("art", std::string());
			d.enabled = s.value("enabled", true);
			if (s.contains("descEn")) d.descEn = parse_lines(s["descEn"]);
			if (s.contains("descZh")) d.descZh = parse_lines(s["descZh"]);
			if (d.descZh.empty()) d.descZh = d.descEn; // untranslated: show English
			d.keyEn = ToLowerAscii(d.en);
			d.keyZh = ToLowerAscii(d.zh);
			d.keyEnCompact = FuzzyCompactKey(d.keyEn);
			d.keyZhCompact = FuzzyCompactKey(d.keyZh);
			// Effects are searched as rendered, so a query matches what the user
			// can actually see (GGG markup like [ContainsAbyss|深淵] is stripped).
			for (const std::string& line : d.descEn) d.descKeyEn += StripStatMarkup(line) + "\n";
			for (const std::string& line : d.descZh) d.descKeyZh += StripStatMarkup(line) + "\n";
			d.descKeyEn = ToLowerAscii(d.descKeyEn);
			d.descKeyZh = ToLowerAscii(d.descKeyZh);
			defs_.push_back(std::move(d));
		}
	} catch (const std::exception& e) {
		defs_.clear();
		regions_.clear();
		if (err) *err = std::string(u8"astrolabes_poe1.json 解析失敗: ") + e.what();
		return false;
	}
	if (defs_.empty() || regions_.empty()) {
		if (err) *err = u8"astrolabes_poe1.json 沒有星盤或半區資料";
		defs_.clear();
		regions_.clear();
		return false;
	}
	for (int i = 0; i < (int)defs_.size(); i++) byId_.emplace(defs_[i].id, i);
	for (int i = 0; i < (int)regions_.size(); i++) regionById_.emplace(regions_[i].id, i);
	return true;
}

const AstrolabeDef* AstrolabeDb::ById(const std::string& id) const
{
	auto it = byId_.find(id);
	return it == byId_.end() ? nullptr : &defs_[it->second];
}

const AtlasQuadrant* AstrolabeDb::RegionById(const std::string& id) const
{
	auto it = regionById_.find(id);
	return it == regionById_.end() ? nullptr : &regions_[it->second];
}

AstrolabeAddResult AstrolabeDb::CanPlace(const std::vector<AstrolabePlacement>& cur,
                                         const std::string& region, const std::string& id) const
{
	AstrolabeAddResult r;
	if (!RegionById(region)) { r.code = AstrolabeAdd::kUnknownRegion; return r; }
	if (!ById(id)) { r.code = AstrolabeAdd::kUnknownAstrolabe; return r; }
	for (const AstrolabePlacement& p : cur) {
		if (p.region != region) continue;
		r.code = AstrolabeAdd::kRegionOccupied;
		r.occupant = ById(p.id);
		return r;
	}
	return r;
}

std::vector<AstrolabePlacement> AstrolabeDb::Sanitize(const std::vector<AstrolabePlacement>& cur,
                                                      std::string* note) const
{
	if (note) note->clear();
	// No catalogue -> no basis to judge. Passing the list through unchanged is
	// deliberate: dropping it would let a missing data file silently erase the
	// user's saved placements on the next save.
	if (!available()) return cur;

	std::vector<AstrolabePlacement> out;
	int unknownRegion = 0, unknownAstro = 0, occupied = 0;
	for (const AstrolabePlacement& p : cur) {
		AstrolabeAddResult r = CanPlace(out, p.region, p.id);
		if (r.ok()) { out.push_back(p); continue; }
		switch (r.code) {
		case AstrolabeAdd::kUnknownRegion:    unknownRegion++; break;
		case AstrolabeAdd::kUnknownAstrolabe: unknownAstro++;  break;
		default:                              occupied++;      break;
		}
	}
	if (note) {
		if (unknownRegion) *note += u8"，忽略 " + std::to_string(unknownRegion) + u8" 個未知半區";
		if (unknownAstro)  *note += u8"，忽略 " + std::to_string(unknownAstro) + u8" 個未知星盤";
		if (occupied)      *note += u8"，忽略 " + std::to_string(occupied) + u8" 個重複配置的半區";
	}
	return out;
}

int AstrolabeDb::MatchScore(const AstrolabeDef& d, const FuzzyQuery& q) const
{
	if (q.empty()) return 1; // everything matches; caller keeps the natural order

	int best = FuzzyNameScore(d.keyZh, d.keyZhCompact, q);
	int en = FuzzyNameScore(d.keyEn, d.keyEnCompact, q);
	if (en > best) best = en;
	if (best) return best;

	for (const std::string* k : { &d.descKeyZh, &d.descKeyEn }) {
		int s = FuzzyTextScore(*k, q);
		if (s) return s;
	}
	return 0;
}

// ---- self-test (folded into --atlas-selftest) --------------------------------

namespace {

struct AstroReport {
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

int RunAstrolabeSelfTest(const std::wstring& exeDir, std::string& out)
{
	AstroReport rep{ out };
	out += "\n-- astrolabes --\n";

	// --- persistence: must hold whether or not the catalogue is present,
	// because this is what protects existing user files.
	{
		// A project saved before astrolabes existed loads with the field empty.
		AtlasBuildFile f;
		bool ok = f.ParseDoc(u8"{\"builds\":[{\"name\":\"a\",\"alloc\":[1,2]}]}");
		rep.check(ok && f.builds.size() == 1 && f.builds[0].astrolabes.empty(),
		          "pre-astrolabe project loads with an empty placement list");

		// A project that uses none of the new fields must serialize exactly as
		// before, so untouched user files stay byte-identical.
		std::string doc = f.SerializeDoc();
		rep.check(doc.find("astrolabes") == std::string::npos &&
		          doc.find("\"map\"") == std::string::npos &&
		          doc.find("mapTier") == std::string::npos,
		          "unused astrolabes/map/mapTier are omitted from the document", doc);

		// Full state survives save -> load, including quadrant order.
		AtlasBuildFile g;
		g.ParseDoc(u8"{\"builds\":[{\"name\":\"a\",\"alloc\":[7]}]}");
		g.builds[0].astrolabes = {
			{ "NorthWest", "Metadata/Items/Currency/AstrolabeBreach" },
			{ "SouthEast", "Metadata/Items/Currency/AstrolabeAbyss" },
		};
		g.builds[0].mapId = "MapWorldsShrine";
		// The planned tier is stored INDEPENDENTLY of the map: a Voidstone lifts
		// a whole quadrant to T16, and some strategies target a specific tier, so
		// T3 here alongside a map whose own tier is 5 is a legitimate state.
		g.builds[0].mapTier = 16;

		// The mirror of the omission check above. Without this, "the keys are
		// absent when unused" would also pass if the writer never emitted them
		// at all, and the round-trip below would be the only thing standing
		// between a silent data loss and the user.
		std::string used = g.SerializeDoc();
		rep.check(used.find("\"astrolabes\"") != std::string::npos &&
		          used.find("\"NorthWest\"") != std::string::npos &&
		          used.find("\"map\"") != std::string::npos &&
		          used.find("\"mapTier\":16") != std::string::npos,
		          "a project that USES the fields writes them", used);

		AtlasBuildFile h;
		bool rt = h.ParseDoc(g.SerializeDoc());
		rep.check(rt && h.builds.size() == 1 &&
		          h.builds[0].astrolabes.size() == 2 &&
		          h.builds[0].astrolabes[0].region == "NorthWest" &&
		          h.builds[0].astrolabes[0].id == "Metadata/Items/Currency/AstrolabeBreach" &&
		          h.builds[0].astrolabes[1].region == "SouthEast" &&
		          h.builds[0].mapId == "MapWorldsShrine" &&
		          h.builds[0].mapTier == 16,
		          "astrolabe placements + main map + planned tier round-trip");

		// The share code carries them, and the PTAT1 prefix is unchanged on
		// purpose — bumping it would make older builds reject new codes.
		AtlasBuildEntry src;
		src.name = u8"星盤測試";
		src.alloc = { 11, 22 };
		src.astrolabes = { { "NorthEast", "Metadata/Items/Currency/AstrolabeRitual" } };
		src.mapId = "MapWorldsIceberg";
		src.mapTier = 14;
		AtlasBuildEntry back;
		std::string perr;
		std::string code = AtlasBuildShareCode(src, "test");
		rep.check(code.compare(0, 6, "PTAT1|") == 0 &&
		          AtlasParseShareCode(code, &back, &perr) &&
		          back.astrolabes.size() == 1 &&
		          back.astrolabes[0].region == "NorthEast" &&
		          back.astrolabes[0].id == src.astrolabes[0].id &&
		          back.mapId == src.mapId && back.mapTier == 14,
		          "share code carries astrolabes + main map + planned tier", perr);

		AtlasBuildEntry noneUsed;
		noneUsed.name = "n";
		noneUsed.alloc = { 1 };
		std::string ex = AtlasExportJson(noneUsed, "v");
		rep.check(ex.find("astrolabes") == std::string::npos &&
		          ex.find("\"map\"") == std::string::npos &&
		          ex.find("mapTier") == std::string::npos,
		          "export omits unused astrolabes/map/mapTier too", ex);

		// A hand-written document, not one this build produced: the reader must
		// cope with a file edited by hand or written by a future version.
		AtlasBuildFile m;
		bool handOk = m.ParseDoc(
			u8"{\"format\":\"pobtools-atlas-builds\",\"builds\":[{\"name\":\"h\",\"alloc\":[1],"
			u8"\"astrolabes\":{\"SouthWest\":\"Metadata/Items/Currency/AstrolabeLegion\"},"
			u8"\"map\":\"MapWorldsDesert\",\"somethingNew\":123}]}");
		rep.check(handOk && m.builds.size() == 1 && m.builds[0].astrolabes.size() == 1 &&
		          m.builds[0].astrolabes[0].region == "SouthWest" &&
		          m.builds[0].mapId == "MapWorldsDesert",
		          "hand-written document with an unknown extra key still loads");

		// Wrong-shaped values must not throw or corrupt the entry — an
		// astrolabes ARRAY (what a naive editor might write) is simply ignored.
		AtlasBuildFile bad;
		bool badOk = bad.ParseDoc(
			u8"{\"builds\":[{\"name\":\"b\",\"alloc\":[1],\"astrolabes\":[\"x\"],\"map\":5,"
			u8"\"mapTier\":\"sixteen\"}]}");
		rep.check(badOk && bad.builds.size() == 1 && bad.builds[0].astrolabes.empty() &&
		          bad.builds[0].mapId.empty() && bad.builds[0].mapTier == 0,
		          "wrong-typed astrolabes/map/mapTier are ignored rather than fatal");

		// The production Save/Load pair, not just the string codec. The caller's
		// BuildGuard restores the user's real file afterwards.
		AtlasBuildFile w;
		w.ParseDoc(u8"{\"builds\":[{\"name\":\"p1\",\"alloc\":[1,2]},{\"name\":\"p2\",\"alloc\":[3]}]}");
		w.builds[0].astrolabes = { { "NorthWest", "Metadata/Items/Currency/AstrolabeBlight" } };
		w.builds[0].mapId = "MapWorldsCage";
		bool wrote = w.Save(exeDir);
		AtlasBuildFile rd;
		bool readBack = rd.Load(exeDir);
		rep.check(wrote && readBack && rd.builds.size() == 2 &&
		          rd.builds[0].astrolabes.size() == 1 &&
		          rd.builds[0].astrolabes[0].id == w.builds[0].astrolabes[0].id &&
		          rd.builds[0].mapId == "MapWorldsCage" &&
		          rd.builds[1].astrolabes.empty() && rd.builds[1].mapId.empty(),
		          "Save/Load on disk preserves per-project astrolabes + main map");
	}

	// --- rules: need the catalogue.
	AstrolabeDb db;
	std::string derr;
	if (!db.Load(exeDir, &derr)) {
		rep.note("astrolabe catalogue absent - rule checks skipped (" + derr + ")");
		// The no-catalogue guard is exactly when user data is most at risk.
		std::vector<AstrolabePlacement> keep = { { "NorthWest", "x" }, { "SouthEast", "y" } };
		std::string n;
		std::vector<AstrolabePlacement> got = db.Sanitize(keep, &n);
		rep.check(got.size() == keep.size() && got[0].region == keep[0].region &&
		          got[0].id == keep[0].id && n.empty(),
		          "without a catalogue Sanitize leaves the saved placements untouched");
		return rep.failures;
	}

	rep.note("catalogue: " + std::to_string(db.All().size()) + " astrolabes, " +
	         std::to_string(db.Regions().size()) + " quadrants, source " + db.Source());
	rep.check(db.All().size() == 11, "catalogue holds 11 astrolabes",
	          std::to_string(db.All().size()));
	rep.check(db.Regions().size() == 4, "catalogue holds 4 quadrants",
	          std::to_string(db.Regions().size()));

	// Fixtures found live rather than hard-coded, so the checks survive a season
	// update that changes the catalogue.
	const AtlasQuadrant& q0 = db.Regions()[0];
	const AtlasQuadrant& q1 = db.Regions()[1];
	const AstrolabeDef& a0 = db.All()[0];
	const AstrolabeDef& a1 = db.All()[1];

	{
		std::vector<AstrolabePlacement> cur;
		rep.check(db.CanPlace(cur, q0.id, a0.id).ok(), "first astrolabe on an empty quadrant is allowed");
		cur.push_back({ q0.id, a0.id });

		AstrolabeAddResult r = db.CanPlace(cur, q0.id, a1.id);
		rep.check(r.code == AstrolabeAdd::kRegionOccupied && r.occupant == &a0,
		          "a second Shaped Region on the same quadrant is refused and names the occupant");
		// The same astrolabe on a DIFFERENT quadrant is legal: nothing in the
		// game data caps how many copies of one astrolabe may be in play.
		rep.check(db.CanPlace(cur, q1.id, a0.id).ok(),
		          "the same astrolabe on another quadrant is allowed");
		rep.check(db.CanPlace(cur, "Nowhere", a0.id).code == AstrolabeAdd::kUnknownRegion,
		          "unknown quadrant refused");
		rep.check(db.CanPlace(cur, q1.id, "Metadata/Items/Currency/AstrolabeNope").code ==
		              AstrolabeAdd::kUnknownAstrolabe,
		          "unknown astrolabe refused");
	}
	{
		// All four quadrants can be filled at once.
		std::vector<AstrolabePlacement> cur;
		for (const AtlasQuadrant& q : db.Regions())
			if (db.CanPlace(cur, q.id, a0.id).ok()) cur.push_back({ q.id, a0.id });
		rep.check(cur.size() == db.Regions().size(), "all four quadrants can hold one at once",
		          std::to_string(cur.size()));
	}
	{
		// T-sanitize: unknown quadrant + unknown astrolabe + a duplicated quadrant.
		std::vector<AstrolabePlacement> dirty = {
			{ q0.id, a0.id },
			{ q0.id, a1.id },                 // same quadrant twice
			{ "Elsewhere", a0.id },           // unknown quadrant
			{ q1.id, "Metadata/Items/Currency/Nope" }, // unknown astrolabe
		};
		std::string note;
		std::vector<AstrolabePlacement> clean = db.Sanitize(dirty, &note);
		rep.check(clean.size() == 1 && clean[0].region == q0.id && clean[0].id == a0.id,
		          "sanitize keeps one placement per quadrant and drops the rest",
		          std::to_string(clean.size()));
		rep.check(!note.empty(), "sanitize reports what it dropped", note);
		std::string note2;
		rep.check(db.Sanitize(clean, &note2).size() == clean.size() && note2.empty(),
		          "sanitize is idempotent and silent on clean input");
	}
	{
		int noArt = 0, noDesc = 0, notTranslated = 0;
		for (const AstrolabeDef& d : db.All()) {
			if (d.art.empty()) noArt++;
			if (d.descZh.empty()) noDesc++;
			if (d.zh.empty() || d.zh == d.en) notTranslated++;
		}
		rep.check(noArt == 0, "every astrolabe has an art path", std::to_string(noArt));
		rep.check(noDesc == 0, "every astrolabe has effect text", std::to_string(noDesc));
		rep.check(notTranslated == 0, "every astrolabe name is translated",
		          std::to_string(notTranslated));
		int quadNoVault = 0;
		for (const AtlasQuadrant& q : db.Regions())
			if (q.vaultZh.empty()) quadNoVault++;
		rep.check(quadNoVault == 0, "every quadrant has a vault name",
		          std::to_string(quadNoVault));
	}
	{
		// --- fuzzy search (shared implementation; these guard the wiring) ---
		auto score = [&](const AstrolabeDef& d, const char* qs) {
			return db.MatchScore(d, MakeFuzzyQuery(qs));
		};
		auto hitCount = [&](const char* qs) {
			FuzzyQuery q = MakeFuzzyQuery(qs);
			int n = 0;
			for (const AstrolabeDef& d : db.All()) if (db.MatchScore(d, q)) n++;
			return n;
		};
		const AstrolabeDef* breach = db.ById("Metadata/Items/Currency/AstrolabeBreach");
		const AstrolabeDef* abyss = db.ById("Metadata/Items/Currency/AstrolabeAbyss");
		rep.check(breach && abyss, "search fixtures present");
		if (breach && abyss) {
			rep.check(score(*breach, u8"執掌") > 0, "zh substring matches");
			rep.check(score(*breach, "grasping") > 0, "en substring matches");
			// Found by the mechanic named in the effect text, not in the name.
			rep.check(score(*breach, u8"裂痕") > 0, "effect text is searchable");
			rep.check(score(*abyss, u8"無光") > score(*breach, u8"裂痕"),
			          "a name hit outranks an effect hit",
			          std::to_string(score(*abyss, u8"無光")) + " vs " +
			              std::to_string(score(*breach, u8"裂痕")));
			rep.check(hitCount("") == (int)db.All().size(), "empty query matches everything");
			rep.check(hitCount("zzzzqqqq") == 0, "nonsense query matches nothing");
		}
	}

	return rep.failures;
}
