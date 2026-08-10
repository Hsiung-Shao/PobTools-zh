// PobTools application self-updater.
//
// TWO INDEPENDENT LINES, because the程式 and the翻譯資料 are maintained by
// different people on different schedules (v0.19.0 onwards):
//
//   App 線   — GET releases/latest, semver tags "v0.19.0". The asset taken is
//       PobTools-update-<ver>.zip: the full app WITHOUT the dictionaries. The
//       launcher shows an update prompt; on click it is downloaded, verified,
//       extracted to a staging dir, and the main thread swaps the install
//       (rename-trick for the running exe) and relaunches. EVERY version bump
//       prompts — the old "patch bump = silent data-only release" rule is gone,
//       because applying anything now means renaming pob-zh.exe and engine\*
//       and restarting the process, which must never happen unasked.
//
//   Data 線  — GET /releases?per_page=100 and pick the highest "data-<n>"
//       sequence. Those releases are ALWAYS marked prerelease, so they can
//       never occupy releases/latest — an老客戶端 that fetched "data-7" would
//       fail parse_semver and go permanently silent. The asset is
//       PobTools-Data-<n>.zip, dictionaries only, applied to Data\ in place
//       (engine picks it up on next start / F3 reload). This line is allowed
//       to fail: no translations update is not a reason to stop shipping
//       program updates.
//
// The local truths are likewise two: the app version is always the
// compile-time POBTOOLS_VERSION_STRING, and the data version is the stamp
// inside Data\translations_version.json (which travels INSIDE both the full
// zip and the data pack, so unzipping a pack by hand also moves the stamp).
// PobTools\update_state.json is informational only — a corrupt or tampered
// state file can never cause a downgrade or a silent redownload loop.
//
// Same worker/cmdQ/Poll skeleton as AtlasUpdater (atlas_update.h).
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
	TransAvailable,  // new translation data exists but automatic updates are off
	                 // -- reporting "up to date" here would be a lie, and the user
	                 // needs somewhere to apply it from
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

	// Main thread. Whether an update may replace the translation dictionaries.
	// Off is for someone editing translations themselves: the dictionaries are
	// replaced wholesale, so an automatic patch-level update silently discards
	// every edit made through the translation editor. Gating the BUTTON is not
	// enough -- the worker applies translation packs on its own schedule with
	// nobody pressing anything, so the gate has to be here.
	void SetTranslationUpdates(bool on) { transUpdates_.store(on); }
	bool TranslationUpdatesEnabled() const { return transUpdates_.load(); }

	// Main thread. Valid in TransAvailable: apply the pending translation pack
	// once, regardless of the setting above. Turning the setting off must not mean
	// "you can never have new translations again".
	void StartTranslationUpdate();

	struct Status {
		// ONE phase for two lines, App 線 winning. Splitting it would mean two
		// widgets in a header that has room for one; the precedence is safe
		// because the Data 線 either applied silently (nothing to show) or is
		// re-offered on the next check, whereas a missed app prompt is a user
		// stuck on an old build. See doCheck for where the two meet.
		AppUpdatePhase phase = AppUpdatePhase::Idle;
		std::string localVer;               // compile-time app version (constant)
		std::string latestAppVer;           // remote app version "0.19.0", once known
		std::string localDataVer;           // Data\translations_version.json ("data-3"), "" = unstamped
		std::string latestDataVer;          // remote data tag "data-3", once known
		std::string message;                // UTF-8 progress / result / error text
		unsigned long long bytesDone = 0, bytesTotal = 0; // app download progress
		bool applyPending = false;          // AppReadyToApply: stageDir is valid
		std::wstring stageDir;
	};
	Status Poll();     // main thread, each frame: snapshot
	void AckNotice();  // main thread: dismiss TransDone/UpToDate/Error back to Idle

	// Main thread. Suspends all update work while a POB is running: applying an
	// app update renames pob-zh.exe and engine\* out of the way, and the same
	// check also downloads translation packs straight over Data\*.json. Disabling
	// the button is not enough — the worker does the translation update on its
	// own schedule, with nobody pressing anything.
	void SetHold(bool on) { hold_.store(on); }
	bool IsHeld() const { return hold_.load(); }

private:
	friend int RunAppUpdateCli(const std::wstring& exeDir, bool checkOnly);
	friend int RunAppFetchTest(const std::wstring& exeDir);
	friend int RunAppUpdateSelfTest(const std::wstring& exeDir);

	enum class Cmd { Check, UpdateApp, UpdateTranslations };
	// The two lines never share a field. They used to (a single `ver`), and that
	// single field fed the UI label, the apply tag written to update_state.json
	// and the download cache directory at once — so a data tag arriving second
	// would have been recorded as the installed app version.
	struct RemoteRelease {
		// App 線 (releases/latest)
		std::string appVer;                // "0.19.0" (tag without the leading v)
		std::string appUrl, appSha;        // browser_download_url + sha256 hex (may be empty)
		bool hasApp = false;
		// Data 線 (highest data-<n> among /releases)
		std::string dataTag;               // "data-3" verbatim — NOT a semver
		long long   dataSeq = -1;          // 3; -1 = none found
		std::string dataUrl, dataSha;
		bool hasData = false;
	};

	void workerLoop();
	bool doCheck(std::string* err);            // worker
	bool fetchAppRelease(RemoteRelease* rel, std::string* err);  // worker: releases/latest
	bool fetchDataRelease(RemoteRelease* rel, std::string* err); // worker: /releases scan
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
	std::atomic<bool> hold_{ false };           // see SetHold
	std::atomic<bool> transUpdates_{ true };    // see SetTranslationUpdates

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

// What the two lines decided, as one pure function so the self test can drive
// every combination — including "both at once", which is the case the old
// if/else-if silently dropped. remoteApp/localApp are parsed semvers;
// remoteDataSeq/localDataSeq are the "data-<n>" sequence numbers (-1 = none).
// autoData is the 自動更新翻譯資料 setting.
struct UpdatePlan {
	bool promptApp = false;    // newer app release exists and has an update asset
	bool applyDataNow = false; // newer data release + the setting says yes
	bool offerData = false;    // newer data release + the setting says no
};
UpdatePlan PlanUpdates(bool hasAppAsset, std::tuple<int, int, int> remoteApp,
                       std::tuple<int, int, int> localApp,
                       bool hasDataAsset, long long remoteDataSeq, long long localDataSeq,
                       bool autoData);

// "data-3" -> 3. False for anything else, including "v0.19.0" and "data-" — the
// Data 線 must never mistake an app tag for a data tag.
bool ParseDataTagSeq(const std::string& tag, long long* seq);

// Is `rel` -- a path relative to the install root, backslash separated -- part of
// the translation data? This is THE boundary between the two release lines: what
// this returns true for lives in PobTools-Data-<n>.zip and nowhere else, what it
// returns false for lives in PobTools-update-<ver>.zip and nowhere else.
//
// ⚠ THE SAFETY DIRECTION FLIPPED IN v0.19.0. While the app zip still carried the
// dictionaries, forgetting to list a data file here merely meant it kept being
// updated. Now a data file this function does not recognise is packed into the
// APP zip, and every app update overwrites the translator's copy of it. So the
// packaging script asserts coverage in both directions instead of trusting this
// list: --translation-data-list walks a staged tree through this very function
// (never a hardcoded list, so a translator adding Data\poe1\ko\ is covered for
// free), and packaging additionally asserts that every single file under
// Data\{poe1,poe2,launcher}\ came back true.
bool IsTranslationDataRel(const std::wstring& rel);

// The installed translation-data stamp: the "data-<n>" tag inside
// <exeDir>Data\translations_version.json, or "" when the file is absent or
// unreadable. The stamp travels inside the packs themselves rather than being
// recorded in update_state.json, for three reasons: a fresh install has no
// update_state.json at all (PobTools\ is excluded from the zips) and would
// otherwise redownload the identical 4.4MB on first launch; the stamp and the
// content are swapped by the same apply_content_two_pass, so "content moved but
// state did not" cannot happen; and a translator who unzips a pack by hand — the
// real workflow once the data is externally maintained — moves the stamp too.
std::string ReadLocalDataVersion(const std::wstring& exeDir);

// Main thread, launcher window already closed. Swaps the staged install into
// exeDir (content files two-pass .new/rename; exe + engine DLLs backed up to
// *.old first, rolled back on any failure) and optionally spawns the new exe.
// Returns 0 on success; fills *errOut otherwise. tag is recorded in
// update_state.json (informational).
//
// includeTranslations=false leaves every IsTranslationDataRel file alone, so an
// app update can be taken without losing edited dictionaries. The exe and
// engine\* are swapped either way -- only the dictionaries are held back.
//
// ⚠ Since v0.19.0 the app pack carries no dictionaries at all, so on the normal
// path this gate skips nothing. It is KEPT because it is the last line of
// defence for the two cases where the pack does contain them: someone pointed at
// the full PobTools-<ver>.zip (a rollback, or a hand-run --app-update against an
// older release), and a packaging bug that let a dictionary leak into the app
// zip. The self test asserts both directions on real files for that reason.
int ApplyStagedAppUpdateAndRelaunch(const std::wstring& exeDir, const std::wstring& stageDir,
                                    const std::string& tag, bool relaunch, std::string* errOut,
                                    bool includeTranslations = true);

// Every-start best-effort cleanup: deletes *.old leftovers and the download
// cache from a previous swap. Safe to call unconditionally.
void CleanupAppUpdateLeftovers(const std::wstring& exeDir);

// "pob-zh.exe --app-update" (checkOnly=false) / "--app-update-check" (true):
// headless check (+ translation catch-up + app stage/swap, without relaunch).
// Ignores the daily throttle. Prints and logs to PobTools\app_update_log.txt.
int RunAppUpdateCli(const std::wstring& exeDir, bool checkOnly);

// "pob-zh.exe --app-fetch-test": one-time redirect verification — downloads
// the latest data pack (github.com 302s to objects.githubusercontent.com) and
// reports the sha256 result. Applies nothing.
int RunAppFetchTest(const std::wstring& exeDir);

// "pob-zh.exe --translation-data-list <dir> [outFile]": maintainer/packaging.
// Walks <dir> and prints every relative path IsTranslationDataRel() accepts, one
// per line, backslash separated, sorted. With outFile it also writes the same
// text as BOM-less UTF-8 with LF endings — PowerShell 5.1 decodes a native
// program's stdout through the console code page (Big5 here), so the file is the
// only lossless channel for a path that is not pure ASCII.
//
// Walking rather than printing a fixed list is the whole point: it is the same
// predicate the updater uses, applied to the tree actually being packaged, so a
// translator who adds Data\poe1\ko\ needs no packaging change. Exit 1 when the
// directory is missing or nothing matched (an empty data pack must not be able
// to sail through packaging as a success).
int RunTranslationDataList(const std::wstring& dir, const std::wstring& outFile);

// "pob-zh.exe --app-update-selftest": offline tests (zip extraction incl.
// backslash entries and zip-slip, sha256 vector, policy matrix, state I/O,
// staged swap + rollback). Report: PobTools\app_update_selftest.txt; 0 = all pass.
int RunAppUpdateSelfTest(const std::wstring& exeDir);

// Concatenation of every CJK literal the updater can surface in
// Status.message; the launcher feeds it to the glyph-range builder so
// dynamic updater messages never render as tofu.
//
// GENERATED from the PT_UPD_MSGS list in app_update.cpp -- do not hand-edit, and
// do not add a message by typing a literal at the setPhase call site. Two checks
// hold this up and BOTH are needed:
//   --font-coverage-selftest  : can every shipped font draw what is in the seed?
//   --app-update-selftest T13 : is everything the updater says in the seed?
// Only the first existed until v0.19.0, which is why phrases that were never
// seeded at all (v0.18.0 shipped two) sailed past a green selftest -- the
// characters it would have objected to were never handed to it.
extern const char* kAppUpdateGlyphSeed;
