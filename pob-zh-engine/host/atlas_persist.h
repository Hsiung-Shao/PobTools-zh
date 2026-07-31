// PobTools atlas planner persistence: multi-build file schema (with silent
// migration from the original single-build format), build export/import as
// json or a clipboard share code, and small UI state (panel width).
//
// Pure data, no ImGui/GL. The build file keeps its original path
// (PobTools/atlas_build_poe1.json) and is upgraded in place on first save.
#pragma once

#include <string>
#include <vector>

// One astrolabe placed on one atlas quadrant. Lives here rather than in
// atlas_astrolabes.h because it is a shape the SAVE FILE has: putting it the
// other way round would make the persistence header depend on the catalogue
// header while the catalogue's self-test depends on this one.
struct AstrolabePlacement {
	std::string region; // AtlasRegions id: "NorthWest" / "NorthEast" / "SouthEast" / "SouthWest"
	std::string id;     // "Metadata/Items/Currency/Astrolabe..."
};

// `notes`, `scarabs`, `targets`, `astrolabes` and `mapId` were all added after
// the format shipped. All are written ONLY when non-empty, so a build that uses
// none of them serializes byte-identically to what earlier versions produced,
// and an older PobTools reading a newer file just ignores the extra keys. Keep
// that property when adding further fields.
struct AtlasBuildEntry {
	std::string name;                  // UTF-8 display name
	std::vector<int> alloc;            // GGG skill ids, start node excluded
	std::string notes;                 // free-form project note (may contain newlines)
	std::vector<std::string> scarabs;  // scarab Metadata ids, in placement order (max 5)
	// At most one per quadrant (the game refuses a second Shaped Region on a
	// quadrant); serialized as an OBJECT keyed by quadrant so the document stays
	// sparse and independent of quadrant ordering.
	std::vector<AstrolabePlacement> astrolabes;
	std::string mapId;                 // the project's main map (WorldAreas.Id), "" = none
	// The tier the project intends to RUN that map at, which is not the map's
	// own tier: once a quadrant's Voidstone is socketed every map in it becomes
	// T16. So this is the user's plan, stored independently of mapId, and the
	// map picker is never filtered by it. 0 = unset, 99 = unique maps.
	int mapTier = 0;
	// Nodes the user picked deliberately, as GGG skill ids. Everything else in
	// `alloc` is wiring the solver chose and may re-route freely (see
	// atlas_optimize.h). Empty alongside a non-empty `alloc` means "saved
	// before targets existed" and triggers the one-time migration prompt.
	std::vector<int> targets;
	// Nodes the user ruled out; the solver routes around them. Kept so a
	// planning session can be resumed instead of re-marked from scratch.
	std::vector<int> blocked;
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
