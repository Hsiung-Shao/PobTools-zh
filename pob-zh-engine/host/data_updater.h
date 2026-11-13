// Shared shell of the two data updaters (AtlasUpdater, PassiveTreeUpdater): one
// worker thread fed by a command queue, a mutex-guarded Status the main thread
// snapshots with Poll() each frame, and a once-per-day check throttle.
// Subclasses supply the record load, doCheck and doUpdate.
#pragma once

#include "error_log.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

long long DataUpdaterNowFiletime(); // GetSystemTimeAsFileTime as 100ns units (data_updater.cpp)

// Phase needs Idle and Error; Status needs phase, message and reloadPending.
template <class Phase, class Status>
class DataUpdater {
public:
	// Loads the subclass record, then starts the worker.
	void Init(const std::wstring& exeDir)
	{
		exeDir_ = exeDir;
		loadRecord();
		worker_ = std::thread(&DataUpdater::workerLoop, this);
	}

	// Cancels any transfer and joins the worker.
	void Shutdown()
	{
		if (worker_.joinable()) {
			{
				std::lock_guard<std::mutex> lk(cmdMx_);
				stop_.store(true);
			}
			cmdCv_.notify_all();
			worker_.join();
		}
	}

	// Main thread. Queues a version check; without force it is throttled to once per day.
	void RequestCheck(bool force)
	{
		if (!worker_.joinable()) return;
		static const long long kDay = 24ll * 3600 * 10'000'000; // FILETIME is 100ns units
		if (!force && lastCheckUtc_ > 0 && DataUpdaterNowFiletime() - lastCheckUtc_ < kDay)
			return; // throttled: checked within the last day
		queue(Cmd::Check);
	}

	// Main thread. Queues a download + import of the version the last check found.
	void StartUpdate()
	{
		if (!worker_.joinable()) return;
		queue(Cmd::Update);
	}

	Status Poll() // main thread, each frame: snapshot
	{
		std::lock_guard<std::mutex> lk(stMx_);
		return st_;
	}

	void AckReload() // main thread: after handling the reload
	{
		std::lock_guard<std::mutex> lk(stMx_);
		st_.reloadPending = false;
		ackExtra(st_);
		st_.phase = Phase::Idle;
		st_.message.clear();
	}

protected:
	// failLogTag: PobLog feature tag for worker-side check/update failures; nullptr = no log line.
	DataUpdater(const char* failLogTag = nullptr) : failLogTag_(failLogTag) {}
	~DataUpdater() = default;

	virtual void loadRecord() = 0;               // Init, before the worker starts
	virtual bool doCheck(std::string* err) = 0;  // worker
	virtual bool doUpdate(std::string* err) = 0; // worker
	virtual void ackExtra(Status&) {}            // AckReload, under stMx_: subclass-only flags

	void setPhase(Phase p, const std::string& msg)
	{
		std::lock_guard<std::mutex> lk(stMx_);
		st_.phase = p;
		st_.message = msg;
	}

	std::wstring exeDir_;
	std::thread worker_;
	std::atomic<bool> stop_{ false };

	std::mutex stMx_;
	Status st_;

	std::atomic<long long> lastCheckUtc_{ 0 }; // FILETIME (100ns units)

private:
	enum class Cmd { Check, Update };

	void queue(Cmd c)
	{
		{
			std::lock_guard<std::mutex> lk(cmdMx_);
			cmdQ_.push_back(c);
		}
		cmdCv_.notify_one();
	}

	void workerLoop()
	{
		for (;;) {
			Cmd cmd;
			{
				std::unique_lock<std::mutex> lk(cmdMx_);
				cmdCv_.wait(lk, [&] { return stop_.load() || !cmdQ_.empty(); });
				if (stop_.load()) break;
				cmd = cmdQ_.front();
				cmdQ_.pop_front();
			}
			std::string err;
			if (cmd == Cmd::Check) {
				if (!doCheck(&err)) {
					// Quiet on screen (nobody asked); retried later because lastCheckUtc
					// is only persisted on success.
					if (failLogTag_) PobLog::Error(failLogTag_, "check failed: " + err);
					setPhase(Phase::Idle, "");
				}
			} else {
				if (!doUpdate(&err)) {
					if (failLogTag_) PobLog::Error(failLogTag_, "update failed: " + err);
					setPhase(Phase::Error, err);
				}
			}
		}
	}

	const char* failLogTag_;

	std::mutex cmdMx_;
	std::condition_variable cmdCv_;
	std::deque<Cmd> cmdQ_;
};
