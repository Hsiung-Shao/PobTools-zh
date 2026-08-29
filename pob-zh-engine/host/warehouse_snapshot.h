// 倉庫收益統計 — snapshots, their history file, and the arithmetic between them.
//
// A snapshot stores AGGREGATES (price-key -> count -> value), never the raw item
// JSON: the raw payload for one quad tab runs to megabytes and answers no
// question this tool asks. Everything in this header is pure data + pure
// functions -- the self-test drives all of it without a socket or a window.
#pragma once

#include <string>
#include <vector>

// One aggregated line of a snapshot. `key` is the identity lines are joined on
// across snapshots (Phase A: the English display name; Phase B: the price-key).
struct SnapshotLine {
	std::string key;
	std::string dispEn;   // English display name; 繁中 is looked up at draw time
	std::string icon;     // poecdn art path ("Art/2DItems/..."), for IconManager
	long long count = 0;
	double chaosEach = 0.0;
	double chaosTotal = 0.0;
	bool priced = false;
	// True when chaosEach is a conservative floor (sub-1c cards nobody bulk
	// lists), not a market quote. Drawn as "~x" so an estimate can never pass
	// for a real price.
	bool estimated = false;
};

struct Snapshot {
	long long utc = 0;               // unix seconds
	std::string league;
	std::vector<std::string> tabIds; // which tabs it covered (ids survive reorders)
	std::vector<SnapshotLine> lines; // sorted by key
	double totalChaos = 0.0;
	double divineRate = 0.0;         // chaos per divine AT SNAPSHOT TIME; 0 = unknown.
	                                 // Stored so history is converted at the rate
	                                 // that was true then, not today's.
	int unpricedKinds = 0;           // distinct keys no price was found for
};

// What changed between two snapshots, joined on line key.
struct SnapshotDiffLine {
	std::string key, dispEn, icon;
	long long dCount = 0;
	double dChaos = 0.0;
	double each = 0.0;      // unit price at the 'to' side (from-side for removals)
	bool estimated = false; // that price is the card floor, not a quote
};

struct SnapshotDiff {
	double dTotalChaos = 0.0;
	double hours = 0.0;              // wall time between the two
	double chaosPerHour = 0.0;       // 0 when hours == 0
	std::vector<SnapshotDiffLine> gained; // dChaos or dCount up, by |dChaos| desc
	std::vector<SnapshotDiffLine> lost;   // down, same order
};

SnapshotDiff DiffSnapshots(const Snapshot& from, const Snapshot& to);

// The whole history: PobTools\warehouse_poe1.json.
struct WarehouseHistory {
	std::vector<Snapshot> snaps;     // utc ascending
	long long sessionStartUtc = 0;   // utc of the snapshot marked "session start"; 0 = none

	bool Load(const std::wstring& exeDir);        // absent/corrupt -> empty, not an error
	bool Save(const std::wstring& exeDir) const;  // atomic (.tmp -> MoveFileExW)

	// Keeps the file bounded: beyond 48h old, one snapshot per hour (the last of
	// each hour); at most 200 total (oldest dropped first). The session-start
	// snapshot is never removed. Call before Save.
	void Prune(long long nowUtc);

	const Snapshot* FindByUtc(long long utc) const;
};
