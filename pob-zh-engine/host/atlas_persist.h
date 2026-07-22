// PobTools atlas planner persistence: multi-build file schema (with silent
// migration from the original single-build format), build export/import as
// json or a clipboard share code, and small UI state (panel width).
//
// Pure data, no ImGui/GL. The build file keeps its original path
// (PobTools/atlas_build_poe1.json) and is upgraded in place on first save.
#pragma once

#include <string>
#include <vector>

struct AtlasBuildEntry {
	std::string name;       // UTF-8 display name
	std::vector<int> alloc; // GGG skill ids, start node excluded
};

struct AtlasBuildFile {
	std::string version;    // tree version at last save (informational)
	int active = 0;
	std::vector<AtlasBuildEntry> builds;

	static std::wstring PathOf(const std::wstring& exeDir);

	// Load resets to a single empty "預設" build when the file is missing or
	// invalid (and returns false); parsed content only replaces the members
	// when fully valid.
	bool Load(const std::wstring& exeDir);
	bool Save(const std::wstring& exeDir) const;

	// String-level codec, selftest-friendly. ParseDoc accepts both the current
	// multi-build schema and the legacy {version, alloc[]} single-build file
	// (migrated into builds[0] = "預設").
	bool ParseDoc(const std::string& json);
	std::string SerializeDoc() const;

	AtlasBuildEntry& Active();               // clamps active into range
	int AddBuild(const std::string& name);   // returns the new index
	int DuplicateBuild(int idx);             // name + "（複製）"; -1 when idx invalid
	bool RemoveBuild(int idx);               // refuses to drop the last build
	std::string UniqueName(const std::string& want) const; // "name (2)" on clash
};

// Single-build export document: {"format":"pobtools-atlas-build", "version",
// "name", "alloc":[ids]}.
std::string AtlasExportJson(const AtlasBuildEntry& b, const std::string& treeVersion);
bool AtlasParseExportJson(const std::string& json, AtlasBuildEntry* out, std::string* err);

// Share code = "PTAT1|" + base64(export json). Parse trims whitespace first.
std::string AtlasBuildShareCode(const AtlasBuildEntry& b, const std::string& treeVersion);
bool AtlasParseShareCode(const std::string& code, AtlasBuildEntry* out, std::string* err);

// Planner UI state (PobTools/atlas_ui.json). panelW is in logical pixels
// (unscaled); 0 means "use the default responsive width".
struct AtlasUiState {
	float panelW = 0.0f;
	std::string season;   // last atlas season the user viewed (e.g. "3.29.0"); "" = default active
	bool Load(const std::wstring& exeDir);
	bool Save(const std::wstring& exeDir) const;
};
