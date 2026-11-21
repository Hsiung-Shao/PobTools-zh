// 王戰收益 — what a boss kill is worth: sum(drop rate x price) minus the entry cost.
// Pure data: no ImGui, no network. Drop rates ship in Data/boss_drops_poe1.json (community numbers,
// see its "source"); prices are injected by the caller as a lookup on warehouse_pricing keys.
#pragma once

#include "warehouse_ninja.h" // NinjaPrice

#include <functional>
#include <map>
#include <string>
#include <vector>

struct BossDrop {
	std::string priceKey, en;
	double ratePct = 0.0;  // 0 < ratePct <= 100; 0 = unknown (listed, not summed until the user enters one)
	std::string rateText;  // the source's own wording when it is not a plain number ("~5", "<0.5")
	std::string note;      // drop group ("Guaranteed Unique")
};
struct BossTicket {
	std::string priceKey, en;
	double qty = 1.0;
};
struct BossVariant {
	std::string name;
	bool uber = false;
	std::vector<BossTicket> tickets; // empty = no entry cost: revenue only
	std::vector<BossDrop> drops;
};
struct Boss {
	std::string key, en, zh; // zh empty = no official name: show en
	std::vector<BossVariant> variants;
};
struct BossDb {
	std::string source;
	std::vector<Boss> bosses;
};

// False + err on malformed JSON, a missing/mistyped field, a rate outside (0,100], a qty <= 0 or a
// duplicate boss key. db is left empty on failure.
bool LoadBossDb(const std::string& json, BossDb* db, std::string* err);

// "<boss.key>|<variant.name>": the key of everything stored per variant.
std::string BossVariantId(const Boss& b, const BossVariant& v);

// What the user typed over the shipped numbers.
struct BossOverrides {
	std::map<std::string, double> price;    // priceKey -> chaos each (one item, one price, every boss)
	std::map<std::string, double> rate;     // "<variantId>|<drop.en>" -> percent
	std::map<std::string, int> killSeconds; // variantId -> seconds per kill
	// The boss list's sort, most significant first: column + 1, negative = descending. A view
	// preference, kept here because this is the panel's one saved file.
	std::vector<int> sort;
};
constexpr int kBossSortCols = 7; // columns of the boss list
std::string SerializeBossOverrides(const BossOverrides& o);
bool ParseBossOverrides(const std::string& json, BossOverrides* o, std::string* err);
// 「重置此王修改」: drops this variant's rates, kill time and the prices of its drops and tickets.
void ResetBossVariantOverrides(BossOverrides* o, const Boss& b, const BossVariant& v);

enum class BossPriceSrc { Market, Manual, None };

struct BossLine {
	std::string priceKey, en;
	double ratePct = 0.0;  // drops: the rate used.  tickets: unused
	double qty = 1.0;      // tickets: copies needed. drops: unused
	bool rateManual = false;
	double unit = 0.0;     // chaos each; 0 when src == None
	double value = 0.0;    // drops: ratePct/100 * unit.  tickets: qty * unit
	BossPriceSrc src = BossPriceSrc::None;
};

struct BossResult {
	std::vector<BossLine> drops, tickets; // same order as the variant
	double revenue = 0.0;     // priced drops only
	double ticketCost = 0.0;  // priced tickets only
	double profit = 0.0;      // revenue - ticketCost
	bool hasTicket = false;
	bool ticketIncomplete = false; // a ticket has no price: profit is overstated, ROI withheld
	int unpricedDrops = 0;
	bool hasRoi = false;      // has tickets, all priced, cost > 0
	double roiPct = 0.0;      // profit / ticketCost * 100
	bool hasPerHour = false;  // a kill time > 0 was given
	double profitPerHour = 0.0; // profit * 3600 / killSeconds
};

// A market price counts only when the lookup returns true and it is not lowConfidence (the stash
// pricer's rule); anything else is unpriced -- left out of the sums and counted, never treated as 0c.
// A manual price wins over the market and prices an otherwise unpriced line.
BossResult ComputeBossVariant(const Boss& b, const BossVariant& v,
                              const std::function<bool(const std::string&, NinjaPrice*)>& lookup,
                              const BossOverrides& o);
