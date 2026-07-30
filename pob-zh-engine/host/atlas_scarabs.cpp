#include "atlas_scarabs.h"

#include "atlas_persist.h"   // round-trip checks in the self-test
#include "atlas_stat_agg.h"  // ToLowerAscii

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <json.hpp> // nlohmann::ordered_json (deps/nlohmann)

#include <algorithm>
#include <cstring> // strlen

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

// ---- fuzzy matching ----------------------------------------------------------

// Punctuation that shows up in these names and would otherwise make a query
// typed without it fail. Listed as UTF-8 literals because they are multi-byte;
// a byte-wise filter would corrupt the surrounding CJK.
static const char* kDropTokens[] = {
	u8"：", u8"，", u8"。", u8"、", u8"（", u8"）", u8"「", u8"」", u8"·", u8"…",
	":", ",", ".", "'", "\"", "(", ")", "[", "]", "-", "/",
};

// Lowercase, then strip whitespace and the punctuation above.
static std::string compact_key(const std::string& s)
{
	std::string out;
	out.reserve(s.size());
	size_t i = 0;
	while (i < s.size()) {
		bool dropped = false;
		for (const char* tok : kDropTokens) {
			size_t n = strlen(tok);
			if (s.compare(i, n, tok) == 0) { i += n; dropped = true; break; }
		}
		if (dropped) continue;
		unsigned char c = (unsigned char)s[i];
		if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { i++; continue; }
		out.push_back(s[i++]);
	}
	return out;
}

// Byte length of the UTF-8 code point starting at s[i].
static size_t utf8_len(const std::string& s, size_t i)
{
	unsigned char c = (unsigned char)s[i];
	size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
	return (i + n <= s.size()) ? n : 1;
}

// Do `needle`'s characters appear in `hay` in order (not necessarily adjacent)?
// Steps whole code points, never bytes: matching half of a CJK character
// against half of another would invent hits that make no sense to a reader.
static bool subsequence(const std::string& hay, const std::string& needle)
{
	size_t pos = 0;
	for (size_t i = 0; i < needle.size();) {
		size_t n = utf8_len(needle, i);
		size_t at = hay.find(needle.substr(i, n), pos);
		if (at == std::string::npos) return false;
		pos = at + n;
		i += n;
	}
	return true;
}

static bool all_tokens_in(const std::string& hay, const std::vector<std::string>& tokens)
{
	for (const std::string& t : tokens)
		if (hay.find(t) == std::string::npos) return false;
	return true;
}

ScarabQuery MakeScarabQuery(const std::string& raw)
{
	ScarabQuery q;
	q.lower = ToLowerAscii(raw);
	// trim so a trailing space does not turn into an empty token
	size_t b = q.lower.find_first_not_of(" \t\r\n");
	size_t e = q.lower.find_last_not_of(" \t\r\n");
	q.lower = (b == std::string::npos) ? std::string() : q.lower.substr(b, e - b + 1);
	q.compact = compact_key(q.lower);
	for (size_t i = 0; i <= q.lower.size();) {
		size_t sp = q.lower.find_first_of(" \t", i);
		std::string tok = q.lower.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
		if (!tok.empty()) q.tokens.push_back(tok);
		if (sp == std::string::npos) break;
		i = sp + 1;
	}
	return q;
}

// Best tier for one name field.
static int name_score(const std::string& key, const std::string& compact, const ScarabQuery& q)
{
	if (key.empty()) return 0;
	if (key == q.lower) return 100;
	if (key.compare(0, q.lower.size(), q.lower) == 0) return 90;
	size_t at = key.find(q.lower);
	if (at != std::string::npos)
		return 80 - (int)(at < 9 ? at : 9);   // earlier hits read as more relevant
	if (!q.compact.empty() && compact.find(q.compact) != std::string::npos) return 70;
	if (q.tokens.size() > 1 && all_tokens_in(key, q.tokens)) return 60;
	if (!q.compact.empty() && subsequence(compact, q.compact)) return 40;
	return 0;
}

int ScarabMatchScore(const ScarabDef& d, const ScarabQuery& q)
{
	if (q.empty()) return 1; // everything matches; caller keeps the natural order

	int best = name_score(d.keyZh, d.keyZhCompact, q);
	int en = name_score(d.keyEn, d.keyEnCompact, q);
	if (en > best) best = en;
	if (best) return best;

	// Nothing in the names: fall back to the effect text, always ranked below
	// any name hit so "what is this scarab called" beats "what does it do".
	for (const std::string* k : { &d.descKeyZh, &d.descKeyEn }) {
		if (k->empty()) continue;
		if (k->find(q.lower) != std::string::npos) return 30;
		if (q.tokens.size() > 1 && all_tokens_in(*k, q.tokens)) return 20;
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
			d.type = s.value("type", std::string());
			d.art = s.value("art", std::string());
			d.tier = s.value("tier", 0);
			d.family = s.value("family", 0);
			d.limit = std::clamp(s.value("limit", 1), 1, kMaxScarabs);
			d.stash = s.value("stash", true);
			if (s.contains("descEn")) d.descEn = parse_lines(s["descEn"]);
			if (s.contains("descZh")) d.descZh = parse_lines(s["descZh"]);
			if (d.descZh.empty()) d.descZh = d.descEn; // untranslated: show English
			d.keyEn = ToLowerAscii(d.en);
			d.keyZh = ToLowerAscii(d.zh);
			d.keyEnCompact = compact_key(d.keyEn);
			d.keyZhCompact = compact_key(d.keyZh);
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
		if (err) *err = u8"scarabs_poe1.json 沒有任何甲蟲";
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

	int same = 0;
	for (const std::string& have : cur) {
		if (have == id) { same++; continue; }
		// A different scarab of the same family blocks the whole family; report
		// the one already placed so the message can name it.
		const ScarabDef* other = ById(have);
		if (other && other->family == def->family) {
			r.code = ScarabAdd::kFamilyConflict;
			r.conflict = other;
			return r;
		}
	}
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
	// user's saved scarabs on the next save.
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
		if (unknown) *note += u8"，忽略 " + std::to_string(unknown) + u8" 個未知甲蟲";
		if (illegal) *note += u8"，忽略 " + std::to_string(illegal) + u8" 個違反放置規則的甲蟲";
		if (over)    *note += u8"，超過 " + std::to_string(kMaxScarabs) + u8" 格的 " +
		                      std::to_string(over) + u8" 個甲蟲已捨棄";
	}
	return out;
}

// ---- self-test (folded into --atlas-selftest) --------------------------------

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

	// --- persistence: these must hold whether or not the catalogue is present,
	// because they are what protects existing user files.
	{
		// T-legacy: a pre-scarab single-build file still loads, with the new
		// fields simply empty.
		AtlasBuildFile f;
		bool mig = f.ParseDoc(u8"{\"version\":\"x\",\"alloc\":[1,2,3]}");
		rep.check(mig && f.builds.size() == 1 && f.builds[0].notes.empty() &&
		          f.builds[0].scarabs.empty(),
		          "legacy file loads with empty notes/scarabs");

		// T-byte-compat: a build that uses neither feature must serialize
		// exactly as it did before the fields existed, so untouched user files
		// stay identical.
		std::string doc = f.SerializeDoc();
		rep.check(doc.find("notes") == std::string::npos &&
		          doc.find("scarabs") == std::string::npos,
		          "unused notes/scarabs are omitted from the document", doc);

		// T-roundtrip: full state survives save -> load.
		AtlasBuildFile g;
		g.ParseDoc(u8"{\"builds\":[{\"name\":\"a\",\"alloc\":[7]}]}");
		g.builds[0].notes = u8"第一行\n第二行";
		g.builds[0].scarabs = { "s1", "s2", "s3", "s4", "s5" };
		AtlasBuildFile h;
		bool rt = h.ParseDoc(g.SerializeDoc());
		rep.check(rt && h.builds.size() == 1 &&
		          h.builds[0].notes == g.builds[0].notes &&
		          h.builds[0].scarabs == g.builds[0].scarabs,
		          "notes + 5 scarabs round-trip through the build file");

		// T-forward: a document written by a NEWER build (unknown keys) must
		// still load — this is the mirror of "old exe reads new file".
		AtlasBuildFile k;
		rep.check(k.ParseDoc(u8"{\"format\":\"pobtools-atlas-builds\",\"builds\":["
		                     u8"{\"name\":\"a\",\"alloc\":[1],\"notes\":\"n\",\"future\":{\"x\":1}}],"
		                     u8"\"unknownTop\":42}") &&
		          k.builds.size() == 1 && k.builds[0].notes == "n",
		          "document with unknown extra keys still loads");

		// T-share: the share code carries the new fields, and a code produced
		// BEFORE they existed still parses (the PTAT1 prefix is unchanged on
		// purpose — bumping it would make older builds reject new codes).
		AtlasBuildEntry src;
		src.name = u8"甲蟲測試";
		src.alloc = { 11, 22 };
		src.notes = u8"備註內容";
		src.scarabs = { "a", "b" };
		AtlasBuildEntry back;
		std::string perr;
		std::string code = AtlasBuildShareCode(src, "test");
		rep.check(code.compare(0, 6, "PTAT1|") == 0 &&
		          AtlasParseShareCode(code, &back, &perr) &&
		          back.notes == src.notes && back.scarabs == src.scarabs &&
		          back.alloc == src.alloc,
		          "share code carries notes + scarabs", perr);

		AtlasBuildEntry old;
		rep.check(AtlasParseExportJson(
		              u8"{\"format\":\"pobtools-atlas-build\",\"version\":\"v\","
		              u8"\"name\":\"old\",\"alloc\":[5,6]}", &old, &perr) &&
		          old.alloc == std::vector<int>({ 5, 6 }) && old.notes.empty() && old.scarabs.empty(),
		          "pre-scarab export file still imports", perr);

		AtlasBuildEntry noneUsed;
		noneUsed.name = "n";
		noneUsed.alloc = { 1 };
		std::string ex = AtlasExportJson(noneUsed, "v");
		rep.check(ex.find("notes") == std::string::npos && ex.find("scarabs") == std::string::npos,
		          "export omits unused notes/scarabs too", ex);

		// T-disk: the production Save/Load pair, not just the string codec —
		// this is the path the planner actually uses, and the one the GUI test
		// cannot drive (ImGui ignores synthesized clicks, see
		// error_win_gui_test_capture). The caller's BuildGuard restores the
		// user's real file afterwards.
		AtlasBuildFile w;
		w.ParseDoc(u8"{\"builds\":[{\"name\":\"p1\",\"alloc\":[1,2]},{\"name\":\"p2\",\"alloc\":[3]}]}");
		w.active = 1;
		w.version = "vtest";
		w.builds[0].notes = u8"專案一備註\n含換行";
		w.builds[0].scarabs = { "sA", "sB" };
		w.builds[1].scarabs = { "sC" };
		bool wrote = w.Save(exeDir);
		AtlasBuildFile r;
		bool readBack = r.Load(exeDir);
		rep.check(wrote && readBack && r.builds.size() == 2 && r.active == 1 &&
		          r.builds[0].notes == w.builds[0].notes &&
		          r.builds[0].scarabs == w.builds[0].scarabs &&
		          r.builds[1].scarabs == w.builds[1].scarabs &&
		          r.builds[1].notes.empty(),
		          "Save/Load on disk preserves per-project notes + scarabs");
	}

	// --- rules: need the catalogue.
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

	rep.note("catalogue: " + std::to_string(db.All().size()) + " scarabs, source " + db.Source());
	rep.check(db.All().size() == 130, "catalogue holds 130 scarabs",
	          std::to_string(db.All().size()));

	// Find live fixtures instead of hard-coding ids, so the test keeps working
	// after a season update changes the catalogue.
	const ScarabDef* lim1 = nullptr;
	const ScarabDef* limN = nullptr;
	const ScarabDef* famA = nullptr;
	const ScarabDef* famB = nullptr;
	for (const ScarabDef& d : db.All()) {
		if (!lim1 && d.limit == 1) lim1 = &d;
		if (!limN && d.limit >= 2) limN = &d;
	}
	for (const ScarabDef& a : db.All()) {
		if (famA) break;
		for (const ScarabDef& b : db.All())
			if (&a != &b && a.family == b.family) { famA = &a; famB = &b; break; }
	}
	rep.check(lim1 && limN, "found limit=1 and limit>1 fixtures");
	rep.check(famA && famB, "found a mutually exclusive pair",
	          famA && famB ? famA->en + " / " + famB->en : "none");

	if (lim1) {
		std::vector<std::string> cur;
		rep.check(db.CanAdd(cur, lim1->id).ok(), "first copy allowed");
		cur.push_back(lim1->id);
		ScarabAddResult r = db.CanAdd(cur, lim1->id);
		rep.check(r.code == ScarabAdd::kOverLimit && r.limit == 1,
		          "second copy of a limit=1 scarab refused");
	}
	if (limN) {
		std::vector<std::string> cur(limN->limit, limN->id);
		ScarabAddResult r = db.CanAdd(cur, limN->id);
		bool expectFull = (int)cur.size() >= kMaxScarabs;
		rep.check(r.code == (expectFull ? ScarabAdd::kFull : ScarabAdd::kOverLimit),
		          "copy limit+1 refused", "limit=" + std::to_string(limN->limit));
		std::vector<std::string> below(limN->limit - 1, limN->id);
		rep.check(db.CanAdd(below, limN->id).ok(), "copies up to the limit allowed");
	}
	if (famA && famB) {
		std::vector<std::string> cur{ famA->id };
		ScarabAddResult r = db.CanAdd(cur, famB->id);
		rep.check(r.code == ScarabAdd::kFamilyConflict && r.conflict == famA,
		          "same-family scarab refused and names the blocker");
	}
	{
		// Five distinct families fill the device; a sixth is refused.
		std::vector<std::string> cur;
		for (const ScarabDef& d : db.All()) {
			if ((int)cur.size() >= kMaxScarabs) break;
			if (db.CanAdd(cur, d.id).ok()) cur.push_back(d.id);
		}
		rep.check((int)cur.size() == kMaxScarabs, "five scarabs fit");
		bool refusedAll = true;
		for (const ScarabDef& d : db.All())
			refusedAll = refusedAll && db.CanAdd(cur, d.id).code == ScarabAdd::kFull;
		rep.check(refusedAll, "a full device refuses every scarab");
	}
	{
		// T-sanitize: unknown id + over-limit duplicate + too many entries.
		std::vector<std::string> dirty;
		dirty.push_back("Metadata/Items/Scarabs/DoesNotExist");
		if (lim1) { dirty.push_back(lim1->id); dirty.push_back(lim1->id); }
		for (const ScarabDef& d : db.All()) {
			if ((int)dirty.size() >= 9) break;
			if (db.CanAdd(std::vector<std::string>(), d.id).ok() &&
			    std::find(dirty.begin(), dirty.end(), d.id) == dirty.end())
				dirty.push_back(d.id);
		}
		std::string note;
		std::vector<std::string> clean = db.Sanitize(dirty, &note);
		rep.check((int)clean.size() <= kMaxScarabs, "sanitize clamps to five",
		          std::to_string(clean.size()));
		rep.check(std::find(clean.begin(), clean.end(),
		                    std::string("Metadata/Items/Scarabs/DoesNotExist")) == clean.end(),
		          "sanitize drops unknown ids");
		std::string note2;
		rep.check(db.Sanitize(clean, &note2) == clean && note2.empty(),
		          "sanitize is idempotent and silent on clean input");
		rep.check(!note.empty(), "sanitize reports what it dropped", note);
	}
	{
		// Every scarab must be placeable in an empty device, or the picker would
		// show permanently dead entries.
		int blocked = 0;
		for (const ScarabDef& d : db.All())
			if (!db.CanAdd(std::vector<std::string>(), d.id).ok()) blocked++;
		rep.check(blocked == 0, "every scarab is placeable on its own",
		          std::to_string(blocked) + " blocked");
	}
	{
		// --- fuzzy search ---------------------------------------------------
		// Fixtures by id so the checks survive a wording change in a future
		// season; every lookup is asserted rather than assumed.
		const ScarabDef* hive = db.ById("Metadata/Items/Scarabs/ScarabBreachNew1");
		const ScarabDef* abyss = db.ById("Metadata/Items/Scarabs/ScarabAbyssNew1");
		rep.check(hive && abyss, "search fixtures present");
		if (hive && abyss) {
			auto score = [&](const ScarabDef& d, const char* qs) {
				return ScarabMatchScore(d, MakeScarabQuery(qs));
			};
			auto hitCount = [&](const char* qs) {
				ScarabQuery q = MakeScarabQuery(qs);
				int n = 0;
				for (const ScarabDef& d : db.All()) if (ScarabMatchScore(d, q)) n++;
				return n;
			};

			rep.check(score(*hive, u8"裂痕") > 0, "zh substring still matches");
			rep.check(score(*hive, "hive") > 0, "en substring still matches");
			// The two locales order the words oppositely; a user who knows one
			// order must still find the scarab.
			rep.check(score(*hive, "hive breach") > 0, "en tokens out of order match");
			rep.check(score(*hive, u8"裂痕 窩巢") > 0, "zh tokens out of order match");
			// 「聖甲蟲：窩巢裂痕」 typed without the fullwidth colon.
			rep.check(score(*hive, u8"聖甲蟲窩巢") > 0, "punctuation-free query matches");
			rep.check(score(*hive, u8"窩裂") > 0, "zh subsequence matches");
			rep.check(score(*hive, "brchhv") > 0, "en subsequence matches");

			// Effect text is searchable but must never outrank a name hit.
			int byEffect = score(*hive, u8"裂痕巢穴");
			rep.check(byEffect > 0, "effect text is searchable");
			rep.check(score(*abyss, u8"深淵") > byEffect,
			          "a name hit outranks an effect hit",
			          std::to_string(score(*abyss, u8"深淵")) + " vs " + std::to_string(byEffect));

			// An exact name beats a name that merely contains the query.
			const ScarabDef* multi = db.ById("Metadata/Items/Scarabs/ScarabAbyssNew2");
			rep.check(multi && score(*abyss, u8"聖甲蟲：深淵") > score(*multi, u8"聖甲蟲：深淵"),
			          "exact name outranks a longer containing name");

			rep.check(hitCount("") == (int)db.All().size(), "empty query matches everything");
			rep.check(hitCount("zzzzqqqq") == 0, "nonsense query matches nothing",
			          std::to_string(hitCount("zzzzqqqq")));
			// A subsequence over whole code points must not pair half of one CJK
			// character with half of another.
			rep.check(hitCount(u8"深") >= 5 && hitCount(u8"深") < (int)db.All().size(),
			          "single CJK char filters without matching everything",
			          std::to_string(hitCount(u8"深")));
			rep.check(hitCount("  breach  ") == hitCount("breach"),
			          "surrounding whitespace is ignored");
		}
	}
	{
		int noArt = 0, noDesc = 0;
		for (const ScarabDef& d : db.All()) {
			if (d.art.empty()) noArt++;
			if (d.descZh.empty()) noDesc++;
		}
		rep.check(noArt == 0, "every scarab has an art path", std::to_string(noArt));
		rep.check(noDesc == 0, "every scarab has effect text", std::to_string(noDesc));
	}

	return rep.failures;
}
