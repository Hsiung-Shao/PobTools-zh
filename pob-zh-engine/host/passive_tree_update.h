// PobTools passive-tree (timeless-jewel view) updater.
//
// Checks BOTH sources for a newer league than the bundled tree
// (Data/passive_tree_poe1.json's "treeVersion", e.g. "3_29"):
//   a. the user's local PoB install — a bare TreeData\3_NN\ folder newer than
//      the bundled tree (PoB self-updated first; its sheets are copied locally,
//      no download needed for sprites), and
//   b. GitHub grindinggear/skilltree-export release tags (the stopgap source
//      when GGG publishes a league before PoB ships it).
// The newer of the two wins. Updating downloads that tag's data.json (node data
// always comes from the GGG export; PoB's tree.lua is not parseable here),
// fetches only the sprite sheets PoB doesn't already have, and feeds everything
// through ImportPassiveTreeData() — a SINGLE in-place tree (no version dirs).
//
// Same worker-thread / Poll() contract and once-per-day throttle as
// AtlasUpdater; the record lives in Data/passive_update.json.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

enum class PassiveUpdatePhase {
	Idle,            // nothing to report (includes throttled-away checks)
	Checking,        // scanning PoB TreeData / querying GitHub tags
	UpToDate,        // check finished: bundled tree already newest
	UpdateAvailable, // newer league found; waiting for the user to click update
	Downloading,     // fetching data.json + missing sprite sheets
	Importing,       // ImportPassiveTreeData
	Done,            // update finished; waiting for the main thread to reload
	Error,
};

class PassiveTreeUpdater {
public:
	void Init(const std::wstring& exeDir);  // reads the bundled treeVersion + throttle record, starts the worker
	void Shutdown();                        // cancels any transfer and joins the worker

	// Main thread. Queues a version check; without force it is throttled to
	// once per day (persisted in Data/passive_update.json).
	void RequestCheck(bool force);

	// Main thread. Valid in UpdateAvailable/Error: downloads + imports latestVer.
	void StartUpdate();

	struct Status {
		PassiveUpdatePhase phase = PassiveUpdatePhase::Idle;
		std::string latestVer;          // folder style ("3_29"), set from UpdateAvailable on
		std::string message;            // UTF-8 progress / result / error text
		bool reloadPending = false;     // Done: tree (and sheets) changed on disk
	};
	Status Poll();                      // main thread, each frame: snapshot
	void AckReload();                   // main thread: after handling the reload

private:
	friend int RunPassiveTreeUpdateCli(const std::wstring& exeDir); // headless reuse

	enum class Cmd { Check, Update };
	void workerLoop();
	bool doCheck(std::string* err);     // worker
	bool doUpdate(std::string* err);    // worker
	void saveRecord();

	void setPhase(PassiveUpdatePhase p, const std::string& msg);

	std::wstring exeDir_;
	std::thread worker_;
	std::atomic<bool> stop_{ false };

	std::mutex cmdMx_;
	std::condition_variable cmdCv_;
	std::deque<Cmd> cmdQ_;

	std::mutex stMx_;
	Status st_;

	// worker-thread state after Init
	std::string currentVer_;            // bundled treeVersion ("3_28"); "" when absent
	std::string latestVer_;             // newest found, folder style ("3_29")
	std::string latestTag_;             // full GitHub tag for the download ("3.29.0")
	std::atomic<long long> lastCheckUtc_{ 0 }; // FILETIME (100ns units)
};

// "pob-zh.exe --pt-update": headless check + download + import (ignores the
// daily throttle). Prints the outcome; 0 on success/up-to-date.
int RunPassiveTreeUpdateCli(const std::wstring& exeDir);
