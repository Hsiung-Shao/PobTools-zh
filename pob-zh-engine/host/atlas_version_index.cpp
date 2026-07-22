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
	std::vector<std::string> tags = TagsNewestFirst();
	compareBase_.clear();
	for (const std::string& t : tags)
		if (t != active_) { compareBase_ = t; break; } // newest that is not the active one
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

std::vector<std::string> AtlasVersionIndex::PruneToNewest(size_t keep)
{
	std::vector<std::string> order = TagsNewestFirst();
	std::vector<std::string> dropped;
	if (order.size() <= keep) return dropped;
	for (size_t i = keep; i < order.size(); i++) {
		if (order[i] == active_) continue; // never drop the active season
		dropped.push_back(order[i]);
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
	active_.clear(); compareBase_.clear(); versions_.clear(); lastCheckUtc_ = 0;

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
	return write_file_utf8(IndexPath(exeDir), doc.dump(1, '\t'));
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

	// rolling prune keeps the newest two, drops the third, never the active one
	std::vector<std::string> dropped = idx.PruneToNewest(2);
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
	std::vector<std::string> none = idx.PruneToNewest(2);
	check(none.empty() && idx.Versions().size() == 2, "prune no-op at keep count");

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

	rep += fails == 0 ? "\nALL PASS\n" : "\nFAILURES: " + std::to_string(fails) + "\n";
	printf("%s", rep.c_str());
	write_file_utf8(exeDir + L"atlas_index_selftest.txt", rep); // report file, per selftest convention
	return fails == 0 ? 0 : 1;
}
