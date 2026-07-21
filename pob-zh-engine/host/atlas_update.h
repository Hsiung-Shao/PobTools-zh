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

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

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

class AtlasUpdater {
public:
	void Init(const std::wstring& exeDir);  // loads Data/atlas_version.json, starts the worker
	void Shutdown();                        // cancels any transfer and joins the worker

	// Main thread. Queues a version check; without force it is throttled to
	// once per day (persisted in Data/atlas_version.json).
	void RequestCheck(bool force);

	// Main thread. Valid in UpdateAvailable/Error: downloads + imports latestTag.
	void StartUpdate();

	struct Status {
		AtlasUpdatePhase phase = AtlasUpdatePhase::Idle;
		std::string latestTag;          // set while UpdateAvailable/Downloading/...
		std::string message;            // UTF-8 progress / result / error text
		int filesDone = 0, filesTotal = 0; // Downloading progress
		bool reloadPending = false;     // Done: tree (and maybe zh) changed on disk
		bool zhRefreshed = false;       // only the zh mapping changed (no GL work)
	};
	Status Poll();                      // main thread, each frame: snapshot
	void AckReload();                   // main thread: after handling reload/refresh

private:
	friend int RunAtlasUpdateCli(const std::wstring& exeDir); // headless reuse of doCheck/doUpdate

	enum class Cmd { Check, Update };
	void workerLoop();
	bool doCheck(std::string* err);     // worker
	bool doUpdate(std::string* err);    // worker
	bool refreshZhMapping(const std::string& tag, const std::string& repoeVer, std::string* err);
	void saveVersionRecord();

	void setPhase(AtlasUpdatePhase p, const std::string& msg);

	std::wstring exeDir_;
	std::thread worker_;
	std::atomic<bool> stop_{ false };

	std::mutex cmdMx_;
	std::condition_variable cmdCv_;
	std::deque<Cmd> cmdQ_;

	std::mutex stMx_;
	Status st_;

	// version record + check result cache (worker-thread only after Init)
	std::string tag_, sha_, repoe_;
	std::string latestTag_, latestSha_; // newest remote tag from the last doCheck
	std::atomic<long long> lastCheckUtc_{ 0 }; // FILETIME (100ns units)
};

// Builds Data/atlas_tree_zh.json from a GGG atlastree-export data.json (English
// names/stats) joined with repoe-fork's Traditional-Chinese Atlas.json on the
// node hash. Writes only after both inputs parsed and the join is sane; a zero
// join keeps the previous mapping. Shared by the worker and the CLIs.
bool GenerateAtlasZhMapping(const std::string& gggDataJson, const std::string& tcAtlasJson,
                            const std::string& tag, const std::string& repoeVersion,
                            const std::wstring& exeDir, std::string* err, std::string* summary);

// "pob-zh.exe --atlas-update": headless check + download + import + zh mapping
// (ignores the daily throttle). Prints the outcome; 0 on success/up-to-date.
int RunAtlasUpdateCli(const std::wstring& exeDir);

// "pob-zh.exe --atlas-zh <data.json> <Atlas_tc.json>": offline zh-mapping
// rebuild from local files (join-logic test bed).
int RunAtlasZhCli(const std::wstring& dataJsonPath, const std::wstring& tcJsonPath,
                  const std::wstring& exeDir);
