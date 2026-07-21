// PobTools economy service.
//
// Phase 5a: a static, offline price source loaded from bundled JSON
// (Data/economy_poe1.json: English name -> chaos value) plus value->tier thresholds
// (Data/economy_tiers_poe1.json). Provides per-item valuation and a "suggested tier"
// for the tier-list cards. Everything is display-only — accepting a suggestion goes
// through the existing MoveItemToTier path, so output stays English.
//
// Phase 5b will add a live provider behind the same surface (worker thread + cache),
// with this static data as the permanent offline fallback.
#pragma once

#include "filter_tierlist.h"   // TierCategory / TierListIndex
#include <string>
#include <vector>
#include <unordered_map>

struct PriceInfo {
	double chaos = 0.0;
	bool known = false;
};

class EconomyService {
public:
	// Load bundled static prices + thresholds. enabled=false keeps available() false
	// (the toggle in Settings), but the data is still loaded so enabling is instant.
	void Init(const std::wstring& exeDir, const std::string& league, bool enabled);
	void SetEnabled(bool en) { enabled_ = en; }
	void SetLeague(const std::string& l) { league_ = l; }

	bool available() const { return enabled_ && !prices_.empty(); }
	const std::string& league() const { return league_; }

	// Chaos value of an English item name (known=false if not priced).
	PriceInfo PriceOf(const std::string& en) const;

	// Suggested draggable tier (a FilterFile::blocks index) for `en` within `cat`,
	// from its value vs the thresholds. Returns -1 if unknown / unavailable / no fit.
	int SuggestTier(const std::string& en, const TierCategory& cat, const TierListIndex& index) const;

private:
	bool enabled_ = false;
	std::string league_;
	std::unordered_map<std::string, double> prices_;  // en name -> chaos value
	std::vector<double> thresholds_;                  // descending chaos cut-offs
};
