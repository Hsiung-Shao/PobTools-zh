#include "atlas_version_index.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <json.hpp> // nlohmann::ordered_json (deps/nlohmann)

#include <algorithm>
#include <cstdlib>

using nlohmann::ordered_json;

// ---- file helpers (same conventions as atlas_tree_data.cpp) -----------------

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

static bool dir_exists(const std::wstring& p)
{
	DWORD a = GetFileAttributesW(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool file_exists(const std::wstring& p)
{
	DWORD a = GetFileAttributesW(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// ---- semver comparison ------------------------------------------------------

int AtlasVersionIndex::CompareSemver(const std::string& a, const std::string& b)
{
	auto part = [](const std::string& s, size_t& pos) -> int {
		int v = 0;
		bool any = false;
		while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') { v = v * 10 + (s[pos] - '0'); pos++; any = true; }
		if (pos < s.size() && s[pos] == '.') pos++;   // skip separator
		else if (pos < s.size() && !any) pos++;        // skip a non-numeric segment (compares as 0)
		return v;
	};
	size_t pa = 0, pb = 0;
	for (int i = 0; i < 4; i++) {
		size_t qa = pa, qb = pb;
		int va = part(a, qa), vb = part(b, qb);
		pa = qa; pb = qb;
		if (va != vb) return va < vb ? -1 : 1;
		if (pa >= a.size() && pb >= b.size()) break;
	}
	return 0;
}

// ---- paths ------------------------------------------------------------------

std::wstring AtlasVersionIndex::IndexPath(const std::wstring& exeDir)
{
	return exeDir + L"Data\\atlas_index.json";
}

std::wstring AtlasVersionIndex::VersionDir(const std::wstring& exeDir, const std::string& tag)
{
	std::wstring w(tag.begin(), tag.end()); // tags are ASCII (semver)
	return exeDir + L"Data\\atlas_versions\\" + w + L"\\";
}

std::wstring AtlasVersionIndex::ResolveDataDir(const std::wstring& exeDir, const std::string& tag) const
{
	std::string t = tag.empty() ? active_ : tag;
	if (!t.empty()) {
		std::wstring vd = VersionDir(exeDir, t);
		if (file_exists(vd + L"atlas_tree_poe1.json")) return vd;
	}
	return exeDir + L"Data\\"; // legacy flat layout
}

// ---- registry ---------------------------------------------------------------

bool AtlasVersionIndex::Has(const std::string& tag) const { return Find(tag) != nullptr; }

const AtlasVersionEntry* AtlasVersionIndex::Find(const std::string& tag) const
{
	for (const auto& e : versions_) if (e.tag == tag) return &e;
	return nullptr;
}

std::vector<std::string> AtlasVersionIndex::TagsNewestFirst() const
{
	std::vector<std::string> tags;
	for (const auto& e : versions_) tags.push_back(e.tag);
	std::sort(tags.begin(), tags.end(),
	          [](const std::string& a, const std::string& b) { return CompareSemver(a, b) > 0; });
	return tags;
}

std::string AtlasVersionIndex::OlderThan(const std::string& tag) const
{
	for (const std::string& t : TagsNewestFirst())
		if (CompareSemver(t, tag) < 0) return t;
	return std::string();
}

void AtlasVersionIndex::refreshCompareBase()
{
	// Prefer the newest version OLDER than the active one, so the default diff
	// reads old -> new. Only when the active season is the oldest installed does
	// this fall back to "newest that is not active" (which then reads new -> old,
	// but at least offers something). This mattered once the registry started
	// holding several revisions of one league: with 3.29.0 active and 3.29.1 also
	// installed, "newest non-active" alone would have pointed the base at a
	// version NEWER than the target.
	compareBase_ = OlderThan(active_);
	if (!compareBase_.empty()) return;
	for (const std::string& t : TagsNewestFirst())
		if (t != active_) { compareBase_ = t; break; }
}

void AtlasVersionIndex::adoptFromDisk(const std::wstring& exeDir)
{
	// The index is a cache of what is on disk, not the other way round. A season
	// folder that carries real data but never made it into atlas_index.json (an
	// update interrupted midway, a hand-copied folder, an index restored from an
	// older backup) is otherwise invisible to the planner forever — which is
	// exactly how a downloaded 3.29.1 ended up unusable while sitting on disk.
	std::wstring glob = exeDir + L"Data\\atlas_versions\\*";
	WIN32_FIND_DATAW fd{};
	HANDLE h = FindFirstFileW(glob.c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return;
	bool added = false;
	std::string newest;   // newest COMPLETE season folder adopted in this pass
	do {
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
		std::wstring name = fd.cFileName;
		if (name == L"." || name == L"..") continue;
		// Tags are ASCII semver; anything else is not ours to adopt.
		std::string tag;
		bool ascii = !name.empty();
		for (wchar_t c : name) {
			if (c < 32 || c > 126) { ascii = false; break; }
			tag.push_back((char)c);
		}
		if (!ascii || Has(tag)) continue;
		// Only adopt a folder that carries a COMPLETE season. A tree file alone
		// is not enough: an interrupted download can leave one behind without
		// its sprites or its Chinese mapping, and promoting that to active below
		// would break the planner outright.
		std::wstring vd = VersionDir(exeDir, tag);
		if (!file_exists(vd + L"atlas_tree_poe1.json")) continue;
		if (!file_exists(vd + L"atlas_tree_zh.json")) continue;
		if (!dir_exists(vd + L"atlas")) continue;
		AtlasVersionEntry e;
		e.tag = tag;
		versions_.push_back(e);
		added = true;
		if (newest.empty() || CompareSemver(tag, newest) > 0) newest = tag;
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	if (!added) return;
	// The newest complete season ON DISK is canonical, even when the index file
	// disagrees. It regularly does: Data/atlas_index.json ships with the app, so
	// every install and every app update writes the packaged copy back over it
	// and orphans a season the user downloaded in-app afterwards. Leaving
	// `active` alone made that season permanently "舊賽季檢視（唯讀）" -- visible,
	// selectable, and silently refusing to save. Which season the user is
	// LOOKING at is a separate, persisted choice (uiState.season), so promoting
	// active here does not move them.
	if (active_.empty() || CompareSemver(newest, active_) > 0) active_ = newest;
	refreshCompareBase();
	dirty_ = true;
}

void AtlasVersionIndex::UpsertActive(const AtlasVersionEntry& e)
{
	bool found = false;
	for (auto& v : versions_)
		if (v.tag == e.tag) { v = e; found = true; break; }
	if (!found) versions_.push_back(e);
	active_ = e.tag;
	refreshCompareBase();
}

void AtlasVersionIndex::SetActive(const std::string& tag)
{
	if (!Has(tag)) return;
	active_ = tag;
	refreshCompareBase();
}

std::string AtlasVersionIndex::SeasonOf(const std::string& tag)
{
	// "3.29.1" -> "3.29". A league is major.minor; the patch is an in-league
	// revision. A tag with fewer than two dots is its own season.
	size_t first = tag.find('.');
	if (first == std::string::npos) return tag;
	size_t second = tag.find('.', first + 1);
	return second == std::string::npos ? tag : tag.substr(0, second);
}

std::vector<std::string> AtlasVersionIndex::PruneSeasons(size_t keepSeasons)
{
	std::vector<std::string> order = TagsNewestFirst();

	// Seasons in newest-first order, de-duplicated.
	std::vector<std::string> seasons;
	for (const std::string& t : order) {
		std::string s = SeasonOf(t);
		if (std::find(seasons.begin(), seasons.end(), s) == seasons.end())
			seasons.push_back(s);
	}

	std::vector<std::string> dropped;
	for (const std::string& t : order) {
		size_t si = (size_t)(std::find(seasons.begin(), seasons.end(), SeasonOf(t)) - seasons.begin());
		bool keep;
		if (si >= keepSeasons) {
			keep = false;              // season older than the retention window
		} else if (si == 0) {
			keep = true;               // current league: every in-league revision
		} else {
			// An older league keeps only its final revision. `order` is
			// newest-first, so this tag is the final one iff no tag of the same
			// season came before it.
			keep = true;
			for (const std::string& o : order) {
				if (o == t) break;
				if (SeasonOf(o) == SeasonOf(t)) { keep = false; break; }
			}
		}
		if (!keep && t != active_) dropped.push_back(t); // never drop the active season
	}
	if (dropped.empty()) return dropped;

	std::vector<AtlasVersionEntry> kept;
	for (const auto& e : versions_)
		if (std::find(dropped.begin(), dropped.end(), e.tag) == dropped.end())
			kept.push_back(e);
	versions_.swap(kept);
	refreshCompareBase();
	return dropped;
}

// ---- load / save ------------------------------------------------------------

void AtlasVersionIndex::Load(const std::wstring& exeDir)
{
	active_.clear(); compareBase_.clear(); versions_.clear(); lastCheckUtc_ = 0; dirty_ = false;

	std::string content;
	if (read_file_utf8(IndexPath(exeDir), content)) {
		try {
			ordered_json doc = ordered_json::parse(content);
			lastCheckUtc_ = doc.value("lastCheckUtc", 0ll);
			if (doc.contains("versions") && doc["versions"].is_array()) {
				for (const auto& jv : doc["versions"]) {
					AtlasVersionEntry e;
					e.tag = jv.value("tag", std::string());
					e.sha = jv.value("sha", std::string());
					e.repoe = jv.value("repoe", std::string());
					if (!e.tag.empty()) versions_.push_back(std::move(e));
				}
			}
			active_ = doc.value("active", std::string());
			if (active_.empty() || !Has(active_)) {
				std::vector<std::string> nf = TagsNewestFirst();
				active_ = nf.empty() ? std::string() : nf.front();
			}
			compareBase_ = doc.value("compareBase", std::string());
			adoptFromDisk(exeDir); // season folders the index never heard about
			if (compareBase_.empty() || !Has(compareBase_) || compareBase_ == active_)
				refreshCompareBase();
			return;
		} catch (...) {
			// fall through to legacy migration
			active_.clear(); compareBase_.clear(); versions_.clear(); lastCheckUtc_ = 0;
		}
	}

	// Legacy migration: single-slot Data/atlas_version.json {tag,sha,repoe,lastCheckUtc}.
	std::string legacy;
	if (read_file_utf8(exeDir + L"Data\\atlas_version.json", legacy)) {
		try {
			ordered_json doc = ordered_json::parse(legacy);
			AtlasVersionEntry e;
			e.tag = doc.value("tag", std::string());
			e.sha = doc.value("sha", std::string());
			e.repoe = doc.value("repoe", std::string());
			lastCheckUtc_ = doc.value("lastCheckUtc", 0ll);
			if (!e.tag.empty()) UpsertActive(e);
		} catch (...) {
		}
	}
	adoptFromDisk(exeDir); // also covers "index missing entirely, folders present"
	// No file at all: stay empty; ResolveDataDir falls back to the flat layout.
}

bool AtlasVersionIndex::Save(const std::wstring& exeDir) const
{
	ordered_json doc;
	doc["format"] = "pobtools-atlas-index";
	doc["active"] = active_;
	doc["compareBase"] = compareBase_;
	ordered_json arr = ordered_json::array();
	for (const std::string& tag : TagsNewestFirst()) {
		const AtlasVersionEntry* e = Find(tag);
		if (!e) continue;
		ordered_json jv;
		jv["tag"] = e->tag;
		if (!e->sha.empty()) jv["sha"] = e->sha;
		if (!e->repoe.empty()) jv["repoe"] = e->repoe;
		arr.push_back(std::move(jv));
	}
	doc["versions"] = std::move(arr);
	doc["lastCheckUtc"] = lastCheckUtc_;

	// ensure Data/ exists (it always should, but be safe for headless tooling)
	CreateDirectoryW((exeDir + L"Data").c_str(), nullptr);
	bool ok = write_file_utf8(IndexPath(exeDir), doc.dump(1, '\t'));
	if (ok) dirty_ = false;
	return ok;
}

// ---- headless self-test (--atlas-index-selftest) ----------------------------

#include <cstdio>

int RunAtlasVersionIndexSelfTest(const std::wstring& exeDir)
{
	if (AttachConsole(ATTACH_PARENT_PROCESS)) {
		FILE* f = nullptr;
		freopen_s(&f, "CONOUT$", "w", stdout);
	}
	std::string rep;
	int fails = 0;
	auto check = [&](bool ok, const char* what) {
		rep += ok ? "PASS  " : "FAIL  ";
		rep += what; rep += "\n";
		if (!ok) fails++;
	};

	// semver ordering (the prune/active logic all rides on this)
	check(AtlasVersionIndex::CompareSemver("3.29.0", "3.28.0") > 0, "3.29.0 > 3.28.0");
	check(AtlasVersionIndex::CompareSemver("3.28.0", "3.29.0") < 0, "3.28.0 < 3.29.0");
	check(AtlasVersionIndex::CompareSemver("3.29.0", "3.29.0") == 0, "3.29.0 == 3.29.0");
	check(AtlasVersionIndex::CompareSemver("3.9.0", "3.28.0") < 0, "3.9.0 < 3.28.0 (numeric, not lexical)");
	check(AtlasVersionIndex::CompareSemver("3.28.1", "3.28.0") > 0, "3.28.1 > 3.28.0 (patch)");

	// upsert makes each new season active and points compareBase at the previous
	AtlasVersionIndex idx;
	idx.UpsertActive({ "3.27.0", "", "" });
	check(idx.Active() == "3.27.0" && idx.CompareBase().empty(), "first season: active set, no base");
	idx.UpsertActive({ "3.28.0", "", "" });
	check(idx.Active() == "3.28.0" && idx.CompareBase() == "3.27.0", "second season: base = previous");
	idx.UpsertActive({ "3.29.0", "sha", "repoe" });
	check(idx.Active() == "3.29.0" && idx.CompareBase() == "3.28.0", "third season: base = 3.28.0");
	check(idx.Versions().size() == 3, "three seasons registered before prune");

	// newest-first ordering regardless of insertion order
	std::vector<std::string> nf = idx.TagsNewestFirst();
	check(nf.size() == 3 && nf[0] == "3.29.0" && nf[1] == "3.28.0" && nf[2] == "3.27.0",
	      "TagsNewestFirst = 3.29, 3.28, 3.27");

	// rolling prune keeps the newest two leagues, drops the third, never the active one
	std::vector<std::string> dropped = idx.PruneSeasons(2);
	check(dropped.size() == 1 && dropped[0] == "3.27.0", "prune drops exactly 3.27.0");
	check(idx.Versions().size() == 2 && idx.Has("3.29.0") && idx.Has("3.28.0") && !idx.Has("3.27.0"),
	      "after prune: only 3.29.0 + 3.28.0 remain");
	check(idx.Active() == "3.29.0" && idx.CompareBase() == "3.28.0", "active/base intact after prune");

	// re-upserting an existing tag updates it in place (no duplicate)
	idx.UpsertActive({ "3.28.0", "newsha", "" });
	check(idx.Versions().size() == 2 && idx.Active() == "3.28.0" && idx.CompareBase() == "3.29.0",
	      "re-upsert existing tag: no duplicate, active flips, base = 3.29.0");
	check(idx.Find("3.28.0") && idx.Find("3.28.0")->sha == "newsha", "re-upsert updates fields");

	// prune is a no-op when already at/under the keep count
	std::vector<std::string> none = idx.PruneSeasons(2);
	check(none.empty() && idx.Versions().size() == 2, "prune no-op at keep count");

	// --- retention is per LEAGUE, not per tag ---------------------------------
	check(AtlasVersionIndex::SeasonOf("3.29.1") == "3.29" &&
	      AtlasVersionIndex::SeasonOf("3.29") == "3.29" &&
	      AtlasVersionIndex::SeasonOf("3.29.0.4.2") == "3.29" &&
	      AtlasVersionIndex::SeasonOf("weird") == "weird",
	      "SeasonOf strips the patch component");
	{
		// The shape the user actually has: several revisions of the current
		// league, several of the previous one, and an older league.
		AtlasVersionIndex s;
		s.UpsertActive({ "3.27.0", "", "" });
		s.UpsertActive({ "3.28.0", "", "" });
		s.UpsertActive({ "3.28.1", "", "" });
		s.UpsertActive({ "3.28.2", "", "" });
		s.UpsertActive({ "3.29.0", "", "" });
		s.UpsertActive({ "3.29.1", "", "" });
		check(s.Versions().size() == 6, "six tags before the per-league prune");

		std::vector<std::string> gone = s.PruneSeasons(2);
		// current league keeps BOTH revisions; 3.28 keeps only its last; 3.27 goes
		check(s.Has("3.29.0") && s.Has("3.29.1"),
		      "current league keeps every revision (3.29.0 AND 3.29.1)");
		check(s.Has("3.28.2") && !s.Has("3.28.1") && !s.Has("3.28.0"),
		      "previous league keeps only its final revision (3.28.2)");
		check(!s.Has("3.27.0"), "leagues outside the window are dropped");
		check(s.Versions().size() == 3, "three tags survive");
		check(gone.size() == 3, "three tags reported as dropped for folder deletion");
		// With 3.29.1 active, the newest version older than it is its own
		// sibling 3.29.0 — so the default diff is the mid-league one. That is
		// the useful default now that in-league revisions are retained, and it
		// is asserted so it stays a decision rather than an accident.
		check(s.Active() == "3.29.1" && s.CompareBase() == "3.29.0",
		      "compare base is the newest OLDER version, i.e. the sibling revision");
	}
	{
		// The moment a NEW league lands: the outgoing league collapses to its
		// final revision and the one before it goes entirely.
		AtlasVersionIndex s;
		s.UpsertActive({ "3.28.2", "", "" });
		s.UpsertActive({ "3.29.0", "", "" });
		s.UpsertActive({ "3.29.1", "", "" });
		s.UpsertActive({ "3.30.0", "", "" }); // new league arrives and becomes active
		std::vector<std::string> gone = s.PruneSeasons(2);
		check(s.Has("3.30.0"), "new league retained");
		check(s.Has("3.29.1") && !s.Has("3.29.0"),
		      "outgoing league collapses to its final revision (3.29.1 kept, 3.29.0 dropped)");
		check(!s.Has("3.28.2"), "the league before that is dropped entirely");
		check(s.Versions().size() == 2 && gone.size() == 2, "exactly two tags survive the rollover");
		check(s.CompareBase() == "3.29.1", "compare base is the previous league's final revision");
	}
	{
		// Compare base must be OLDER than active, not merely "not active" —
		// otherwise a mid-league revision installed alongside the active one
		// points the default diff backwards.
		AtlasVersionIndex s;
		s.UpsertActive({ "3.28.0", "", "" });
		s.UpsertActive({ "3.29.0", "", "" });
		s.UpsertActive({ "3.29.1", "", "" });
		s.SetActive("3.29.0"); // user is viewing the earlier revision
		check(s.CompareBase() == "3.28.0",
		      "base is the newest version OLDER than active, not the newer sibling");
	}
	{
		// The active season is never dropped, even when its league is outside the
		// window (a user previewing an old league must not lose it underfoot).
		AtlasVersionIndex s;
		s.UpsertActive({ "3.29.0", "", "" });
		s.UpsertActive({ "3.28.0", "", "" });
		s.UpsertActive({ "3.27.0", "", "" });
		s.SetActive("3.27.0");
		std::vector<std::string> gone = s.PruneSeasons(2);
		check(gone.empty() && s.Has("3.27.0"), "the active season survives an out-of-window prune");
	}

	// OlderThan: the newest installed season strictly older than a given tag
	// (backs the compare base + the cross-season TC backfill source selection)
	{
		AtlasVersionIndex oi;
		oi.UpsertActive({ "3.27.0", "", "" });
		oi.UpsertActive({ "3.28.0", "", "" });
		oi.UpsertActive({ "3.29.0", "", "" });
		check(oi.OlderThan("3.29.0") == "3.28.0", "OlderThan(3.29.0) = 3.28.0");
		check(oi.OlderThan("3.28.0") == "3.27.0", "OlderThan(3.28.0) = 3.27.0");
		check(oi.OlderThan("3.27.0").empty(), "OlderThan(oldest) = empty");
	}

	// --- against the REAL install: every season folder holding a tree must be
	// reachable through the index. The in-memory cases above cannot catch an
	// index file that simply never listed a folder sitting on disk, which is the
	// state a downloaded 3.29.1 was found in.
	{
		std::wstring root = exeDir + L"Data\\atlas_versions\\";
		std::vector<std::string> onDisk;
		WIN32_FIND_DATAW fd{};
		HANDLE h = FindFirstFileW((root + L"*").c_str(), &fd);
		if (h != INVALID_HANDLE_VALUE) {
			do {
				if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
				std::wstring n = fd.cFileName;
				if (n == L"." || n == L"..") continue;
				std::string tag(n.begin(), n.end());
				if (file_exists(root + n + L"\\atlas_tree_poe1.json")) onDisk.push_back(tag);
			} while (FindNextFileW(h, &fd));
			FindClose(h);
		}
		if (onDisk.empty()) {
			rep += "      no installed season folders here - on-disk adoption check skipped\n";
		} else {
			AtlasVersionIndex live;
			live.Load(exeDir);
			std::string missing;
			for (const std::string& t : onDisk)
				if (!live.Has(t)) missing += (missing.empty() ? "" : ",") + t;
			check(missing.empty(), "every season folder on disk is registered in the index");
			if (!missing.empty()) rep += "      missing: " + missing + "\n";
			check(!live.Active().empty(), "a live index resolves an active season");
			check(live.Has(live.Active()), "the active season is one of the registered ones");
			// The newest COMPLETE season on disk is canonical. Anything else and
			// the planner marks it "舊賽季檢視（唯讀）" and silently refuses to
			// save into it -- which is what a downloaded 3.29.1 did while the
			// packaged atlas_index.json still said 3.29.0.
			std::string newestComplete;
			for (const std::string& t : onDisk) {
				std::wstring vd = AtlasVersionIndex::VersionDir(exeDir, t);
				if (!file_exists(vd + L"atlas_tree_zh.json") || !dir_exists(vd + L"atlas")) continue;
				if (newestComplete.empty() ||
				    AtlasVersionIndex::CompareSemver(t, newestComplete) > 0) newestComplete = t;
			}
			if (!newestComplete.empty()) {
				check(live.Active() == newestComplete,
				      "active = the newest complete season on disk");
				if (live.Active() != newestComplete)
					rep += "      active=" + live.Active() + " newest on disk=" + newestComplete + "\n";
			}
		}
	}

	// --- adoption against a synthetic install tree ----------------------------
	// The real-install block above can only observe whatever this machine
	// happens to have. This one builds the exact situation the bug came from
	// (index names an older season, a newer complete one sits on disk) plus the
	// case that must NOT be adopted (a half-finished download), in a throwaway
	// directory so nothing real is touched.
	{
		wchar_t tmp[MAX_PATH]{};
		GetTempPathW(MAX_PATH, tmp);
		std::wstring root = std::wstring(tmp) + L"pobtools_idx_selftest\\";
		auto rmrf = [](const std::wstring& dir, auto&& self) -> void {
			WIN32_FIND_DATAW f{};
			HANDLE hh = FindFirstFileW((dir + L"*").c_str(), &f);
			if (hh != INVALID_HANDLE_VALUE) {
				do {
					std::wstring n = f.cFileName;
					if (n == L"." || n == L"..") continue;
					if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) self(dir + n + L"\\", self);
					else DeleteFileW((dir + n).c_str());
				} while (FindNextFileW(hh, &f));
				FindClose(hh);
			}
			RemoveDirectoryW(dir.c_str());
		};
		rmrf(root, rmrf);
		auto season = [&](const wchar_t* tag, bool complete) {
			std::wstring d = root + L"Data\\atlas_versions\\" + tag + L"\\";
			CreateDirectoryW(d.c_str(), nullptr);
			write_file_utf8(d + L"atlas_tree_poe1.json", "{}");
			if (complete) {
				write_file_utf8(d + L"atlas_tree_zh.json", "{}");
				CreateDirectoryW((d + L"atlas").c_str(), nullptr);
			}
		};
		CreateDirectoryW(root.c_str(), nullptr);
		CreateDirectoryW((root + L"Data").c_str(), nullptr);
		CreateDirectoryW((root + L"Data\\atlas_versions").c_str(), nullptr);
		season(L"3.28.0", true);
		season(L"3.29.0", true);
		season(L"3.29.1", true);    // downloaded in-app, never made it into the index
		season(L"3.30.0", false);   // interrupted download: tree only, no zh, no sprites
		write_file_utf8(root + L"Data\\atlas_index.json",
			"{\"format\":\"pobtools-atlas-index\",\"active\":\"3.29.0\",\"compareBase\":\"3.28.0\","
			"\"versions\":[{\"tag\":\"3.29.0\"},{\"tag\":\"3.28.0\"}],\"lastCheckUtc\":0}");

		AtlasVersionIndex s;
		s.Load(root);
		check(s.Has("3.29.1"), "a season folder missing from the index is adopted");
		check(s.Active() == "3.29.1", "adoption promotes active to the newer complete season");
		check(!s.Has("3.30.0"), "an incomplete season folder is NOT adopted");
		check(s.CompareBase() == "3.29.0", "compare base follows the promoted active");
		check(s.NeedsSave(), "a repaired index reports that it needs writing back");
		// Writing it back must make the repair stick with nothing left to do.
		check(s.Save(root) && !s.NeedsSave(), "saving clears the needs-write flag");
		AtlasVersionIndex again;
		again.Load(root);
		check(again.Active() == "3.29.1" && !again.NeedsSave(),
		      "reloading the saved index needs no further repair");
		rmrf(root, rmrf);
	}

	rep += fails == 0 ? "\nALL PASS\n" : "\nFAILURES: " + std::to_string(fails) + "\n";
	printf("%s", rep.c_str());
	write_file_utf8(exeDir + L"atlas_index_selftest.txt", rep); // report file, per selftest convention
	return fails == 0 ? 0 : 1;
}
