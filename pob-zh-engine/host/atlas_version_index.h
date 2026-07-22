// PobTools atlas-tree version registry.
//
// Replaces the old single-slot Data/atlas_version.json with a rolling registry
// of the atlas passive-tree seasons installed on disk, so two leagues can live
// side by side (Data/atlas_versions/<tag>/) and be diffed. The auto updater
// installs new seasons here and prunes to the newest two; the planner resolves
// which season to load through this index.
//
// Backward compatible: a legacy flat install (Data/atlas_tree_poe1.json +
// Data/atlas_version.json, no atlas_versions/ dir) keeps working — the resolver
// falls back to the flat Data/ directory when a version subfolder is absent.
//
// Pure data, no ImGui / GL / network.
#pragma once

#include <string>
#include <vector>

struct AtlasVersionEntry {
	std::string tag;    // GGG release tag, e.g. "3.29.0"
	std::string sha;    // source commit (optional)
	std::string repoe;  // repoe-fork TC dataset version (optional)
};

class AtlasVersionIndex {
public:
	// Loads Data/atlas_index.json under exeDir. When absent, migrates in-memory
	// from the legacy Data/atlas_version.json (single tag) if present; otherwise
	// stays empty. Never fails hard — always leaves a usable object.
	void Load(const std::wstring& exeDir);
	bool Save(const std::wstring& exeDir) const;

	// Active = the season the planner loads by default; empty when nothing known.
	const std::string& Active() const { return active_; }
	// Base season the version-compare view diffs against (usually the previous).
	const std::string& CompareBase() const { return compareBase_; }
	const std::vector<AtlasVersionEntry>& Versions() const { return versions_; }
	long long LastCheckUtc() const { return lastCheckUtc_; }
	void SetLastCheckUtc(long long v) { lastCheckUtc_ = v; }

	bool Has(const std::string& tag) const;
	const AtlasVersionEntry* Find(const std::string& tag) const;

	// Tags sorted newest-first by semver.
	std::vector<std::string> TagsNewestFirst() const;

	// Newest installed season strictly older than `tag` ("" if none). Used to pick
	// the compare base and the source season for the cross-season TC backfill.
	std::string OlderThan(const std::string& tag) const;

	// Insert or update an entry, then set it active and point compareBase at the
	// next-newest installed season.
	void UpsertActive(const AtlasVersionEntry& e);
	// Set active explicitly (must already exist); refreshes compareBase.
	void SetActive(const std::string& tag);

	// Keep only the `keep` newest seasons; returns the dropped tags so the caller
	// can delete their directories.
	std::vector<std::string> PruneToNewest(size_t keep);

	// Data/atlas_versions/<tag>/ (trailing backslash), regardless of existence.
	static std::wstring VersionDir(const std::wstring& exeDir, const std::string& tag);
	// Data/atlas_index.json
	static std::wstring IndexPath(const std::wstring& exeDir);

	// Directory that actually holds `tag`'s data files: the version subfolder if
	// it exists, else the legacy flat Data/ dir. tag "" means the active season.
	// Always returns a path ending in a backslash.
	std::wstring ResolveDataDir(const std::wstring& exeDir, const std::string& tag) const;

	// -1 / 0 / +1 by semver ("3.29.0" > "3.28.0"); non-numeric parts compare 0.
	static int CompareSemver(const std::string& a, const std::string& b);

private:
	void refreshCompareBase();

	std::string active_, compareBase_;
	std::vector<AtlasVersionEntry> versions_;
	long long lastCheckUtc_ = 0;
};

// "pob-zh.exe --atlas-index-selftest": headless check of semver ordering,
// upsert/active/compareBase and rolling prune (in-memory; no file I/O). Prints a
// console report and returns 0 on full pass.
int RunAtlasVersionIndexSelfTest(const std::wstring& exeDir);
