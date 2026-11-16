// PobTools: moving over from another POB launcher.
//   1. import the builds an older tool (PoeCharm3-style: its exe beside the
//      stock POB folder) keeps inside its POB into ours
//   2. download the stock portable POB when none is installed
// Pure file / parsing logic here; the launcher drives it from a worker thread.
#pragma once

#include <atomic>
#include <functional>
#include <string>

// ---- build import ---------------------------------------------------------

// The folder a POB install really reads builds from: the buildPath attribute of
// <Misc> in <pobDir>\Settings.xml when present and non-empty, else
// <pobDir>\Builds. No trailing separator; `pobDir` has none either. The folder
// need not exist.
std::wstring PobBuildDir(const std::wstring& pobDir);

struct BuildCopyStats {
	int copied = 0;   // files written under their own name
	int renamed = 0;  // files written under a suffixed name (name clash, other content)
	int skipped = 0;  // files already there with identical content
	int failed = 0;   // files that could not be read or written
};

// Copies every file and subfolder of `srcDir` into `dstDir` (created when
// missing), keeping the relative layout. Never overwrites and never touches the
// source:
//   * same relative path, identical bytes  -> skipped
//   * same relative path, different bytes  -> written beside it as
//     "<stem> (匯入)<ext>", then "<stem> (匯入 2)<ext>", "(匯入 3)"... -- the
//     first of those names that is free, or skipped when one of them already
//     holds identical bytes
//   * a folder that exists on both sides is merged, never renamed
BuildCopyStats ImportBuildTree(const std::wstring& srcDir, const std::wstring& dstDir);

struct BuildImportResult {
	bool sourceFound = false; // the picked folder holds at least one POB install (DetectInstalls rules)
	bool needPoe1 = false;    // the source has a PoE1 POB but we have none: nothing imported for it
	bool needPoe2 = false;    // same for PoE2
	BuildCopyStats poe1, poe2;
};

// `srcToolDir`: the old tool's folder (its exe beside the POB folders);
// `ourExeDir`: ours. Both are scanned with DetectInstalls; each generation's
// PobBuildDir tree is imported into the same generation of ours with
// ImportBuildTree. A source install without a build folder contributes nothing.
// Trailing backslash on either argument is optional.
BuildImportResult ImportBuildsFromTool(const std::wstring& srcToolDir, const std::wstring& ourExeDir);

// ---- stock POB download ---------------------------------------------------

// "PathOfBuildingCommunity-Portable" / "PathOfBuildingCommunity-PoE2-Portable":
// the install folder beside the exe, and (plus ".zip") the ONLY release asset
// name accepted.
const wchar_t* PobPortableDirName(bool poe2);

struct PobRelease {
	std::string tag;      // "v2.67.2"
	std::string url;      // browser_download_url of the portable zip
	std::string sha256;   // lower-case hex, from the asset's "digest": "sha256:<hex>"
	unsigned long long size = 0;
};

// Parses a GitHub "releases/latest" JSON body. Succeeds only when an asset is
// named exactly <PobPortableDirName(poe2)>.zip and carries a url and a sha256
// digest; anything else (other names such as the Setup exe, a missing digest,
// malformed JSON) fails with *err set.
bool ParsePobRelease(const std::string& json, bool poe2, PobRelease& out, std::string* err);

// Installs a downloaded portable zip as <exeDir>\<PobPortableDirName(poe2)>:
//   * refuses when that folder already exists (it is left untouched)
//   * refuses when the bytes' sha256 is not `sha256Hex` (case-insensitive; empty = refuse)
//   * the archive may hold the POB files at its root or inside one top-level
//     folder; either way Launch.lua ends up directly in the install folder
//   * an archive without Launch.lua is refused
// On any failure nothing new is left in exeDir (no install folder, no staging
// leftovers). `exeDir` may or may not end with a backslash.
bool InstallPobZip(const void* zip, size_t size, const std::string& sha256Hex,
                   const std::wstring& exeDir, bool poe2, std::string* err);

// Fetches releases/latest, downloads the portable zip and installs it. Blocking;
// call from a worker thread. `progress(got, total)` runs on that thread.
bool DownloadAndInstallPob(const std::wstring& exeDir, bool poe2, const std::atomic<bool>* cancel,
                           const std::function<void(unsigned long long, unsigned long long)>& progress,
                           std::string* err);

// --migrate-selftest: report at <exeDir>PobTools\migrate_selftest.txt, returns
// the failure count.
int RunMigrateSelfTest(const std::wstring& exeDir);
