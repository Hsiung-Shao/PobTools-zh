// PobTools atlas-tree auto updater.
//
// Checks GitHub grindinggear/atlastree-export for the newest release tag,
// downloads data.json plus the referenced sprite sheets, feeds them through the
// existing ImportAtlasTreeData() validation, and (re)builds the bundled
// English/Traditional-Chinese node mapping (Data/atlas_tree_zh.json) from the
// repoe-fork dataset. Network and conversion run on one worker thread; the
// planner polls Poll() each frame and performs the GL hot-reload on Done.
//
// Never downgrades on failure: the tree json, the zh mapping and the version
// record are each only overwritten after their replacement fully validated.
#pragma once

#include "data_updater.h"

#include <string>

enum class AtlasUpdatePhase {
	Idle,            // nothing to report (includes throttled-away checks)
	Checking,        // querying GitHub tags / repoe-fork version
	UpToDate,        // check finished: local data already newest
	UpdateAvailable, // newer tag found; waiting for the user to click update
	Downloading,     // fetching data.json + sprite sheets
	Importing,       // ImportAtlasTreeData + zh-mapping rebuild
	Done,            // update finished; waiting for the main thread to reload
	Error,
};

struct AtlasUpdateStatus {
	AtlasUpdatePhase phase = AtlasUpdatePhase::Idle;
	std::string latestTag;          // set while UpdateAvailable/Downloading/...
	std::string message;            // UTF-8 progress / result / error text
	int filesDone = 0, filesTotal = 0; // Downloading progress
	bool reloadPending = false;     // Done: tree (and maybe zh) changed on disk
	bool zhRefreshed = false;       // only the zh mapping changed (no GL work)
};

// Init loads Data/atlas_version.json (also the daily-throttle record);
// StartUpdate downloads + imports latestTag; AckReload also clears zhRefreshed.
// Worker failures are not logged (check: back to Idle; update: Error).
class AtlasUpdater : public DataUpdater<AtlasUpdatePhase, AtlasUpdateStatus> {
public:
	using Status = AtlasUpdateStatus;

private:
	friend int RunAtlasUpdateCli(const std::wstring& exeDir); // headless reuse of doCheck/doUpdate

	void loadRecord() override;
	bool doCheck(std::string* err) override;     // worker
	bool doUpdate(std::string* err) override;    // worker
	void ackExtra(Status& st) override { st.zhRefreshed = false; }
	bool refreshZhMapping(const std::string& tag, const std::string& ref, const std::string& repoeVer, std::string* err);
	void saveVersionRecord();

	// version record + check result cache (worker-thread only after Init)
	std::string tag_, sha_, repoe_;
	std::string latestTag_, latestSha_; // newest remote tag from the last doCheck
};

// Builds atlas_tree_zh.json inside destDir (trailing backslash, e.g. a season
// folder Data/atlas_versions/<tag>/) from a GGG atlastree-export data.json
// (English names/stats) joined with repoe-fork's Traditional-Chinese Atlas.json
// on the node hash. enAtlasJson (repoe-fork English Atlas.json, same row order
// as the TC one) pairs statsZh to statsEn by full text; empty or unparsable =
// number matching only. Writes only after both inputs parsed and the join is
// sane; a zero join keeps the previous mapping. Shared by the worker and the CLIs.
bool GenerateAtlasZhMapping(const std::string& gggDataJson, const std::string& tcAtlasJson,
                            const std::string& tag, const std::string& repoeVersion,
                            const std::wstring& destDir, std::string* err, std::string* summary,
                            const std::string& enAtlasJson = std::string());

// Newest atlas data on GitHub: the highest plain-semver tag, or the default
// branch head when its commit message starts with that same version (GGG
// pushed "3.30.0c" to master without tagging it). headBody = GET commits?per_page=1,
// empty/unparsable = tag only. Download by *sha, not by tag name.
bool PickAtlasRemote(const std::string& tagsBody, const std::string& headBody,
                     std::string* tag, std::string* sha, std::string* err);
// Local tag/sha vs remote: new tag, or same tag with a different known sha.
bool AtlasNeedsUpdate(const std::string& localTag, const std::string& localSha,
                      const std::string& remoteTag, const std::string& remoteSha);

// "pob-zh.exe --atlas-update": headless check + download + import + zh mapping
// (ignores the daily throttle). Prints the outcome; 0 on success/up-to-date.
int RunAtlasUpdateCli(const std::wstring& exeDir);

// "pob-zh.exe --atlas-zh <data.json> <Atlas_tc.json> [<Atlas_en.json>]": offline
// zh-mapping rebuild from local files (join-logic test bed).
int RunAtlasZhCli(const std::wstring& dataJsonPath, const std::wstring& tcJsonPath,
                  const std::wstring& enJsonPath, const std::wstring& exeDir);
