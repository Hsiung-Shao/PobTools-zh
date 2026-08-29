// 倉庫收益統計 — how stash contents get here, independent of who serves them.
//
// The tool is written against IStashProvider, not against an endpoint. Today the
// only implementation rides the legacy character-window API with a POESESSID
// cookie (a TEST channel: the user pastes their own session id); when the
// official OAuth application is approved, a second implementation talks to
// api.pathofexile.com with an access token and this file is the only boundary
// that has to hold -- everything above it (service, snapshots, UI) sees the same
// StashItemRaw either way.
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// One stash tab, as listed by the API. `id` is the stable identity a snapshot
// remembers -- indexes renumber whenever the user drags a tab around.
struct StashTabInfo {
	int index = 0;
	std::string id;      // GGG's opaque tab id (hex string)
	std::string name;    // player-given, any language
	std::string type;    // "CurrencyStash" / "PremiumStash" / ...
	unsigned colour = 0; // 0xRRGGBB from the tab's colour swatch
};

// The minimum an item must carry to be counted and priced. Deliberately not the
// whole GGG item JSON: a snapshot stores aggregates, so anything not needed for
// identity, count or price-key building is dropped at the parse boundary.
struct StashItemRaw {
	std::string name;      // unique/rare title line; empty for normal items
	std::string typeLine;  // full display type
	std::string baseType;  // base item name (absent in old payloads; falls back to typeLine)
	std::string icon;      // full poecdn URL as the API sends it
	int frameType = 0;     // 0 normal, 3 unique, 4 gem, 5 currency, 6 div card
	long long stackSize = 1;
	int ilvl = 0;
	int gemLevel = 0;      // properties[] "Level"
	int gemQuality = 0;    // properties[] "Quality"
	int mapTier = 0;       // properties[] "Map Tier"
	int links = 0;         // largest socket-link group
	bool corrupted = false;
	int tabIndex = -1;     // which requested tab it came from
};

struct StashAuth {
	std::string accountName; // required by the sessid channel; the user types it
	std::string secret;      // POESESSID today, an OAuth access token later.
	                         // Lives in memory only -- persistence is
	                         // warehouse_state's DPAPI business, and this string
	                         // must never appear in a message or a log line.
};

// Why a call failed, coarsely -- the UI answers each of these differently
// (re-enter the session id / wait / check the network / report a bug).
enum class StashError {
	None,
	Auth,        // 401/403: session expired or never valid
	Blocked,     // 403 that looks like an edge-protection page, not GGG's own answer
	RateLimited, // 429: the throttle will hold the next call back
	Network,     // transport failure
	Parse,       // 200 but the body did not look like stash JSON
};

class IStashProvider {
public:
	virtual ~IStashProvider() = default;

	// All synchronous, worker-thread only. Each call throttles itself (minimum
	// spacing + whatever the server's rate-limit headers demanded); the caller
	// just calls in sequence.
	virtual bool Verify(std::string* err, StashError* kind,
	                    const std::atomic<bool>* cancel) = 0;
	virtual bool ListTabs(const std::string& league, std::vector<StashTabInfo>& out,
	                      std::string* err, StashError* kind,
	                      const std::atomic<bool>* cancel) = 0;
	virtual bool FetchTab(const std::string& league, int tabIndex,
	                      std::vector<StashItemRaw>& out, std::string* err,
	                      StashError* kind, const std::atomic<bool>* cancel) = 0;
};

std::unique_ptr<IStashProvider> CreateSessidStashProvider(const StashAuth& auth);

// The parse half of FetchTab/ListTabs, split out so the self-test can feed it
// fixtures without a socket. Either output may be null when the caller only
// wants the other; `tabIndex` is stamped into every parsed item.
bool ParseStashTabJson(const std::string& body, int tabIndex,
                       std::vector<StashItemRaw>* items,
                       std::vector<StashTabInfo>* tabs, std::string* err);

// Conservative client-side pacing: a fixed minimum spacing between requests,
// pushed further back by what the server's rate-limit headers report and by any
// 429's Retry-After. Pure arithmetic over fed-in state -- the self-test drives
// it with synthetic headers and asserts on NextDelayMs.
class SimpleThrottle {
public:
	void SetMinIntervalMs(int ms) { minMs_ = ms; }

	// Record that a request was just issued at `nowMs` (monotonic).
	void OnRequest(long long nowMs) { lastReqMs_ = nowMs; }

	// Feed every response's status and collected headers (lower-cased keys:
	// "x-rate-limit-rules", "x-rate-limit-<rule>", "x-rate-limit-<rule>-state",
	// "retry-after").
	void OnResponse(long long nowMs, int status,
	                const std::unordered_map<std::string, std::string>& headers);

	// How long the next request must still wait from `nowMs`. 0 = go now.
	int NextDelayMs(long long nowMs) const;

private:
	long long lastReqMs_ = 0;
	long long blockedUntilMs_ = 0;
	int minMs_ = 1000;
};
