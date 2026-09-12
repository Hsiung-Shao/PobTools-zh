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
	std::vector<SnapshotLine> lines; // sorted by key; EMPTY for a summary
	double totalChaos = 0.0;
	double divineRate = 0.0;         // chaos per divine AT SNAPSHOT TIME; 0 = unknown.
	                                 // Stored so history is converted at the rate
	                                 // that was true then, not today's.
	int unpricedKinds = 0;           // distinct keys no price was found for
	// An old snapshot is thinned to its totals (WarehouseHistory::Prune): the
	// lines go, lineCount remembers how many there were. A summary still draws
	// the curve and the timeline, but cannot be compared item by item. Absent
	// from files written before summaries existed = a full snapshot.
	bool summary = false;
	int lineCount = 0;               // meaningful only when summary
};

// What changed between two snapshots, joined on line key.
struct SnapshotDiffLine {
	std::string key, dispEn, icon;
	long long dCount = 0;
	double dChaos = 0.0;      // value change = dQtyChaos + dPriceChaos
	double dQtyChaos = 0.0;   // the count change, valued at the 'to' price (the
	                          // 'from' price for an item that is gone)
	double dPriceChaos = 0.0; // the market re-pricing the count held before --
	                          // including a quote that vanished (to-price 0)
	double each = 0.0;        // unit price at the 'to' side (from-side for removals)
	bool estimated = false;   // that price is the card floor, not a quote
};

struct SnapshotDiff {
	double dTotalChaos = 0.0;
	double hours = 0.0;              // wall time between the two
	double chaosPerHour = 0.0;       // 0 when hours == 0
	// dTotalChaos split by cause. qtyGain + qtyLoss is what the player did --
	// items farmed, items spent -- and priceMove is the market moving what was
	// already held. Mixing them made a divine-price dip read as "spending".
	double qtyGain = 0.0;            // >= 0
	double qtyLoss = 0.0;            // <= 0
	double priceMove = 0.0;
	double farmPerHour = 0.0;        // (qtyGain + qtyLoss) / hours; 0 when hours == 0
	// Either side is a summary: only dTotalChaos / hours / chaosPerHour are
	// known -- no lines, no split.
	bool summaryOnly = false;
	std::vector<SnapshotDiffLine> gained; // dChaos or dCount up, by |dChaos| desc
	std::vector<SnapshotDiffLine> lost;   // down, same order
};

SnapshotDiff DiffSnapshots(const Snapshot& from, const Snapshot& to);

// The history of one league of one game:
//   PobTools\warehouse\<poe1|poe2>_<league>.json
// Per league: a league ends and its snapshots are never compared with the next
// one's, and a small file keeps each read-modify-write (UI thread) cheap.
struct WarehouseHistory {
	std::vector<Snapshot> snaps;     // utc ascending
	long long sessionStartUtc = 0;   // utc of the snapshot marked "session start"; 0 = none

	// absent/corrupt -> empty, not an error
	bool Load(const std::wstring& exeDir, const std::string& game, const std::string& league);
	// atomic (unique .tmp -> MoveFileExW), compact JSON
	bool Save(const std::wstring& exeDir, const std::string& game,
	          const std::string& league) const;

	// Keeps the file bounded: the newest kFullKept snapshots and the session
	// start keep their lines, older ones become summaries; at most kMaxSnaps in
	// total (oldest dropped first). The session start is never removed. Call
	// before Save.
	static constexpr int kFullKept = 20;
	static constexpr int kMaxSnaps = 1000;
	void Prune();

	const Snapshot* FindByUtc(long long utc) const;

	static std::wstring PathOf(const std::wstring& exeDir, const std::string& game,
	                           const std::string& league);
	// A league's part of the file name: ASCII letters, digits and '-' kept, a
	// space becomes '_'. Anything else (or an over-long name) is replaced too,
	// and a hash of the real name is appended, so two leagues never share a file.
	static std::string LeagueFileStem(const std::string& league);

	// Read-modify-write against the league's file. Two panels can hold the same
	// history -- the standalone tool and the atlas planner's embed, in two
	// processes or two launcher tabs -- and a plain Save of a stale copy would
	// erase the other's snapshots. Both reload, apply, prune, save, and leave
	// *this equal to what was written (that league's history). Neither may be
	// called while a pointer into snaps is held: *this is replaced wholesale.
	bool AppendAndSave(const std::wstring& exeDir, const std::string& game,
	                   const std::string& league, const Snapshot& s);
	// False (and nothing written) when that snapshot no longer exists on disk,
	// or is a summary (nothing to diff against).
	bool SetSessionStartAndSave(const std::wstring& exeDir, const std::string& game,
	                            const std::string& league, long long utc);

	// One-time move from the single-file layout (PobTools\warehouse_<game>.json,
	// every league in one file) into per-league files. Merges into files that
	// already exist -- a snapshot's utc is its identity, never duplicated -- and
	// the session start goes with its league. On success the old file is renamed
	// to .migrated (kept, restorable by hand); any failure leaves it where it is
	// for the next try. Returns the snapshots moved, 0 when there was nothing to
	// do, -1 on failure.
	static int MigrateLegacy(const std::wstring& exeDir, const std::string& game);
};
