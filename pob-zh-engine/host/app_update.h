// PobTools application self-updater.
//
// Checks GitHub Hsiung-Shao/PobTools-zh releases/latest, then applies the
// two-tier update policy (version convention, chosen by the maintainer):
//   patch bump  (0.1.0 -> 0.1.x)  = translation-data-only release: the
//       Translations zip is downloaded, sha256-verified against the GitHub
//       asset digest and applied to Data\ silently (engine picks it up on
//       next start / F3 reload);
//   minor/major bump               = app release: the launcher shows an
//       update prompt; on click the full zip is downloaded, verified,
//       extracted to a staging dir, and the main thread swaps the install
//       (rename-trick for the running exe) and relaunches.
//
// Same worker/cmdQ/Poll skeleton as AtlasUpdater (atlas_update.h). The local
// app version truth is always the compile-time POBTOOLS_VERSION_STRING —
// update_state.json is informational only, so a corrupt or tampered state
// file can never cause a downgrade.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

enum class AppUpdatePhase {
	Idle,            // nothing to report
	Checking,        // querying releases/latest
	UpToDate,        // check finished: this build is current
	TransUpdating,   // translation pack: download + verify + apply (automatic)
	TransDone,       // translation pack applied; informational notice
	AppAvailable,    // newer app release; waiting for the user to click update
	AppDownloading,  // fetching the full app zip
	AppStaging,      // extract + validate into the staging dir
	AppReadyToApply, // stage ready; main thread must swap + relaunch
	Error,
};

class AppUpdater {
public:
	~AppUpdater() { Shutdown(); }          // stack-friendly: never leaves a joinable worker
	void Init(const std::wstring& exeDir); // loads PobTools\update_state.json, starts the worker
	void Shutdown();                       // cancels any transfer and joins the worker

	// Main thread. Queues a version check; without force it is throttled to
	// once per day (persisted in update_state.json).
	void RequestCheck(bool force);

	// Main thread. Valid in AppAvailable/Error: downloads + stages the app zip.
	void StartAppUpdate();

	struct Status {
		AppUpdatePhase phase = AppUpdatePhase::Idle;
		std::string localVer;               // compile-time version (constant)
		std::string latestVer;              // remote version, set once known
		std::string message;                // UTF-8 progress / result / error text
		unsigned long long bytesDone = 0, bytesTotal = 0; // app download progress
		bool applyPending = false;          // AppReadyToApply: stageDir is valid
		std::wstring stageDir;
	};
	Status Poll();     // main thread, each frame: snapshot
	void AckNotice();  // main thread: dismiss TransDone/UpToDate/Error back to Idle

private:
	friend int RunAppUpdateCli(const std::wstring& exeDir, bool checkOnly);
	friend int RunAppFetchTest(const std::wstring& exeDir);
	friend int RunAppUpdateSelfTest(const std::wstring& exeDir);

	enum class Cmd { Check, UpdateApp };
	struct RemoteRelease {
		std::string ver;                   // "0.2.0" (tag without the leading v)
		std::string appUrl, appSha;        // browser_download_url + sha256 hex (may be empty)
		std::string transUrl, transSha;
		bool hasApp = false, hasTrans = false;
	};

	void workerLoop();
	bool doCheck(std::string* err);            // worker
	bool doUpdateTranslations(std::string* err); // worker (invoked from doCheck)
	bool doUpdateApp(std::string* err);        // worker
	bool downloadAsset(const std::string& url, const std::string& sha256hex,
	                   std::vector<unsigned char>* out, std::string* err, bool reportBytes);
	void loadState();
	void saveState();
	void setPhase(AppUpdatePhase p, const std::string& msg);

	std::wstring exeDir_;
	std::thread worker_;
	std::atomic<bool> stop_{ false };

	std::mutex cmdMx_;
	std::condition_variable cmdCv_;
	std::deque<Cmd> cmdQ_;

	std::mutex stMx_;
	Status st_;

	// state record (worker-thread only after Init)
	std::string appliedTrans_, appliedApp_, latestSeen_;
	std::atomic<long long> lastCheckUtc_{ 0 };
	RemoteRelease latest_;
};

// Two-tier policy decision, exposed for the self test. remote/localApp are
// parsed semvers; localTrans is the effective local translation version
// (max of update_state.json's appliedTranslations and the app version).
struct AppUpdateDecision { bool applyTransNow = false; bool promptApp = false; };
AppUpdateDecision ClassifyAppUpdate(std::tuple<int, int, int> remote,
                                    std::tuple<int, int, int> localApp,
                                    std::tuple<int, int, int> localTrans);

// Main thread, launcher window already closed. Swaps the staged install into
// exeDir (content files two-pass .new/rename; exe + engine DLLs backed up to
// *.old first, rolled back on any failure) and optionally spawns the new exe.
// Returns 0 on success; fills *errOut otherwise. tag is recorded in
// update_state.json (informational).
int ApplyStagedAppUpdateAndRelaunch(const std::wstring& exeDir, const std::wstring& stageDir,
                                    const std::string& tag, bool relaunch, std::string* errOut);

// Every-start best-effort cleanup: deletes *.old leftovers and the download
// cache from a previous swap. Safe to call unconditionally.
void CleanupAppUpdateLeftovers(const std::wstring& exeDir);

// "pob-zh.exe --app-update" (checkOnly=false) / "--app-update-check" (true):
// headless check (+ translation catch-up + app stage/swap, without relaunch).
// Ignores the daily throttle. Prints and logs to PobTools\app_update_log.txt.
int RunAppUpdateCli(const std::wstring& exeDir, bool checkOnly);

// "pob-zh.exe --app-fetch-test": one-time redirect verification — downloads
// the latest Translations asset (github.com 302s to
// objects.githubusercontent.com) and reports the sha256 result. Applies nothing.
int RunAppFetchTest(const std::wstring& exeDir);

// "pob-zh.exe --app-update-selftest": offline tests (zip extraction incl.
// backslash entries and zip-slip, sha256 vector, policy matrix, state I/O,
// staged swap + rollback). Report: PobTools\app_update_selftest.txt; 0 = all pass.
int RunAppUpdateSelfTest(const std::wstring& exeDir);

// Concatenation of every CJK literal the updater can surface in
// Status.message; the launcher feeds it to the glyph-range builder so
// dynamic updater messages never render as tofu.
extern const char* kAppUpdateGlyphSeed;
