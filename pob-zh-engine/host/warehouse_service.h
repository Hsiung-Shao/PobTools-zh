// 倉庫收益統計 — the worker behind the panel.
//
// Same shape as AtlasUpdater: one worker thread, a command queue in, a Status
// snapshot out, polled by the UI every frame. All network -- stash fetches and
// ninja pricing -- happens here; the panel never blocks.
#pragma once

#include "warehouse_ninja.h"
#include "warehouse_provider.h"
#include "warehouse_snapshot.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

enum class WarehousePhase {
	Idle,
	Verifying,
	ListingTabs,
	FetchingTabs,
	Pricing,
	Done,   // a snapshot is ready; TakeSnapshot + AckDone
	Error,  // message says what; AckDone returns to Idle
};

class WarehouseService {
public:
	void Init(const std::wstring& exeDir); // starts the worker; touches no network
	void Shutdown();                       // cancels any transfer and joins

	// Main thread. Copied under a lock; the worker takes its own copy per job, so
	// a mid-job change applies from the next command on.
	void SetAuth(const StashAuth& a);

	// Main thread. Drops the held credential (overwriting its bytes) and the
	// session verdict with it -- the 清除 button. A job already running keeps the
	// copy it took; the next one has nothing to send.
	void ForgetAuth();

	// Main thread. "poe1" / "poe2": which realm and which price table later
	// requests use (captured into each command when it is queued). Clears the
	// per-realm status (tab/league lists, session verdict).
	void SetGame(const std::string& game);

	void RequestVerify();
	void RequestLeagues();                        // poe.ninja league list
	void RequestTabList(const std::string& league);
	void RequestSnapshot(const std::string& league,
	                     const std::vector<std::string>& tabIds, bool withPricing);

	struct Status {
		WarehousePhase phase = WarehousePhase::Idle;
		std::string message;      // UTF-8; built from fixed text + counts + tab
		                          // names only -- never from auth fields
		int tabsDone = 0, tabsTotal = 0;
		bool authOk = false;      // last Verify succeeded
		bool authFailed = false;  // last call died on StashError::Auth -> re-enter id
		bool blocked = false;     // last call died on StashError::Blocked
		std::vector<StashTabInfo> tabs;
		bool tabsReady = false;
		std::vector<std::string> leagues;
		bool leaguesReady = false;
		bool snapshotReady = false;
	};
	Status Poll();                    // main thread, each frame
	bool TakeSnapshot(Snapshot* out); // valid once snapshotReady; clears the flag
	void AckDone();                   // after Done/Error was shown: back to Idle

private:
	struct Cmd {
		enum class Kind { Verify, Leagues, ListTabs, Snapshot } kind = Kind::Verify;
		std::string game = "poe1"; // captured when queued
		std::string league;
		std::vector<std::string> tabIds;
		bool withPricing = true;
	};

	void workerLoop();
	void doVerify(const StashAuth& auth, const Cmd& cmd);
	void doLeagues(const Cmd& cmd);
	void doListTabs(const StashAuth& auth, const Cmd& cmd);
	void doSnapshot(const StashAuth& auth, const Cmd& cmd);
	void setPhase(WarehousePhase p, const std::string& msg);
	StashAuth authCopy();
	void noteError(StashError kind, const std::string& err);

	// The shared request guard (warehouse_state.h). guardRefuses: a pause the
	// server asked for has not passed -> Error, and nothing is sent. noteBackoff:
	// carries the provider's pending pause into the guard when a job ends.
	bool guardRefuses();
	void noteBackoff(const IStashProvider& p);
	struct BackoffCarry { // noteBackoff at scope exit, whichever way a job ends
		WarehouseService* svc;
		const IStashProvider* provider;
		~BackoffCarry() { svc->noteBackoff(*provider); }
	};

	std::wstring exeDir_;
	std::thread worker_;
	std::atomic<bool> stop_{ false };

	std::mutex cmdMx_;
	std::condition_variable cmdCv_;
	std::deque<Cmd> cmdQ_;
	std::string game_ = "poe1"; // guarded by cmdMx_

	std::mutex authMx_;
	StashAuth auth_;

	std::mutex stMx_;
	Status st_;
	Snapshot pending_;

	NinjaPriceSource ninja_; // worker-thread only after Init
};

// Pure helpers, exposed for the self-test.
//
// Folds raw items into aggregated, key-sorted lines (counts only; no prices).
// Items BuildPriceKey cannot key are aggregated under "other|<typeLine>" so the
// player still sees them counted -- they simply never price.
void WarehouseAggregateItems(const std::vector<StashItemRaw>& items, Snapshot& snap);

// Applies prices to aggregated lines and recomputes the totals. `lookup`
// returns false for unknown keys; low-confidence prices count as unknown.
void WarehousePriceSnapshot(Snapshot& snap,
                            const std::function<bool(const std::string&, NinjaPrice*)>& lookup,
                            double divineRate);
