// PobTools atlas planner persistence: multi-build file schema (silent migration from the single-build format),
// build export/import as json or a clipboard share code, and small UI state. Pure data, no ImGui/GL.
// The build file keeps its path (PobTools/atlas_build_poe1.json) and is upgraded in place on first save.
#pragma once

#include <map>
#include <string>
#include <vector>

// One astrolabe placed on one atlas quadrant. Lives here (not atlas_astrolabes.h) because it is a SAVE FILE shape;
// the other way round would make this header depend on the catalogue header, whose self-test depends on this one.
struct AstrolabePlacement {
	std::string region; // AtlasRegions id: "NorthWest" / "NorthEast" / "SouthEast" / "SouthWest"
	std::string id;     // "Metadata/Items/Currency/Astrolabe..."
};

// One 3.30 thread slot of the map device: a thread type and how many are socketed (lives here like AstrolabePlacement).
struct ThreadSlot {
	std::string id;
	int count = 1;
	bool operator==(const ThreadSlot& o) const { return id == o.id && count == o.count; }
};

// One line of a bound revenue record: an item the stretch produced, valued by
// its count change (the market moving stock already held is not production).
struct AtlasProfitItem {
	std::string en;
	std::string zh;        // stored when bound: the share code's recipient needs no dictionary
	long long dCount = 0;
	double chaos = 0.0;
};

// A stretch of the stash revenue history (倉庫收益) bound to a project. Unlike mapPrice / cost this IS part of the
// build (export json and share code carry it). All money in chaos; divineRate (at toUtc) converts.
struct AtlasProfitRecord {
	long long fromUtc = 0, toUtc = 0;
	std::string league;
	double hours = 0.0;
	double qtyGain = 0.0;    // value of items gained (>= 0)
	double qtyLoss = 0.0;    // value of items spent (<= 0)
	double priceMove = 0.0;  // the market re-pricing stock held throughout
	double net = 0.0;        // total stash value change = the three above
	double divineRate = 0.0; // 0 = unknown
	double costPerMap = 0.0; // the project's cost card total when bound; 0 = unknown
	std::vector<AtlasProfitItem> top; // the biggest gains, at most 10
	bool empty() const { return fromUtc <= 0 || toUtc <= fromUtc; }
};

// `notes`, `scarabs`, `targets`, `astrolabes` and `mapId` were added after the format shipped. All are written ONLY
// when non-empty, so a build without them serializes byte-identically to earlier versions and older PobTools just
// ignores extra keys. Keep that property when adding fields.
struct AtlasBuildEntry {
	std::string name;                  // UTF-8 display name
	std::vector<int> alloc;            // GGG skill ids, start node excluded
	std::string notes;                 // free-form project note (may contain newlines)
	std::vector<std::string> scarabs;  // scarab Metadata ids, in placement order (max 5)
	// 3.30 thread slots, in slot order (max 3). JSON key "threads": [{"id": "...", "n": 6}, ...]; written only
	// when non-empty, shared like scarabs.
	std::vector<ThreadSlot> threads;
	// At most one per quadrant (the game refuses a second Shaped Region); serialized as an OBJECT keyed by quadrant (sparse, order-independent).
	std::vector<AstrolabePlacement> astrolabes;
	std::string mapId;                 // the project's main map (WorldAreas.Id), "" = none
	// The tier the project intends to RUN that map at (not the map's own tier: a socketed Voidstone makes every map
	// in the quadrant T16). Independent of mapId; the picker is never filtered by it. 0 = unset, 99 = unique maps.
	int mapTier = 0;
	// Nodes the user picked deliberately (GGG skill ids); the rest of `alloc` is solver wiring that may re-route
	// (atlas_optimize.h). Empty alongside a non-empty `alloc` = "saved before targets existed" -> one-time migration prompt.
	std::vector<int> targets;
	// Nodes the user ruled out; the solver routes around them (kept so a planning session can resume).
	std::vector<int> blocked;
	// Picked options of multiple-choice nodes (3.30+): node id -> effect id.
	// Part of the build, so it travels through the export json and share code.
	std::map<int, int> variants;
	// What the player pays for one map of this project, in chaos (the 收益 tab's cost card). 0 = unset.
	// A market price, not part of the build: build file only, never the export json or share code.
	double mapPrice = 0.0;
	// What the player PAID for this project's map-device items, item id -> chaos per unit. Same rules as mapPrice
	// (build file only, written when set). costRecordedUtc = when the market was last recorded (0 = never).
	std::map<std::string, double> costPrices;
	long long costRecordedUtc = 0;
	// How many maps the player plans to run (the cost card shows the whole plan). Same rules as mapPrice (written when > 0).
	int plannedMaps = 0;
	// Shared with the build (write_extras): see AtlasProfitRecord.
	AtlasProfitRecord profit;
};

struct AtlasBuildFile {
	std::string version;    // tree version at last save (informational)
	int active = 0;
	std::vector<AtlasBuildEntry> builds;

	static std::wstring PathOf(const std::wstring& exeDir);

	// Load resets to a single empty "預設" build when the file is missing or invalid (returns false); parsed content replaces members only when fully valid.
	bool Load(const std::wstring& exeDir);
	bool Save(const std::wstring& exeDir) const;

	// String-level codec. ParseDoc accepts the multi-build schema and the legacy {version, alloc[]} file (migrated into builds[0] = "預設").
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

// Planner UI state (PobTools/atlas_ui.json). panelW is in unscaled logical pixels; 0 = default responsive width.
struct AtlasUiState {
	float panelW = 0.0f;
	std::string season;   // last atlas season the user viewed (e.g. "3.29.0"); "" = default active
	bool Load(const std::wstring& exeDir);
	bool Save(const std::wstring& exeDir) const;
};
