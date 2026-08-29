// 倉庫收益統計 — prices from poe.ninja's 2026 economy API.
//
// The old /api/data/* endpoints 404'd in 2026; the current surface is
//   /poe1/api/economy/exchange/current/overview?league=<L>&type=<T>
//   /poe1/api/economy/stash/current/item/overview?league=<L>&type=<T>
//   /poe1/api/economy/leagues
// type=All returns nothing, so exchange types are fetched one by one and merged.
// International realm only -- which is exactly the realm this tool targets.
//
// House rules carried over from poe-market-zh/bg/ninja.js (hard-won):
//   * item-overview lines with count < 5 are LOW CONFIDENCE -- poe.ninja itself
//     hides them; without the filter the top of every list is 1-3-listing price
//     manipulation. They are kept but flagged, and the pricer skips them.
//   * results are cached 15 minutes (PobTools\cache\ninja\<league>.json); the
//     site updates ~15-minutely and asks not to be polled faster.
//   * the divine rate comes from the Divine Orb line and is only believed when
//     it is at least 30 chaos.
#pragma once

#include <atomic>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

struct NinjaPrice {
	double chaos = 0.0;
	int listingCount = 0;      // 0 = the endpoint had no count (exchange lines)
	bool lowConfidence = false;
};

class NinjaPriceSource {
public:
	void Init(const std::wstring& exeDir);

	// Worker thread. Loads the disk cache and, when it is stale (or force),
	// refetches every type and rewrites it. A type that fails to fetch or parse
	// is skipped; only "nothing at all could be fetched" is an error.
	bool Refresh(const std::string& league, bool force, std::string* err,
	             const std::atomic<bool>* cancel);

	// After Refresh: price by warehouse_pricing key. False = no price known.
	bool PriceOf(const std::string& key, NinjaPrice* out) const;

	// Chaos per divine; 0 = unknown or implausible (< 30c).
	double DivineRate() const { return divineRate_; }
	long long FetchedUtc() const { return fetchedUtc_; }

	// Pure parsers, exposed for the self-test. Output pairs are (price-key, price).
	//
	// keyPrefix: the exchange endpoint serves more than currency -- divination
	// cards trade there too (type=DivinationCard; the item overview 404s for
	// them) -- and each family keys under its own namespace ("card|").
	static bool ParseExchangeOverview(const std::string& body,
	                                  std::vector<std::pair<std::string, NinjaPrice>>* out,
	                                  std::string* err,
	                                  const char* keyPrefix = "currency|");
	static bool ParseItemOverview(const std::string& body, const std::string& type,
	                              std::vector<std::pair<std::string, NinjaPrice>>* out,
	                              std::string* err);
	// The PoE1-only stash currency overview (legacy currencyoverview shape:
	// lines[].currencyTypeName / chaosEquivalent / receive.count). Fetched as a
	// gap-filler: the exchange API covers only what the bulk market actively
	// trades, and everything it misses (low-volume currency, splinters...)
	// would otherwise show as 未估價.
	static bool ParseCurrencyOverview(const std::string& body,
	                                  std::vector<std::pair<std::string, NinjaPrice>>* out,
	                                  std::string* err);

	// The 15-minute freshness rule, pure so the self-test can pin it.
	static bool CacheFresh(long long fetchedUtc, long long nowUtc)
	{
		return fetchedUtc > 0 && nowUtc >= fetchedUtc && nowUtc - fetchedUtc < 15 * 60;
	}

private:
	bool loadCache(const std::string& league);
	void saveCache(const std::string& league) const;

	std::wstring exeDir_;
	std::string league_;
	std::unordered_map<std::string, NinjaPrice> prices_;
	double divineRate_ = 0.0;
	long long fetchedUtc_ = 0;
};

// GET /poe1/api/economy/leagues -> the league ids, current challenge league
// first. Worker thread.
bool FetchNinjaLeagues(std::vector<std::string>& out, std::string* err,
                       const std::atomic<bool>* cancel);
