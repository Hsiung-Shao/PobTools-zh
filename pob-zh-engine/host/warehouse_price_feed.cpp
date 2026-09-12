#include "warehouse_price_feed.h"

#include <vector>

void NinjaPriceFeed::Init(const std::wstring& exeDir)
{
	exeDir_ = exeDir;
	src_.Init(exeDir);
	stop_ = false;
	worker_ = std::thread([this] { workerLoop(); });
}

void NinjaPriceFeed::Shutdown()
{
	{
		std::lock_guard<std::mutex> lk(mx_);
		stop_ = true;
	}
	cv_.notify_all();
	if (worker_.joinable()) worker_.join();
}

void NinjaPriceFeed::Request(const std::string& game, const std::string& league, bool force)
{
	{
		std::lock_guard<std::mutex> lk(mx_);
		game_ = game == "poe2" ? "poe2" : "poe1";
		league_ = league;
		force_ = force_ || force; // a coalesced forced request stays forced
		pending_ = true;
		st_.busy = true;
	}
	cv_.notify_all();
}

NinjaPriceFeed::Status NinjaPriceFeed::Poll()
{
	std::lock_guard<std::mutex> lk(mx_);
	return st_;
}

void NinjaPriceFeed::workerLoop()
{
	for (;;) {
		std::string game, league;
		bool force = false;
		{
			std::unique_lock<std::mutex> lk(mx_);
			cv_.wait(lk, [this] { return stop_.load() || pending_; });
			if (stop_) return;
			game = game_;
			league = league_;
			force = force_;
			pending_ = false;
			force_ = false;
		}

		std::string err;
		bool ok = false;
		std::shared_ptr<const PriceTable> table;
		// Nothing thrown here may cross the thread boundary: an escaped exception
		// terminates the whole process (see error_worker_thread_json_null_crash).
		try {
			if (league.empty()) {
				std::vector<std::string> leagues;
				if (FetchNinjaLeagues(game, leagues, &err, &stop_) && !leagues.empty())
					league = leagues[0];
			}
			ok = !league.empty() && src_.Refresh(game, league, force, &err, &stop_);
			if (ok) table = std::make_shared<const PriceTable>(src_.Prices());
		} catch (const std::exception& e) {
			ok = false;
			err = std::string(u8"內部錯誤: ") + e.what();
		} catch (...) {
			ok = false;
			err = u8"內部錯誤（未知例外）";
		}

		std::lock_guard<std::mutex> lk(mx_);
		st_.busy = pending_; // a newer request may already be waiting
		if (ok) {
			st_.error.clear();
			st_.league = league;
			st_.fetchedUtc = src_.FetchedUtc();
			st_.divineRate = src_.DivineRate();
			st_.prices = std::move(table);
		} else {
			st_.error = err.empty() ? std::string(u8"無法取得價格") : err;
		}
	}
}
