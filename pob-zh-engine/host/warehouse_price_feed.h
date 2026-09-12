// 倉庫收益統計 — poe.ninja prices for views that are not a snapshot.
//
// The snapshot worker (WarehouseService) prices inside its own job. The atlas
// planner's cost card needs the same table outside any job, so this is a small
// worker of its own around NinjaPriceSource. Both share the same 15-minute disk
// cache (PobTools\cache\ninja\<game>_<league>.json): opening the card right
// after a snapshot costs no request at all.
#pragma once

#include "warehouse_ninja.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

class NinjaPriceFeed {
public:
	using PriceTable = std::unordered_map<std::string, NinjaPrice>;

	void Init(const std::wstring& exeDir); // starts the worker; touches no network
	void Shutdown();                       // cancels any transfer and joins; idempotent

	// Main thread. league empty = poe.ninja's current league. Requests coalesce:
	// only the newest one is served.
	void Request(const std::string& game, const std::string& league, bool force);

	struct Status {
		bool busy = false;
		std::string error;         // UTF-8, last failure; cleared by a success
		std::string league;        // the league the table is for
		long long fetchedUtc = 0;
		double divineRate = 0.0;   // chaos per divine; 0 = unknown
		std::shared_ptr<const PriceTable> prices; // null until the first success
	};
	Status Poll(); // main thread, each frame

private:
	void workerLoop();

	std::wstring exeDir_;
	std::thread worker_;
	std::atomic<bool> stop_{ false };

	std::mutex mx_;
	std::condition_variable cv_;
	bool pending_ = false;
	std::string game_ = "poe1", league_;
	bool force_ = false;
	Status st_;

	NinjaPriceSource src_; // worker thread only after Init
};
