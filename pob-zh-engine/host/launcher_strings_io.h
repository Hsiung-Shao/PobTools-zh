// Launcher UI strings as editable translation data. The strings in launcher_strings.h are compiled into the exe;
// they also ship as Data\launcher\<locale>\launcher.json, laid out EXACTLY like Data\poe1\zh-rTW\ (meta.json with a
// load_order plus one dictionary file), so the external-data-path override, the app/translation-data update split and
// the translation editor handle it with no special case.
// The JSON is an OVERLAY, never the source of truth: whatever it does not define keeps the compiled string, so a
// stale, partial or corrupt file never blanks a button and a NEW build's text still renders with data updates off.
#pragma once

#include <deque>
#include <string>

#include "launcher_strings.h"

struct LauncherStringStore {
	LauncherStrings s{};             // fields point into the compiled table or into `owned`
	// deque, not vector: adding an element must never move the others (`s` holds raw pointers into them).
	std::deque<std::string> owned;
	int overridden = 0;              // how many fields the JSON replaced
	bool fileFound = false;

	// Copying would duplicate `owned` while `s` kept pointing at the ORIGINAL strings (dangling once it dies).
	// Moving is safe (deque move preserves element addresses).
	LauncherStringStore() = default;
	LauncherStringStore(const LauncherStringStore&) = delete;
	LauncherStringStore& operator=(const LauncherStringStore&) = delete;
	LauncherStringStore(LauncherStringStore&&) = default;
	LauncherStringStore& operator=(LauncherStringStore&&) = default;
};

// Compiled defaults with <slotRoot><locale>\launcher.json layered on top.
// slotRoot ends with a backslash (<exeDir>Data\launcher\ or the folder configured
// for DictSlot::Launcher). Keys are the ENGLISH strings, like the POB dictionaries.
LauncherStringStore LoadLauncherStrings(const std::wstring& slotRoot, const std::wstring& locale);

// Maintainer CLI (--launcher-strings-export): regenerate
// <exeDir>Data\launcher\<locale>\{meta.json,launcher.json} from the compiled
// tables. Run before packaging so the shipped JSON cannot drift from the binary.
// Returns 0 on success.
int RunLauncherStringsExport(const std::wstring& exeDir, const std::wstring& locale = L"zh-rTW");

// Headless checks (--launcher-strings-selftest): English keys pairwise unique,
// overlay/fallback semantics, pointer lifetime across many overrides, export
// round-trip. Report at <exeDir>PobTools\launcher_strings_selftest.txt; 0 = pass.
int RunLauncherStringsSelfTest(const std::wstring& exeDir);
