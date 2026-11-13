// 倉庫收益統計 — how stash contents get here, independent of who serves them.
//
// The tool is written against IStashProvider, not an endpoint. Today's only implementation rides
// the legacy character-window API with a POESESSID cookie (a TEST channel); an OAuth one can slot
// in later, and everything above this boundary sees the same StashItemRaw.
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

// The minimum an item must carry to be counted and priced; the rest of GGG's item JSON is dropped at the parse boundary.
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
	std::string secret;      // POESESSID today. Memory only (persistence is warehouse_state's DPAPI);
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
	Forbidden,   // 403 HTML that is neither Cloudflare nor a session answer: the
	             // site refusing this REQUEST (e.g. a realm the endpoint lacks)
};

class IStashProvider {
public:
	virtual ~IStashProvider() = default;

	// All synchronous, worker-thread only. Each call throttles itself (minimum spacing + the
	// server's rate-limit headers); the caller just calls in sequence.
	// charLeagues (optional): leagues this account has characters in on the provider's realm.
	virtual bool Verify(std::string* err, StashError* kind,
	                    const std::atomic<bool>* cancel,
	                    std::vector<std::string>* charLeagues = nullptr) = 0;
	virtual bool ListTabs(const std::string& league, std::vector<StashTabInfo>& out,
	                      std::string* err, StashError* kind,
	                      const std::atomic<bool>* cancel) = 0;
	virtual bool FetchTab(const std::string& league, int tabIndex,
	                      std::vector<StashItemRaw>& out, std::string* err,
	                      StashError* kind, const std::atomic<bool>* cancel) = 0;

	// How long the server still wants this account left alone (429 Retry-After, bucket penalty), ms
	// from now; 0 = none. A provider lives for one job, so the caller carries this past it (warehouse_state's request guard).
	virtual int RemainingBackoffMs() const { return 0; }
};

// `realm` is the legacy endpoints' realm parameter: "pc" (PoE1, default) or "poe2"
// (whether the legacy path serves PoE2 stashes: --warehouse-probe poe2).
std::unique_ptr<IStashProvider> CreateSessidStashProvider(const StashAuth& auth,
                                                          const std::string& realm = "pc");

// The sessid channel's reading of an HTTP 403 body, split out so the self-test
// can pin it with real-shaped pages. *err gets a fixed message plus at most the
// page <title> -- never other body text.
StashError ClassifyStash403(const std::string& body, std::string* err);

// GGG's JSON error body ({"error":{"code":N,"message":"..."}}) as one short
// line, "message (code N)"; empty when the body is not that shape. Fixed
// server text -- safe to show and to log.
std::string StashApiErrorText(const std::string& body);

// The parse half of FetchTab/ListTabs, split out so the self-test can feed it
// fixtures without a socket. Either output may be null when the caller only
// wants the other; `tabIndex` is stamped into every parsed item.
bool ParseStashTabJson(const std::string& body, int tabIndex,
                       std::vector<StashItemRaw>* items,
                       std::vector<StashTabInfo>* tabs, std::string* err);

// The sessid channel's floor between two requests. GGG's account bucket is one pool for the whole
// account (~45 per 60 s, 30 at times); at 2 s a full-stash snapshot is spread over ~2N seconds.
constexpr int kStashMinSpacingMs = 2000;

// Conservative client-side pacing: fixed minimum spacing, pushed back by the rate-limit headers
// and any 429 Retry-After. Pure arithmetic over fed-in state (self-test asserts on NextDelayMs).
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

	// Until when the server asked to be left alone (monotonic ms; 0 = never).
	long long BlockedUntilMs() const { return blockedUntilMs_; }

private:
	long long lastReqMs_ = 0;
	long long blockedUntilMs_ = 0;
	int minMs_ = kStashMinSpacingMs;
};
