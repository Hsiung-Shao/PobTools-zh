// PobTools passive-tree (timeless-jewel view) updater.
//
// Checks BOTH sources for a newer league than the bundled tree (Data/passive_tree_poe1.json's
// "treeVersion", e.g. "3_29"):
//   a. the local PoB install: a bare TreeData\3_NN\ folder newer than the bundled tree (sprite
//      sheets are copied locally), and
//   b. GitHub grindinggear/skilltree-export release tags (when GGG publishes before PoB ships).
// The newer wins. Updating downloads that tag's data.json (node data always comes from the GGG export;
// PoB's tree.lua is not parseable here), fetches only the missing sprite sheets, and feeds it through
// ImportPassiveTreeData() -- a SINGLE in-place tree (no version dirs).
//
// Same worker-thread / Poll() contract and once-per-day throttle as AtlasUpdater
// (record in Data/passive_update.json).
#pragma once

#include "data_updater.h"

#include <string>

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

struct PassiveUpdateStatus {
	PassiveUpdatePhase phase = PassiveUpdatePhase::Idle;
	std::string latestVer;          // folder style ("3_29"), set from UpdateAvailable on
	std::string message;            // UTF-8 progress / result / error text
	bool reloadPending = false;     // Done: tree (and sheets) changed on disk
};

// Init reads the bundled treeVersion + Data/passive_update.json (throttle record);
// StartUpdate downloads + imports latestVer. Worker failures are logged under "tree".
class PassiveTreeUpdater : public DataUpdater<PassiveUpdatePhase, PassiveUpdateStatus> {
public:
	using Status = PassiveUpdateStatus;
	PassiveTreeUpdater() : DataUpdater("tree") {}

private:
	friend int RunPassiveTreeUpdateCli(const std::wstring& exeDir); // headless reuse

	void loadRecord() override;
	bool doCheck(std::string* err) override;     // worker
	bool doUpdate(std::string* err) override;    // worker
	void saveRecord();

	// worker-thread state after Init
	std::string currentVer_;            // bundled treeVersion ("3_28"); "" when absent
	std::string latestVer_;             // newest found, folder style ("3_29")
	std::string latestTag_;             // full GitHub tag for the download ("3.29.0")
};

// "pob-zh.exe --pt-update": headless check + download + import (ignores the
// daily throttle). Prints the outcome; 0 on success/up-to-date.
int RunPassiveTreeUpdateCli(const std::wstring& exeDir);
