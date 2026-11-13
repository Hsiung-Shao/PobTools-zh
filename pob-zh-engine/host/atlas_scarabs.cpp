#include "atlas_scarabs.h"

#include "atlas_persist.h"   // round-trip checks in the self-test
#include "atlas_stat_agg.h"  // ToLowerAscii

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <json.hpp> // nlohmann::ordered_json (deps/nlohmann)

#include <algorithm>

using nlohmann::ordered_json;

// Same file helper as the sibling atlas modules; kept local rather than shared
// so this stays a leaf with no dependency on the persistence layer.
static bool scarab_read_file(const std::wstring& path, std::string& out)
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

// fuzzy matching
// The tiers live in fuzzy_match.cpp (shared with the astrolabe and map pickers).

int ScarabMatchScore(const ScarabDef& d, const ScarabQuery& q)
{
	if (q.empty()) return 1; // everything matches; caller keeps the natural order

	int best = FuzzyNameScore(d.keyZh, d.keyZhCompact, q);
	int en = FuzzyNameScore(d.keyEn, d.keyEnCompact, q);
	if (en > best) best = en;
	if (best) return best;

	// Nothing in the names: fall back to the effect text, always ranked below any name hit.
	for (const std::string* k : { &d.descKeyZh, &d.descKeyEn }) {
		int s = FuzzyTextScore(*k, q);
		if (s) return s;
	}
	return 0;
}

bool ScarabDb::Load(const std::wstring& exeDir, std::string* err)
{
	defs_.clear();
	byId_.clear();
	source_.clear();

	std::string body;
	if (!scarab_read_file(exeDir + L"Data\\scarabs_poe1.json", body)) {
		if (err) *err = u8"找不到 Data\\scarabs_poe1.json";
		return false;
	}
	try {
		ordered_json doc = ordered_json::parse(body);
		if (doc.value("format", std::string()) != "pobtools-scarabs") {
			if (err) *err = u8"scarabs_poe1.json 的 format 欄位不符";
			return false;
		}
		source_ = doc.value("source", std::string());
		const auto& arr = doc["scarabs"];
		if (!arr.is_array()) {
			if (err) *err = u8"scarabs_poe1.json 缺少 scarabs 陣列";
			return false;
		}
		defs_.reserve(arr.size());
		for (const auto& s : arr) {
			if (!s.is_object()) continue;
			ScarabDef d;
			d.id = s.value("id", std::string());
			d.en = s.value("en", std::string());
			d.zh = s.value("zh", std::string());
			if (d.id.empty() || d.en.empty()) continue;
			d.art = s.value("art", std::string());
			d.limit = std::clamp(s.value("limit", 1), 1, kMaxScarabs);
			d.stash = s.value("stash", true);
			if (s.contains("descEn")) d.descEn = parse_lines(s["descEn"]);
			if (s.contains("descZh")) d.descZh = parse_lines(s["descZh"]);
			if (d.descZh.empty()) d.descZh = d.descEn; // untranslated: show English
			d.keyEn = ToLowerAscii(d.en);
			d.keyZh = ToLowerAscii(d.zh);
			d.keyEnCompact = FuzzyCompactKey(d.keyEn);
			d.keyZhCompact = FuzzyCompactKey(d.keyZh);
			// Effects are searched as rendered, so a query matches what the user
			// can actually see (GGG markup like [ContainsAbyss|深淵] is stripped).
			for (const std::string& s : d.descEn) d.descKeyEn += StripStatMarkup(s) + "\n";
			for (const std::string& s : d.descZh) d.descKeyZh += StripStatMarkup(s) + "\n";
			d.descKeyEn = ToLowerAscii(d.descKeyEn);
			d.descKeyZh = ToLowerAscii(d.descKeyZh);
			defs_.push_back(std::move(d));
		}
	} catch (const std::exception& e) {
		defs_.clear();
		if (err) *err = std::string(u8"scarabs_poe1.json 解析失敗: ") + e.what();
		return false;
	}
	if (defs_.empty()) {
		if (err) *err = u8"scarabs_poe1.json 沒有任何項目";
		return false;
	}
	for (int i = 0; i < (int)defs_.size(); i++) byId_.emplace(defs_[i].id, i);
	return true;
}

const ScarabDef* ScarabDb::ById(const std::string& id) const
{
	auto it = byId_.find(id);
	return it == byId_.end() ? nullptr : &defs_[it->second];
}

ScarabAddResult ScarabDb::CanAdd(const std::vector<std::string>& cur, const std::string& id) const
{
	ScarabAddResult r;
	const ScarabDef* def = ById(id);
	if (!def) { r.code = ScarabAdd::kUnknown; return r; }
	if ((int)cur.size() >= kMaxScarabs) { r.code = ScarabAdd::kFull; return r; }

	const int same = (int)std::count(cur.begin(), cur.end(), id);
	if (same >= def->limit) {
		r.code = ScarabAdd::kOverLimit;
		r.limit = def->limit;
		return r;
	}
	return r;
}

std::vector<std::string> ScarabDb::Sanitize(const std::vector<std::string>& ids, std::string* note) const
{
	if (note) note->clear();
	// No catalogue -> no basis to judge. Passing the list through unchanged is
	// deliberate: dropping it would let a missing data file silently erase the
	// user's saved slots on the next save.
	if (!available()) return ids;

	std::vector<std::string> out;
	int unknown = 0, illegal = 0, over = 0;
	for (const std::string& id : ids) {
		ScarabAddResult r = CanAdd(out, id);
		if (r.ok()) { out.push_back(id); continue; }
		switch (r.code) {
		case ScarabAdd::kUnknown: unknown++; break;
		case ScarabAdd::kFull:    over++;    break;
		default:                  illegal++; break;
		}
	}
	if (note) {
		if (unknown) *note += u8"，忽略 " + std::to_string(unknown) + u8" 個未知項目";
		if (illegal) *note += u8"，忽略 " + std::to_string(illegal) + u8" 個違反放置規則的項目";
		if (over)    *note += u8"，超過 " + std::to_string(kMaxScarabs) + u8" 個地圖格的 " +
		                      std::to_string(over) + u8" 個項目已捨棄";
	}
	return out;
}

// self-test (folded into --atlas-selftest)

namespace {

struct ScarabReport {
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

int RunScarabSelfTest(const std::wstring& exeDir, std::string& out)
{
	ScarabReport rep{ out };
	out += "\n-- scarabs + notes --\n";

	// persistence: must hold with or without the catalogue (they protect existing user files).
	{
		// T-legacy: a pre-scarab single-build file still loads, with the new
		// fields simply empty.
		AtlasBuildFile f;
		bool mig = f.ParseDoc(u8"{\"version\":\"x\",\"alloc\":[1,2,3]}");
		rep.check(mig && f.builds.size() == 1 && f.builds[0].notes.empty() &&
		          f.builds[0].scarabs.empty() && f.builds[0].targets.empty() &&
		          f.builds[0].blocked.empty(),
		          "legacy file loads with empty notes/scarabs/targets/blocked");

		// T-byte-compat: a build using none of the features must serialize exactly as before the fields existed.
		std::string doc = f.SerializeDoc();
		rep.check(doc.find("notes") == std::string::npos &&
		          doc.find("scarabs") == std::string::npos &&
		          doc.find("targets") == std::string::npos &&
		          doc.find("blocked") == std::string::npos,
		          "unused notes/scarabs/targets/blocked are omitted from the document", doc);

		// T-roundtrip: full state survives save -> load.
		AtlasBuildFile g;
		g.ParseDoc(u8"{\"builds\":[{\"name\":\"a\",\"alloc\":[7]}]}");
		g.builds[0].notes = u8"第一行\n第二行";
		g.builds[0].scarabs = { "s1", "s2", "s3", "s4", "s5" };
		g.builds[0].targets = { 7, 4242 };
		g.builds[0].blocked = { 99 };
		AtlasBuildFile h;
		bool rt = h.ParseDoc(g.SerializeDoc());
		rep.check(rt && h.builds.size() == 1 &&
		          h.builds[0].notes == g.builds[0].notes &&
		          h.builds[0].scarabs == g.builds[0].scarabs &&
		          h.builds[0].targets == g.builds[0].targets &&
		          h.builds[0].blocked == g.builds[0].blocked,
		          "notes + 5 scarabs + targets + blocked round-trip through the build file");

		// T-forward: a document written by a NEWER build (unknown keys) must
		// still load — this is the mirror of "old exe reads new file".
		AtlasBuildFile k;
		rep.check(k.ParseDoc(u8"{\"format\":\"pobtools-atlas-builds\",\"builds\":["
		                     u8"{\"name\":\"a\",\"alloc\":[1],\"notes\":\"n\",\"future\":{\"x\":1}}],"
		                     u8"\"unknownTop\":42}") &&
		          k.builds.size() == 1 && k.builds[0].notes == "n",
		          "document with unknown extra keys still loads");

		// T-share: the share code carries the new fields, and an older code still parses (PTAT1 prefix
		// unchanged on purpose: bumping it would make older builds reject new codes).
		AtlasBuildEntry src;
		src.name = u8"甲蟲測試";
		src.alloc = { 11, 22 };
		src.notes = u8"備註內容";
		src.scarabs = { "a", "b" };
		src.targets = { 22 };
		src.blocked = { 33 };
		AtlasBuildEntry back;
		std::string perr;
		std::string code = AtlasBuildShareCode(src, "test");
		rep.check(code.compare(0, 6, "PTAT1|") == 0 &&
		          AtlasParseShareCode(code, &back, &perr) &&
		          back.notes == src.notes && back.scarabs == src.scarabs &&
		          back.targets == src.targets && back.blocked == src.blocked &&
		          back.alloc == src.alloc,
		          "share code carries notes + scarabs + targets + blocked", perr);

		AtlasBuildEntry old;
		rep.check(AtlasParseExportJson(
		              u8"{\"format\":\"pobtools-atlas-build\",\"version\":\"v\","
		              u8"\"name\":\"old\",\"alloc\":[5,6]}", &old, &perr) &&
		          old.alloc == std::vector<int>({ 5, 6 }) && old.notes.empty() &&
		          old.scarabs.empty() && old.targets.empty() && old.blocked.empty(),
		          "pre-scarab export file still imports", perr);

		AtlasBuildEntry noneUsed;
		noneUsed.name = "n";
		noneUsed.alloc = { 1 };
		std::string ex = AtlasExportJson(noneUsed, "v");
		rep.check(ex.find("notes") == std::string::npos && ex.find("scarabs") == std::string::npos &&
		          ex.find("targets") == std::string::npos &&
		          ex.find("blocked") == std::string::npos,
		          "export omits unused notes/scarabs/targets/blocked too", ex);

		// T-disk: the production Save/Load pair (the GUI test can't drive it: ImGui ignores synthesized
		// clicks, see error_win_gui_test_capture). The caller's BuildGuard restores the user's real file.
		AtlasBuildFile w;
		w.ParseDoc(u8"{\"builds\":[{\"name\":\"p1\",\"alloc\":[1,2]},{\"name\":\"p2\",\"alloc\":[3]}]}");
		w.active = 1;
		w.version = "vtest";
		w.builds[0].notes = u8"專案一備註\n含換行";
		w.builds[0].scarabs = { "sA", "sB" };
		w.builds[1].scarabs = { "sC" };
		w.builds[0].targets = { 2 };
		w.builds[0].blocked = { 3 };
		bool wrote = w.Save(exeDir);
		AtlasBuildFile r;
		bool readBack = r.Load(exeDir);
		rep.check(wrote && readBack && r.builds.size() == 2 && r.active == 1 &&
		          r.builds[0].notes == w.builds[0].notes &&
		          r.builds[0].scarabs == w.builds[0].scarabs &&
		          r.builds[1].scarabs == w.builds[1].scarabs &&
		          r.builds[0].targets == w.builds[0].targets &&
		          r.builds[0].blocked == w.builds[0].blocked &&
		          r.builds[1].targets.empty() &&
		          r.builds[1].notes.empty(),
		          "Save/Load on disk preserves per-project notes + scarabs + targets + blocked");
	}

	{
		// Always run: a missing data file must never erase a saved slot list.
		ScarabDb none;
		std::vector<std::string> keep = { "Metadata/Items/Scarabs/ScarabAbyssNew1", "y" };
		std::string n;
		rep.check(none.Sanitize(keep, &n) == keep && n.empty(),
		          "without a catalogue Sanitize leaves the saved list untouched (unconditional)");
	}

	// rules: need the catalogue.
	ScarabDb db;
	std::string derr;
	if (!db.Load(exeDir, &derr)) {
		rep.note("scarab catalogue absent - rule checks skipped (" + derr + ")");
		// The no-catalogue guard is exactly when user data is most at risk.
		std::vector<std::string> keep = { "x", "y" };
		std::string n;
		rep.check(db.Sanitize(keep, &n) == keep && n.empty(),
		          "without a catalogue Sanitize leaves the saved list untouched");
		return rep.failures;
	}

	// Only the eight Vaal fragments remain in the catalogue.
	size_t nScarab = 0;
	for (const ScarabDef& d : db.All())
		if (d.id.find("/Scarabs/") != std::string::npos) nScarab++;
	rep.note("catalogue: " + std::to_string(db.All().size()) + " entries, source " + db.Source());
	rep.check(nScarab == 0, "catalogue holds no scarabs", std::to_string(nScarab));
	rep.check(db.All().size() == 8, "catalogue holds 8 Vaal map fragments",
	          std::to_string(db.All().size()));
	{
		// An old build's scarab ids are dropped, its fragments kept, and the note says so.
		const std::string frag = db.All().empty() ? std::string() : db.All()[0].id;
		std::string note;
		std::vector<std::string> clean = db.Sanitize(
			{ "Metadata/Items/Scarabs/ScarabAbyssNew1", frag, "Metadata/Items/Scarabs/ScarabBreachNew1" }, &note);
		rep.check(clean == std::vector<std::string>{ frag } && !note.empty(),
		          "an old build's scarabs are dropped, its fragment kept", note);
	}
	{
		// The four Sacrifice pieces are what opens a Vaal side area, so they
		// MUST all fit together; a second copy of one is refused by limit.
		std::vector<const ScarabDef*> sac;
		for (const ScarabDef& d : db.All())
			if (d.id.find("CurrencyVaalFragment1_") != std::string::npos) sac.push_back(&d);
		rep.check(sac.size() == 4, "four Sacrifice fragments present",
		          std::to_string(sac.size()));
		if (sac.size() == 4) {
			std::vector<std::string> cur;
			bool allOk = true;
			for (const ScarabDef* d : sac) {
				allOk = allOk && db.CanAdd(cur, d->id).ok();
				cur.push_back(d->id);
			}
			rep.check(allOk && cur.size() == 4, "all four Sacrifice fragments fit together");
			rep.check(db.Sanitize(cur, nullptr).size() == 4,
			          "Sanitize keeps the whole Sacrifice set");
			ScarabAddResult r = db.CanAdd(cur, sac[0]->id);
			rep.check(r.code == ScarabAdd::kOverLimit && r.limit == 1,
			          "a duplicate fragment is refused by limit");
		}

		// The picker filters with ScarabMatchScore, so searching the way a user
		// would has to surface them.
		auto found = [&](const char* q) {
			ScarabQuery sq = MakeScarabQuery(q);
			int n = 0;
			for (const ScarabDef& d : db.All())
				if (ScarabMatchScore(d, sq) > 0) n++;
			return n;
		};
		rep.check(found(u8"奉獻") == 4, "搜尋「奉獻」找到 4 個獻祭碎片",
		          std::to_string(found(u8"奉獻")));
		rep.check(found(u8"凡人") == 4, "搜尋「凡人」找到 4 個凡人碎片",
		          std::to_string(found(u8"凡人")));
		rep.check(found("Sacrifice") == 4, "英文查詢同樣找得到",
		          std::to_string(found("Sacrifice")));
		rep.check(found(u8"黎明的奉獻") == 1, "全名查詢只命中一件",
		          std::to_string(found(u8"黎明的奉獻")));
		rep.check(found(u8"黎奉") == 1, "中文子序列查詢命中",
		          std::to_string(found(u8"黎奉")));
		rep.check(found(u8"瓦爾區域") == (int)db.All().size(), "效果文字可搜尋",
		          std::to_string(found(u8"瓦爾區域")));
		rep.check(found("") == (int)db.All().size(), "empty query matches everything");
		rep.check(found("zzzzqqqq") == 0, "nonsense query matches nothing");
	}
	{
		// Five fill the device; a sixth is refused.
		std::vector<std::string> cur;
		for (const ScarabDef& d : db.All()) {
			if ((int)cur.size() >= kMaxScarabs) break;
			if (db.CanAdd(cur, d.id).ok()) cur.push_back(d.id);
		}
		rep.check((int)cur.size() == kMaxScarabs, "five entries fit");
		bool refusedAll = true;
		for (const ScarabDef& d : db.All())
			refusedAll = refusedAll && db.CanAdd(cur, d.id).code == ScarabAdd::kFull;
		rep.check(refusedAll, "a full device refuses every entry");

		std::vector<std::string> dirty = { "Metadata/Items/MapFragments/DoesNotExist" };
		for (const ScarabDef& d : db.All()) dirty.push_back(d.id);
		std::string note, note2;
		std::vector<std::string> clean = db.Sanitize(dirty, &note);
		rep.check((int)clean.size() == kMaxScarabs, "sanitize clamps to five",
		          std::to_string(clean.size()));
		rep.check(db.Sanitize(clean, &note2) == clean && note2.empty(),
		          "sanitize is idempotent and silent on clean input");
		rep.check(!note.empty(), "sanitize reports what it dropped", note);
	}
	{
		int noArt = 0, noDesc = 0;
		for (const ScarabDef& d : db.All()) {
			if (d.art.empty()) noArt++;
			if (d.descZh.empty()) noDesc++;
		}
		rep.check(noArt == 0, "every entry has an art path", std::to_string(noArt));
		rep.check(noDesc == 0, "every entry has effect text", std::to_string(noDesc));
	}

	return rep.failures;
}
