// 輿圖策略 × 倉庫收益 — what one map of a project costs to run.
//
// A project's map device holds up to five scarabs / Vaal fragments (the list
// repeats an entry once per copy placed); each is priced by its English name on
// poe.ninja's exchange table, the same key the stash pricer uses. Regular maps
// have no poe.ninja price any more, so the map itself is a price the user types.
//
// Pure data: no ImGui, no ScarabDb, no network -- the caller resolves slots to
// MapCostInput and injects the price lookup, so the self-test drives it directly.
#pragma once

#include "atlas_persist.h"      // AtlasProfitRecord
#include "warehouse_ninja.h"    // NinjaPrice
#include "warehouse_snapshot.h" // Snapshot, DiffSnapshots

#include <functional>
#include <map>
#include <string>
#include <vector>

// One map-device slot, resolved by the caller (ScarabDb::ById).
struct MapCostInput {
	std::string id;          // Metadata id: the aggregation key
	std::string en, zh, art;
	bool tradable = true;    // ScarabDef::stash -- false = on no stash or trade site
};

struct MapCostLine {
	std::string id, en, zh, art;
	int qty = 0;             // copies placed in the device
	double chaosEach = 0.0;  // the unit cost used: the recorded/typed one, else market
	double chaosTotal = 0.0;
	double marketEach = 0.0; // live poe.ninja unit price, for reference (0 = none)
	bool priced = false;
	bool fromBasis = false;  // chaosEach is the project's recorded/typed price
	bool tradable = true;
};

struct MapCostSummary {
	std::vector<MapCostLine> lines; // placement order, one line per distinct item
	double totalChaos = 0.0;        // priced lines + the manual map price
	int unpricedKinds = 0;          // tradable, but no confident price
	int untradableKinds = 0;        // cannot be bought at all
	double mapChaos = 0.0;
	bool mapIncluded = false;       // a manual map price was given
};

// Low-confidence prices (fewer than 5 listings) count as unpriced, exactly as
// the stash pricer treats them. manualMapChaos <= 0 leaves the map out.
//
// basis (optional): what the player actually PAID, item id -> chaos per unit.
// One bulk purchase records the market once; batch buyers type what each batch
// cost. A basis price wins over the market (and makes even an untradable item
// count); items without one follow the live market.
MapCostSummary ComputeMapCost(const std::vector<MapCostInput>& slots, double manualMapChaos,
                              const std::function<bool(const std::string&, NinjaPrice*)>& lookup,
                              const std::map<std::string, double>* basis = nullptr);

// Binds the stash history's stretch from -> to to a project (收益紀錄): the
// value change split by cause (DiffSnapshots) and the ten biggest gains by the
// value of their count change. zhOf names each item for display; the name is
// stored, so whoever imports the share code needs no dictionary. costPerMap is
// the cost card's total at binding time (0 = unknown). Money is rounded to
// 0.01 chaos -- it travels in a share code. Empty when either side is a
// summary or `to` is not after `from`.
AtlasProfitRecord MakeProfitRecord(const Snapshot& from, const Snapshot& to,
                                   const std::function<std::string(const std::string&)>& zhOf,
                                   double costPerMap);
